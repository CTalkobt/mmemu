#include "test_harness.h"
#include "plugins/6502/main/cpu6502.h"
#include "plugins/6502/main/cpu65c02.h"
#include "libmem/main/memory_bus.h"
#include <nlohmann/json.hpp>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

// Whole-instruction-set tests for the 6502 and 65C02 cores, run when the
// reference suites are present (they are not part of the repository):
//
//  - Klaus Dormann's functional tests
//    (github.com/Klaus2m5/6502_65C02_functional_tests, bin_files/):
//    MMEMU_DORMANN_DIR, default ../refs/6502_65C02_functional_tests/bin_files
//  - SingleStepTests/65x02 (github.com/SingleStepTests/65x02), one JSON
//    file per opcode under 6502/ and wdc65c02/: MMEMU_SST_DIR, default
//    ../refs/65x02-sample. Each test sets the registers and RAM, executes one
//    instruction and checks registers, RAM and the cycle count.

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

std::string refDir(const char* env, const char* fallback) {
    const char* v = std::getenv(env);
    return v ? v : fallback;
}

// Run a Dormann image: load at $0000, start at $0400, stop when the PC loops
// on itself (JMP * is the test's trap); return the trap address.
uint16_t runDormann(MOS6502& cpu, const std::string& path, long maxSteps) {
    FlatMemoryBus bus("system", 16);
    std::ifstream f(path, std::ios::binary);
    std::vector<char> img((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    for (size_t i = 0; i < img.size() && i < 0x10000; i++) bus.write8((uint32_t)i, (uint8_t)img[i]);
    cpu.setDataBus(&bus);
    cpu.setCodeBus(&bus);
    cpu.reset();
    cpu.setPc(0x0400);
    uint32_t last = 0xFFFFFFFF;
    for (long s = 0; s < maxSteps; s++) {
        uint32_t pc = cpu.pc();
        if (pc == last) break;
        last = pc;
        cpu.step();
    }
    cpu.setDataBus(nullptr);
    cpu.setCodeBus(nullptr);
    return (uint16_t)cpu.pc();
}

// NMOS opcodes whose results vary between chips (XAA/LXA's "magic" constant,
// SHA/TAS/SHY/SHX's page-crossing store): the core models the common
// behaviour SingleStepTests records, so they are tested like the others.
bool unstableNmos(int) { return false; }

bool jamNmos(int op) {
    switch (op) {
        case 0x02: case 0x12: case 0x22: case 0x32: case 0x42: case 0x52:
        case 0x62: case 0x72: case 0x92: case 0xB2: case 0xD2: case 0xF2:
            return true;
        default: return false;
    }
}

struct Failures {
    int tests = 0, regs = 0, ram = 0, cycles = 0;
    std::string first;
};

// Run one opcode file; P is compared without B and the unused bit (they are
// not flags in the register).
Failures runSstFile(MOS6502& cpu, const std::string& path) {
    Failures f;
    std::ifstream in(path);
    json tests = json::parse(in, nullptr, false);
    if (!tests.is_array()) return f;
    FlatMemoryBus bus("system", 16);
    cpu.setDataBus(&bus);
    cpu.setCodeBus(&bus);
    for (const auto& t : tests) {
        const auto& ini = t["initial"];
        const auto& fin = t["final"];
        for (const auto& r : ini["ram"]) bus.write8(r[0].get<uint32_t>(), r[1].get<uint8_t>());
        cpu.reset();
        cpu.regWrite(0, ini["a"].get<uint32_t>());
        cpu.regWrite(1, ini["x"].get<uint32_t>());
        cpu.regWrite(2, ini["y"].get<uint32_t>());
        cpu.regWrite(3, ini["s"].get<uint32_t>());
        cpu.regWrite(4, ini["pc"].get<uint32_t>());
        cpu.regWrite(5, ini["p"].get<uint32_t>());
        int cyc = cpu.step();
        f.tests++;
        bool regsOk = cpu.regRead(0) == fin["a"].get<uint32_t>() && cpu.regRead(1) == fin["x"].get<uint32_t>() &&
                      cpu.regRead(2) == fin["y"].get<uint32_t>() && cpu.regRead(3) == fin["s"].get<uint32_t>() &&
                      cpu.regRead(4) == fin["pc"].get<uint32_t>() &&
                      ((cpu.regRead(5) ^ fin["p"].get<uint32_t>()) & 0xCF) == 0;
        bool ramOk = true;
        for (const auto& r : fin["ram"])
            if (bus.peek8(r[0].get<uint32_t>()) != r[1].get<uint8_t>()) { ramOk = false; break; }
        bool cycOk = cyc == (int)t["cycles"].size();
        if (!regsOk) f.regs++;
        if (!ramOk) f.ram++;
        if (!cycOk) f.cycles++;
        if ((!regsOk || !ramOk || !cycOk) && f.first.empty()) {
            char buf[256];
            snprintf(buf, sizeof buf, "%s: A=%02X X=%02X Y=%02X S=%02X PC=%04X P=%02X cyc=%d, want A=%02X X=%02X Y=%02X S=%02X PC=%04X P=%02X cyc=%d%s",
                     t["name"].get<std::string>().c_str(), cpu.regRead(0), cpu.regRead(1), cpu.regRead(2), cpu.regRead(3),
                     cpu.regRead(4), cpu.regRead(5), cyc, fin["a"].get<uint32_t>(), fin["x"].get<uint32_t>(),
                     fin["y"].get<uint32_t>(), fin["s"].get<uint32_t>(), fin["pc"].get<uint32_t>(),
                     fin["p"].get<uint32_t>(), (int)t["cycles"].size(), ramOk ? "" : " (RAM differs)");
            f.first = buf;
        }
    }
    cpu.setDataBus(nullptr);
    cpu.setCodeBus(nullptr);
    return f;
}

// Run all opcode files of a variant; print a line per opcode that failed.
// Returns the number of failing opcodes that are not excluded.
int runSstVariant(const std::string& dir, bool cmos, bool checkCycles) {
    int bad = 0, files = 0;
    long total = 0;
    for (int op = 0; op < 256; op++) {
        char name[16];
        snprintf(name, sizeof name, "%02x.json", op);
        std::string path = dir + "/" + name;
        if (!fs::exists(path)) continue;
        if (!cmos && jamNmos(op)) continue;             // JAM: the CPU locks up
        std::unique_ptr<MOS6502> cpu(cmos ? new MOS65C02() : new MOS6502());
        Failures f = runSstFile(*cpu, path);
        files++;
        total += f.tests;
        bool failed = f.regs || f.ram || (checkCycles && f.cycles);
        if (!failed && !f.cycles) continue;
        bool excused = !cmos && unstableNmos(op);
        printf("  %s $%02X: %d/%d regs, %d ram, %d cycles%s\n      first: %s\n", cmos ? "65C02" : "6502", op,
               f.regs, f.tests, f.ram, f.cycles, excused ? " (unstable opcode, not counted)" : "", f.first.c_str());
        if (failed && !excused) bad++;
    }
    printf("  %s: %d opcode files, %ld tests, %d opcodes failing\n", cmos ? "65C02" : "6502", files, total, bad);
    return files ? bad : -1;
}

} // namespace

TEST_CASE(dormann_6502_functional_test) {
    std::string dir = refDir("MMEMU_DORMANN_DIR", "../refs/6502_65C02_functional_tests/bin_files");
    std::string path = dir + "/6502_functional_test.bin";
    if (!fs::exists(path)) { printf("  (skipped: %s not found)\n", path.c_str()); return; }
    MOS6502 cpu;
    uint16_t trap = runDormann(cpu, path, 200000000);
    if (trap != 0x3469) printf("  trapped at $%04X (see 6502_functional_test.lst)\n", trap);
    ASSERT_EQ((int)trap, 0x3469);
}

TEST_CASE(dormann_65c02_functional_and_extended_tests) {
    std::string dir = refDir("MMEMU_DORMANN_DIR", "../refs/6502_65C02_functional_tests/bin_files");
    std::string base = dir + "/6502_functional_test.bin";
    std::string ext = dir + "/65C02_extended_opcodes_test.bin";
    if (!fs::exists(base) || !fs::exists(ext)) { printf("  (skipped: %s not found)\n", ext.c_str()); return; }
    MOS65C02 cpu1;
    uint16_t trap = runDormann(cpu1, base, 200000000);   // the documented set on the 65C02
    if (trap != 0x3469) printf("  6502 test on the 65C02 trapped at $%04X\n", trap);
    ASSERT_EQ((int)trap, 0x3469);
    MOS65C02 cpu2;
    trap = runDormann(cpu2, ext, 200000000);
    if (trap != 0x24F1) printf("  65C02 extended test trapped at $%04X (see 65C02_extended_opcodes_test.lst)\n", trap);
    ASSERT_EQ((int)trap, 0x24F1);
}

TEST_CASE(singlestep_6502_all_opcodes) {
    std::string dir = refDir("MMEMU_SST_DIR", "../refs/65x02-sample") + "/6502";
    if (!fs::is_directory(dir)) { printf("  (skipped: %s not found)\n", dir.c_str()); return; }
    int bad = runSstVariant(dir, false, true);
    ASSERT_EQ(bad, 0);
}

TEST_CASE(singlestep_65c02_all_opcodes) {
    std::string dir = refDir("MMEMU_SST_DIR", "../refs/65x02-sample") + "/wdc65c02";
    if (!fs::is_directory(dir)) { printf("  (skipped: %s not found)\n", dir.c_str()); return; }
    int bad = runSstVariant(dir, true, true);
    ASSERT_EQ(bad, 0);
}
