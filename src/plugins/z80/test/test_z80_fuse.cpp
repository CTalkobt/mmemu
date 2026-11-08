// Fuse's Z80 core tests (tests/z80/fuse: tests.in, tests.expected, from
// Fuse z80/tests, GPL-2.0-or-later): every test's bus events (memory reads
// and writes, port reads and writes, and the contention points, with their
// T-states), registers, final T-state and changed memory, as Fuse's
// coretest.c prints them.

#include "test_harness.h"
#include "plugins/z80/main/cpu_z80.h"
#include "libmem/main/memory_bus.h"
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Log {
    Z80* cpu = nullptr;
    std::vector<std::string> lines;
    void add(const char* type, unsigned addr, int data = -1) {
        char b[48];
        if (data < 0) std::snprintf(b, sizeof b, "%5u %s %04x", (unsigned)cpu->cycles(), type, addr);
        else std::snprintf(b, sizeof b, "%5u %s %04x %02x", (unsigned)cpu->cycles(), type, addr, data);
        lines.emplace_back(b);
    }
};

class MemBus : public FlatMemoryBus {
public:
    explicit MemBus(Log* log) : FlatMemoryBus("mem", 16), m_log(log) {}
    uint8_t read8(uint32_t a) override {
        uint8_t v = FlatMemoryBus::read8(a);
        m_log->add("MR", a, v);
        return v;
    }
    void write8(uint32_t a, uint8_t v) override {
        m_log->add("MW", a, v);
        FlatMemoryBus::write8(a, v);
    }
private:
    Log* m_log;
};

class IoBus : public FlatMemoryBus {
public:
    explicit IoBus(Log* log) : FlatMemoryBus("io", 16), m_log(log) {}
    uint8_t read8(uint32_t a) override { uint8_t v = (uint8_t)(a >> 8); m_log->add("PR", a, v); return v; }
    void write8(uint32_t a, uint8_t v) override { m_log->add("PW", a, v); }
private:
    Log* m_log;
};

// coretest.c: every memory cycle is "contended" (logged, no delay); ports
// at $4000-$7FFF and even ports log their contention points
class Timing : public IZ80Timing {
public:
    explicit Timing(Log* log) : m_log(log) {}
    int contend(uint16_t a, uint64_t, bool) override { m_log->add("MC", a); return 0; }
    int ioBefore(uint16_t port, uint64_t, bool) override {
        if ((port & 0xC000) == 0x4000) m_log->add("PC", port);
        return 1;
    }
    int ioAfter(uint16_t port, uint64_t t, bool) override {
        Z80* cpu = m_log->cpu;
        if (port & 1) {
            if ((port & 0xC000) == 0x4000) {
                for (int i = 0; i < 3; ++i) { m_log->add("PC", port); cpu->setCycles(cpu->cycles() + 1); }
                cpu->setCycles(t);              // the core adds the 3
            }
        } else {
            m_log->add("PC", port);
        }
        return 3;
    }
private:
    Log* m_log;
};

struct FuseTest {
    std::string name;
    std::vector<std::string> regs1, regs2;   // the two register lines
    std::vector<std::pair<unsigned, std::vector<unsigned>>> mem;
};

std::vector<std::string> words(const std::string& line) {
    std::istringstream is(line);
    std::vector<std::string> w;
    std::string x;
    while (is >> x) w.push_back(x);
    return w;
}

bool readInput(std::istream& in, FuseTest& t) {
    std::string line;
    while (std::getline(in, line) && words(line).empty()) {}
    if (!in) return false;
    t.name = words(line)[0];
    std::getline(in, line); t.regs1 = words(line);
    std::getline(in, line); t.regs2 = words(line);
    t.mem.clear();
    while (std::getline(in, line)) {
        auto w = words(line);
        if (w.empty()) continue;
        if (w[0] == "-1") break;
        std::vector<unsigned> bytes;
        for (size_t i = 1; i < w.size() && w[i] != "-1"; ++i) bytes.push_back((unsigned)std::stoul(w[i], nullptr, 16));
        t.mem.emplace_back((unsigned)std::stoul(w[0], nullptr, 16), bytes);
    }
    return true;
}

// The expected output: the name, the event lines, the registers, changed memory
bool readExpected(std::istream& in, std::string& name, std::vector<std::string>& out) {
    std::string line;
    out.clear();
    while (std::getline(in, line) && words(line).empty()) {}
    if (!in) return false;
    name = words(line)[0];
    while (std::getline(in, line) && !words(line).empty()) out.push_back(line);
    return true;
}

