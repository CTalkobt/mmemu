// SPC700 core (#15) against Tom Harte's SingleStepTests
// (github.com/SingleStepTests/spc700, MIT licence): for each opcode a single
// instruction from a given state; the final registers, memory and the number
// of cycles must match.
//
// The tests (84 MB) are not in this repository: they are read from
// ../refs/ss-spc700/v1 (or $MMEMU_SPC700_TESTS) when present, else the test
// is skipped. By default the first 300 tests of each file run; set
// MMEMU_SPC700_FULL=1 for all of them.

#include "test_harness.h"
#include "plugins/machines/snes/main/spc700.h"
#include "libmem/main/ibus.h"
#include <nlohmann/json.hpp>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace {
class Ram64 : public IBus {
public:
    Ram64() { m_cfg = {16, 8, BusRole::DATA, true, 0xFFFF}; }
    uint8_t mem[0x10000] = {};
    const BusConfig& config() const override { return m_cfg; }
    const char* name() const override { return "ram"; }
    uint8_t peek8(uint32_t a) override { return mem[a & 0xFFFF]; }
    uint8_t read8(uint32_t a) override { return mem[a & 0xFFFF]; }
    void write8(uint32_t a, uint8_t v) override { mem[a & 0xFFFF] = v; }
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
    if (const char* d = std::getenv("MMEMU_SPC700_TESTS")) return d;
    return "../refs/ss-spc700/v1";
}
}  // namespace

TEST_CASE(spc700_single_step_tests) {
    const std::string dir = testsDir();
    {
        std::ifstream probe(dir + "/e8.json");
        if (!probe) { printf("  (skipped: no SingleStepTests at %s)\n", dir.c_str()); return; }
    }
    const bool full = std::getenv("MMEMU_SPC700_FULL") != nullptr;
    const size_t perFile = full ? SIZE_MAX : 300;
    static Ram64 ram;
    Spc700 cpu;
    cpu.setDataBus(&ram);
    int total = 0, failed = 0;
    std::map<int, int> failsByOp;
    std::string firstFail;
    char name[16];
    for (int op = 0; op < 256; ++op) {
        if (op == 0xEF || op == 0xFF) continue;            // SLEEP / STOP: the tests run on past the halt
        std::snprintf(name, sizeof name, "%02x.json", op);
        std::ifstream f(dir + "/" + name, std::ios::binary);
        if (!f) continue;
        nlohmann::json tests = nlohmann::json::array();
        if (full) {
            try { f >> tests; } catch (...) { printf("  bad JSON %s\n", name); continue; }
        } else {
            std::string buf(600000, '\0');
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
            for (const auto& kv : in["ram"]) ram.mem[kv[0].get<uint32_t>() & 0xFFFF] = (uint8_t)kv[1].get<int>();
            cpu.regWrite(Spc700::R_A, in["a"].get<int>());
            cpu.regWrite(Spc700::R_X, in["x"].get<int>());
            cpu.regWrite(Spc700::R_Y, in["y"].get<int>());
            cpu.regWrite(Spc700::R_SP, in["sp"].get<int>());
            cpu.regWrite(Spc700::R_PSW, in["psw"].get<int>());
            cpu.regWrite(Spc700::R_PC, in["pc"].get<int>());
            const int cycles = cpu.step();
            const auto& out = t["final"];
            std::string why;
            auto chk = [&](const char* r, int idx, const char* key) {
                const uint32_t got = cpu.regRead(idx), want = out[key].get<uint32_t>();
                if (got != want) { char b[64]; std::snprintf(b, sizeof b, " %s %X/%X", r, got, want); why += b; }
            };
            chk("A", Spc700::R_A, "a"); chk("X", Spc700::R_X, "x"); chk("Y", Spc700::R_Y, "y");
            chk("SP", Spc700::R_SP, "sp"); chk("PC", Spc700::R_PC, "pc"); chk("PSW", Spc700::R_PSW, "psw");
            for (const auto& kv : out["ram"]) {
                const uint32_t a = kv[0].get<uint32_t>();
                const int want = kv[1].get<int>();
                if (ram.mem[a & 0xFFFF] != want) { char b[64]; std::snprintf(b, sizeof b, " [%04X] %02X/%02X", a, ram.mem[a & 0xFFFF], want); why += b; }
            }
            const int wantCycles = (int)t["cycles"].size();
            if (cycles != wantCycles) { char b[48]; std::snprintf(b, sizeof b, " cycles %d/%d", cycles, wantCycles); why += b; }
            // Clean up for the next test
            for (const auto& kv : in["ram"]) ram.mem[kv[0].get<uint32_t>() & 0xFFFF] = 0;
            for (const auto& kv : out["ram"]) ram.mem[kv[0].get<uint32_t>() & 0xFFFF] = 0;
            ++total;
            if (!why.empty()) {
                ++failed;
                if (failsByOp[op]++ == 0 && failsByOp.size() <= 40) printf("  %s:%s\n", t["name"].get<std::string>().c_str(), why.c_str());
                if (firstFail.empty()) firstFail = t["name"].get<std::string>() + why;
            }
        }
    }
    printf("  %d tests, %d failed (%zu opcodes)\n", total, failed, failsByOp.size());
    ASSERT_EQ(failed, 0);
}
