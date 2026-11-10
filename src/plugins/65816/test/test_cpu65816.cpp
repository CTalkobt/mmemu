// WDC 65C816 core (#12) against Tom Harte's SingleStepTests
// (github.com/SingleStepTests/65816, MIT licence): for each opcode, in
// native and emulation mode, a single instruction from a given state; the
// final registers, memory and the number of bus cycles must match.
//
// The tests (3 GB) are not in this repository: they are read from
// ../refs/ss65816/v1 (or $MMEMU_65816_TESTS) when present, else the test
// is skipped. By default the first 200 tests of each file run; set
// MMEMU_65816_FULL=1 for all of them.

#include "test_harness.h"
#include "plugins/65816/main/cpu65816.h"
#include "libmem/main/ibus.h"
#include <nlohmann/json.hpp>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
// 16 MB of RAM, sparse: what a test sets, the rest zero
class SparseRam : public IBus {
public:
    SparseRam() { m_cfg = {24, 8, BusRole::DATA, true, 0xFFFFFF}; }
    std::unordered_map<uint32_t, uint8_t> mem;
    struct Access { uint32_t addr; uint8_t value; bool write; };
    std::vector<Access> log;
    const BusConfig& config() const override { return m_cfg; }
    const char* name() const override { return "ram"; }
    uint8_t peek8(uint32_t a) override { auto it = mem.find(a & 0xFFFFFF); return it == mem.end() ? 0 : it->second; }
    uint8_t read8(uint32_t a) override { const uint8_t v = peek8(a); log.push_back({a & 0xFFFFFF, v, false}); return v; }
    void write8(uint32_t a, uint8_t v) override { log.push_back({a & 0xFFFFFF, v, true}); mem[a & 0xFFFFFF] = v; }
    size_t stateSize() const override { return 0; }
    void saveState(uint8_t*) const override {}
    void loadState(const uint8_t*) override {}
    int  writeCount() const override { return 0; }
    void getWrites(uint32_t*, uint8_t*, uint8_t*, int) const override {}
    void clearWriteLog() override {}
private:
    BusConfig m_cfg;
};

std::string testsDir() {
    if (const char* d = std::getenv("MMEMU_65816_TESTS")) return d;
    return "../refs/ss65816/v1";
}
}  // namespace