std::vector<std::string> run(const FuseTest& t) {
    Log log;
    MemBus mem(&log);
    IoBus io(&log);
    Timing timing(&log);
    Z80 cpu;
    log.cpu = &cpu;
    for (unsigned i = 0; i < 0x10000; i += 4) {
        mem.FlatMemoryBus::write8(i, 0xde); mem.FlatMemoryBus::write8(i + 1, 0xad);
        mem.FlatMemoryBus::write8(i + 2, 0xbe); mem.FlatMemoryBus::write8(i + 3, 0xef);
    }
    for (auto& m : t.mem)
        for (size_t i = 0; i < m.second.size(); ++i) mem.FlatMemoryBus::write8((m.first + i) & 0xFFFF, (uint8_t)m.second[i]);
    std::vector<uint8_t> initial(0x10000);
    for (unsigned i = 0; i < 0x10000; ++i) initial[i] = mem.peek8(i);

    cpu.setDataBus(&mem);
    cpu.setIoBus(&io);
    cpu.setTiming(&timing);
    cpu.reset();
    auto hex = [](const std::string& s) { return (uint32_t)std::stoul(s, nullptr, 16); };
    const int regIdx[13] = {Z80::R_AF, Z80::R_BC, Z80::R_DE, Z80::R_HL, Z80::R_AF2, Z80::R_BC2, Z80::R_DE2,
                            Z80::R_HL2, Z80::R_IX, Z80::R_IY, Z80::R_SP, Z80::R_PC, Z80::R_WZ};
    for (int i = 0; i < 13; ++i) cpu.regWrite(regIdx[i], hex(t.regs1[i]));
    cpu.regWrite(Z80::R_I, hex(t.regs2[0]));
    cpu.regWrite(Z80::R_R, hex(t.regs2[1]));
    cpu.regWrite(Z80::R_IFF, (std::stoi(t.regs2[2]) ? 1 : 0) | (std::stoi(t.regs2[3]) ? 2 : 0));
    cpu.regWrite(Z80::R_IM, (uint32_t)std::stoi(t.regs2[4]));
    if (std::stoi(t.regs2[5])) cpu.setHalted();
    const uint64_t end = (uint64_t)std::stoul(t.regs2[6]);
    cpu.setCycles(0);
    while (cpu.cycles() < end) cpu.step();

    std::vector<std::string> out = log.lines;
    char b[160];
    std::snprintf(b, sizeof b, "%04x %04x %04x %04x %04x %04x %04x %04x %04x %04x %04x %04x %04x",
                  cpu.regRead(Z80::R_AF), cpu.regRead(Z80::R_BC), cpu.regRead(Z80::R_DE), cpu.regRead(Z80::R_HL),
                  cpu.regRead(Z80::R_AF2), cpu.regRead(Z80::R_BC2), cpu.regRead(Z80::R_DE2), cpu.regRead(Z80::R_HL2),
                  cpu.regRead(Z80::R_IX), cpu.regRead(Z80::R_IY), cpu.regRead(Z80::R_SP), cpu.regRead(Z80::R_PC),
                  cpu.regRead(Z80::R_WZ));
    out.emplace_back(b);
    const uint32_t iff = cpu.regRead(Z80::R_IFF);
    std::snprintf(b, sizeof b, "%02x %02x %d %d %d %d %d", cpu.regRead(Z80::R_I), cpu.regRead(Z80::R_R),
                  (int)(iff & 1), (int)((iff >> 1) & 1), (int)cpu.regRead(Z80::R_IM), cpu.isHalted() ? 1 : 0,
                  (int)cpu.cycles());
    out.emplace_back(b);
    // Changed memory, in runs, as coretest prints it
    for (unsigned i = 0; i < 0x10000; ++i) {
        if (mem.peek8(i) == initial[i]) continue;
        std::string l;
        std::snprintf(b, sizeof b, "%04x ", i);
        l = b;
        while (i < 0x10000 && mem.peek8(i) != initial[i]) {
            std::snprintf(b, sizeof b, "%02x ", mem.peek8(i));
            l += b;
            ++i;
        }
        out.push_back(l + "-1");
    }
    return out;
}

std::string norm(const std::string& s) {
    std::string r;
    for (auto& w : words(s)) r += (r.empty() ? "" : " ") + w;
    return r;
}

}  // namespace

TEST_CASE(z80_fuse_core_tests) {
    std::ifstream in("tests/z80/fuse/tests.in"), exp("tests/z80/fuse/tests.expected");
    ASSERT(in && exp);
    FuseTest t;
    int total = 0, failed = 0;
    while (readInput(in, t)) {
        std::string ename;
        std::vector<std::string> want;
        ASSERT(readExpected(exp, ename, want));
        ASSERT_EQ(ename, t.name);
        auto got = run(t);
        ++total;
        bool same = got.size() == want.size();
        for (size_t i = 0; same && i < got.size(); ++i) same = norm(got[i]) == norm(want[i]);
        if (!same) {
            if (++failed <= 12) {
                std::printf("    fuse test %s differs\n", t.name.c_str());
                for (size_t i = 0; i < std::max(got.size(), want.size()); ++i) {
                    std::string g = i < got.size() ? norm(got[i]) : "", w = i < want.size() ? norm(want[i]) : "";
                    std::printf("      %s %-40s | %s\n", g == w ? " " : "*", g.c_str(), w.c_str());
                }
            }
        }
    }
    std::printf("    %d / %d Fuse core tests match\n", total - failed, total);
    ASSERT_EQ(failed, 0);
    ASSERT(total > 1300);
}