TEST_CASE(w65c816_single_step_tests) {
    const std::string dir = testsDir();
    {
        std::ifstream probe(dir + "/a9.n.json");
        if (!probe) { printf("  (skipped: no SingleStepTests at %s)\n", dir.c_str()); return; }
    }
    const bool full = std::getenv("MMEMU_65816_FULL") != nullptr;
    const size_t perFile = full ? SIZE_MAX : 200;
    SparseRam ram;
    W65C816 cpu;
    cpu.setDataBus(&ram);
    int total = 0, failed = 0;
    std::map<std::string, int> failsByFile;
    std::string firstFail;
    char name[16];
    for (int op = 0; op < 256; ++op) {
        for (const char* m : {"n", "e"}) {
            std::snprintf(name, sizeof name, "%02x.%s.json", op, m);
            std::ifstream f(dir + "/" + name, std::ios::binary);
            if (!f) continue;
            nlohmann::json tests = nlohmann::json::array();
            if (full) {
                try { f >> tests; } catch (...) { printf("  bad JSON %s\n", name); continue; }
            } else {
                // The first tests only: whole objects from the start of the file
                std::string buf(400000, '\0');
                f.read(&buf[0], (std::streamsize)buf.size());
                buf.resize((size_t)f.gcount());
                int depth = 0;
                size_t start = 0;
                for (size_t i = 0; i < buf.size() && tests.size() < perFile; ++i) {
                    if (buf[i] == '{') { if (depth++ == 0) start = i; }
                    else if (buf[i] == '}' && --depth == 0) tests.push_back(nlohmann::json::parse(buf.substr(start, i - start + 1)));
                }
            }
            size_t n = 0;
            for (const auto& t : tests) {
                if (n++ >= perFile) break;
                const auto& in = t["initial"];
                ram.mem.clear();
                for (const auto& kv : in["ram"]) ram.mem[kv[0].get<uint32_t>()] = (uint8_t)kv[1].get<int>();
                cpu.regWrite(W65C816::R_E, 0);                       // native, then the test's state
                cpu.regWrite(W65C816::R_P, in["p"].get<int>());
                cpu.regWrite(W65C816::R_E, in["e"].get<int>());
                cpu.regWrite(W65C816::R_A, in["a"].get<int>());
                cpu.regWrite(W65C816::R_X, in["x"].get<int>());
                cpu.regWrite(W65C816::R_Y, in["y"].get<int>());
                cpu.regWrite(W65C816::R_S, in["s"].get<int>());
                cpu.regWrite(W65C816::R_D, in["d"].get<int>());
                cpu.regWrite(W65C816::R_DBR, in["dbr"].get<int>());
                cpu.regWrite(W65C816::R_PBR, in["pbr"].get<int>());
                cpu.regWrite(W65C816::R_PC, in["pc"].get<int>());
                cpu.setIrqLine(false);
                ram.log.clear();
                int cycles = cpu.step();
                // MVN/MVP move a byte a pass: the tests take the whole move as one instruction
                if (op == 0x44 || op == 0x54)
                    while (cpu.regRead(W65C816::R_PC) == in["pc"].get<uint32_t>() && cycles < 100000) cycles += cpu.step();
                const auto& out = t["final"];
                std::string why;
                auto chk = [&](const char* r, int idx, const char* key) {
                    const uint32_t got = cpu.regRead(idx), want = out[key].get<uint32_t>();
                    if (got != want) { char b[64]; std::snprintf(b, sizeof b, " %s %X/%X", r, got, want); why += b; }
                };
                chk("A", W65C816::R_A, "a"); chk("X", W65C816::R_X, "x"); chk("Y", W65C816::R_Y, "y");
                chk("S", W65C816::R_S, "s"); chk("D", W65C816::R_D, "d"); chk("DBR", W65C816::R_DBR, "dbr");
                chk("PBR", W65C816::R_PBR, "pbr"); chk("PC", W65C816::R_PC, "pc"); chk("P", W65C816::R_P, "p");
                chk("E", W65C816::R_E, "e");
                for (const auto& kv : out["ram"]) {
                    const uint32_t a = kv[0].get<uint32_t>();
                    const int want = kv[1].get<int>();
                    if (ram.peek8(a) != want) { char b[64]; std::snprintf(b, sizeof b, " [%06X] %02X/%02X", a, ram.peek8(a), want); why += b; break; }
                }
                const int wantCycles = (int)t["cycles"].size();
                // The bus accesses (cycles with a value: VDA or VPA active), in order
                if (op != 0x44 && op != 0x54) {
                    size_t k = 0;
                    bool same = true;
                    for (const auto& c : t["cycles"]) {
                        if (c[1].is_null()) continue;
                        const bool w = c[2].get<std::string>()[3] == 'w';
                        if (k >= ram.log.size() || ram.log[k].addr != c[0].get<uint32_t>() || ram.log[k].write != w ||
                            ram.log[k].value != c[1].get<int>()) { same = false; break; }
                        ++k;
                    }
                    if (same && k != ram.log.size()) same = false;
                    if (!same) why += " bus";
                }
                if (cycles != wantCycles) { char b[48]; std::snprintf(b, sizeof b, " cycles %d/%d", cycles, wantCycles); why += b; }
                ++total;
                if (!why.empty()) {
                    ++failed;
                    if (failsByFile[name]++ == 0 && failsByFile.size() <= 40)
                        printf("    %s %s:%s\n", name, t["name"].get<std::string>().c_str(), why.c_str());
                }
            }
        }
    }
    printf("    %d / %d SingleStepTests match (%s)\n", total - failed, total, full ? "all" : "200 per file");
    if (failed) printf("    %zu files with failures\n", failsByFile.size());
    EXPECT_EQ(failed, 0);
}

#include "libmem/main/memory_bus.h"

// Interrupts (not in SingleStepTests): emulation-mode IRQ (B clear on the
// stack, the $FFFE vector), native-mode IRQ and NMI (the bank pushed, the
// $FFEE / $FFEA vectors), WAI woken by a masked IRQ, BRK's B flag
TEST_CASE(w65c816_interrupts) {
    FlatMemoryBus mem("ram", 24);
    W65C816 cpu;
    cpu.setDataBus(&mem);
    auto vec = [&](uint16_t at, uint16_t to) { mem.write8(at, to & 0xFF); mem.write8(at + 1, to >> 8); };
    vec(0xFFFC, 0x0200);                     // reset
    vec(0xFFFE, 0x1000);                     // emulation IRQ/BRK
    vec(0xFFEE, 0x1100);                     // native IRQ
    vec(0xFFEA, 0x1200);                     // native NMI
    const uint8_t prog[] = {0x58,            // $0200 CLI
                            0xEA,            // NOP
                            0x18, 0xFB,      // CLC; XCE: native mode
                            0xEA, 0xEA,
                            0xCB,            // WAI
                            0xEA,
                            0x00, 0x00};     // BRK (native)
    for (size_t i = 0; i < sizeof prog; ++i) mem.write8(0x200 + (uint32_t)i, prog[i]);
    cpu.reset();
    EXPECT_EQ(cpu.pc(), 0x200u);
    EXPECT_TRUE(cpu.emulation());
    cpu.step();                              // CLI
    cpu.setIrqLine(true);
    EXPECT_EQ(cpu.step(), 7);                // emulation: 7 cycles
    EXPECT_EQ(cpu.pc(), 0x1000u);
    const uint16_t s = (uint16_t)cpu.regRead(W65C816::R_S);
    EXPECT_EQ(s, 0x1FCu);
    EXPECT_TRUE(!(mem.peek8(0x1FD) & W65C816::FX));   // B clear for a hardware interrupt
    EXPECT_EQ(mem.peek8(0x1FE), 0x01);       // PC $0201 (the NOP)
    EXPECT_TRUE(cpu.p() & W65C816::FI);
    cpu.setIrqLine(false);

    cpu.setPc(0x202);
    cpu.regWrite(W65C816::R_P, 0x30);        // I clear
    cpu.step(); cpu.step();                  // CLC, XCE
    EXPECT_TRUE(!cpu.emulation());
    cpu.regWrite(W65C816::R_PBR, 0);
    cpu.setIrqLine(true);
    EXPECT_EQ(cpu.step(), 8);                // native: 8 cycles, the bank pushed
    EXPECT_EQ(cpu.pc(), 0x1100u);
    cpu.setIrqLine(false);
    cpu.setNmiLine(true);
    cpu.step();
    EXPECT_EQ(cpu.pc(), 0x1200u);
    cpu.setNmiLine(false);

    cpu.setPc(0x206);                        // WAI with interrupts masked
    cpu.regWrite(W65C816::R_P, 0x04 | 0x30);
    cpu.step();
    EXPECT_TRUE(cpu.isHalted());
    cpu.step(); cpu.step();
    EXPECT_EQ(cpu.pc(), 0x207u);
    cpu.setIrqLine(true);                    // masked: wakes WAI, carries on
    cpu.step();
    EXPECT_TRUE(!cpu.isHalted());
    EXPECT_EQ(cpu.pc(), 0x208u);
}

// holdInterrupts (the SNES's IrqLock): the next boundary doesn't take an IRQ, nor an NMI if asked
TEST_CASE(w65c816_hold_interrupts) {
    FlatMemoryBus mem("ram", 24);
    W65C816 cpu;
    cpu.setDataBus(&mem);
    auto vec = [&](uint16_t at, uint16_t to) { mem.write8(at, to & 0xFF); mem.write8(at + 1, to >> 8); };
    vec(0xFFFC, 0x0200);
    vec(0xFFFE, 0x1000);                     // emulation IRQ
    vec(0xFFFA, 0x1200);                     // emulation NMI
    const uint8_t prog[] = {0x58, 0xEA, 0xEA, 0xEA, 0xEA};   // CLI, NOPs
    for (size_t i = 0; i < sizeof prog; ++i) mem.write8(0x200 + (uint32_t)i, prog[i]);
    cpu.reset();
    cpu.step();                              // CLI
    cpu.setIrqLine(true);
    cpu.holdInterrupts(false);
    cpu.step();                              // held: the NOP runs
    EXPECT_EQ(cpu.pc(), 0x202u);
    cpu.step();                              // then the IRQ
    EXPECT_EQ(cpu.pc(), 0x1000u);
    cpu.setIrqLine(false);

    cpu.setPc(0x202);
    cpu.setNmiLine(true);
    cpu.holdInterrupts(false);               // an NMI isn't held unless asked
    cpu.step();
    EXPECT_EQ(cpu.pc(), 0x1200u);
    cpu.setNmiLine(false);

    cpu.setPc(0x202);
    cpu.setNmiLine(true);
    cpu.holdInterrupts(true);
    cpu.step();
    EXPECT_EQ(cpu.pc(), 0x203u);
    cpu.step();
    EXPECT_EQ(cpu.pc(), 0x1200u);
}
