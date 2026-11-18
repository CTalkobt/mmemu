// Motorola 6809 core (#13). The instruction set was also checked against
// GCC6809's simulator (b-em mc6809nc) on random states: registers, memory and
// cycles agree, except where mc6809nc departs from the data sheet (DAA adding
// $60 twice).

#include "test_harness.h"
#include "plugins/6809/main/cpu6809.h"
#include "libmem/main/memory_bus.h"
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace {
struct Rig {
    FlatMemoryBus mem{"system", 16};
    M6809 cpu;
    Rig() { cpu.setDataBus(&mem); }
    void load(uint16_t at, std::vector<uint8_t> code) {
        for (size_t i = 0; i < code.size(); ++i) mem.write8((uint32_t)(at + i), code[i]);
        mem.write8(0xFFFE, at >> 8);
        mem.write8(0xFFFF, at & 0xFF);
        cpu.reset();
    }
    int run(int n) { int t = 0; for (int i = 0; i < n; ++i) t += cpu.step(); return t; }
};
}  // namespace

// Reset, loads, arithmetic and flags, with the data sheet's cycle counts
TEST_CASE(m6809_basic) {
    Rig r;
    r.load(0x1000, {0x86, 0x7F,             // LDA #$7F      2
                    0x8B, 0x01,             // ADDA #1       2  -> $80: N, V, H
                    0xC6, 0x10,             // LDB #$10      2
                    0x3D,                   // MUL          11  -> D = $0800
                    0xCC, 0x12, 0x34,       // LDD #$1234    3
                    0xC3, 0xED, 0xCC,       // ADDD #$EDCC   4  -> 0, C, Z
                    0x19});                 // DAA
    EXPECT_EQ(r.cpu.pc(), 0x1000u);
    EXPECT_TRUE((r.cpu.cc() & (M6809::CC_I | M6809::CC_F)) == (M6809::CC_I | M6809::CC_F));
    EXPECT_EQ(r.run(2), 4);
    EXPECT_EQ(r.cpu.a(), 0x80);
    EXPECT_EQ(r.cpu.cc() & 0x2F, M6809::CC_N | M6809::CC_V | M6809::CC_H);
    EXPECT_EQ(r.run(2), 13);
    EXPECT_EQ(r.cpu.d(), 0x0800);
    EXPECT_EQ(r.run(2), 7);
    EXPECT_EQ(r.cpu.d(), 0x0000);
    EXPECT_EQ(r.cpu.cc() & 0x0F, M6809::CC_Z | M6809::CC_C);
    // DAA: A = $EA with H and C set adds $66 once (data sheet)
    r.cpu.regWrite(M6809::R_A, 0xEA);
    r.cpu.regWrite(M6809::R_CC, M6809::CC_H | M6809::CC_C);
    r.run(1);
    EXPECT_EQ(r.cpu.a(), 0x50);
    EXPECT_TRUE(r.cpu.cc() & M6809::CC_C);
}

// Indexed modes and their extra cycles: ,X+  [n16,Y]  -2,U  D,X  n8,PCR
TEST_CASE(m6809_indexed) {
    Rig r;
    r.mem.write8(0x2000, 0x11); r.mem.write8(0x2001, 0x22);
    r.mem.write8(0x3010, 0x20); r.mem.write8(0x3011, 0x01);   // pointer to $2001
    r.mem.write8(0x40FE, 0x33);
    r.load(0x1000, {0x8E, 0x20, 0x00,       // LDX #$2000
                    0xA6, 0x80,             // LDA ,X+       4+2
                    0x10, 0x8E, 0x30, 0x00, // LDY #$3000
                    0xE6, 0xB9, 0x00, 0x10, // LDB [$10,Y]   4+4+3
                    0xCE, 0x41, 0x00,       // LDU #$4100
                    0xA6, 0x5E,             // LDA -2,U      4+1
                    0x30, 0x8B,             // LEAX D,X      4+4
                    0xA6, 0x8C, 0xFD});     // LDA -3,PCR    4+1
    r.run(1);
    EXPECT_EQ(r.run(1), 6);
    EXPECT_EQ(r.cpu.a(), 0x11);
    EXPECT_EQ(r.cpu.x(), 0x2001);
    r.run(1);
    EXPECT_EQ(r.run(1), 11);
    EXPECT_EQ(r.cpu.b(), 0x22);
    r.run(1);
    EXPECT_EQ(r.run(1), 5);
    EXPECT_EQ(r.cpu.a(), 0x33);
    EXPECT_EQ(r.run(1), 8);
    EXPECT_EQ(r.cpu.x(), (uint16_t)(0x2001 + 0x3322));
    EXPECT_EQ(r.run(1), 5);
    EXPECT_EQ(r.cpu.a(), 0xA6);                      // PC after the operand, less 3: its own opcode
}

// FIRQ stacks PC and CC (E clear), IRQ the whole state (E set); RTI undoes
// each; NMI waits until S is loaded; SWI stacks everything and masks
TEST_CASE(m6809_interrupts) {
    Rig r;
    // Vectors: IRQ $2000, FIRQ $2100, NMI $2200, SWI $2300; handlers: RTI
    for (uint16_t v : {0xFFF8, 0xFFF6, 0xFFFC, 0xFFFA}) {
        const uint16_t h = v == 0xFFF8 ? 0x2000 : v == 0xFFF6 ? 0x2100 : v == 0xFFFC ? 0x2200 : 0x2300;
        r.mem.write8(v, h >> 8); r.mem.write8(v + 1, h & 0xFF);
        r.mem.write8(h, 0x3B);
    }
    r.load(0x1000, {0x12,                   // NOP
                    0x10, 0xCE, 0x80, 0x00, // LDS #$8000 (arms NMI)
                    0x1C, 0xAF,             // ANDCC #$AF (I, F clear)
                    0x12, 0x12, 0x12, 0x12});
    r.cpu.setNmiLine(true);                          // before S is loaded: not taken
    r.run(1);
    EXPECT_EQ(r.cpu.pc(), 0x1001u);
    r.cpu.setNmiLine(false);
    r.run(2);                                        // LDS, ANDCC
    r.cpu.setFirqLine(true);
    EXPECT_EQ(r.run(1), 10);
    EXPECT_EQ(r.cpu.pc(), 0x2100u);
    EXPECT_EQ(r.cpu.sp(), 0x8000u - 3);
    EXPECT_TRUE(!(r.mem.peek8(r.cpu.sp()) & M6809::CC_E));
    r.cpu.setFirqLine(false);
    EXPECT_EQ(r.run(1), 6);                          // RTI (E clear): PC and CC
    EXPECT_EQ(r.cpu.pc(), 0x1007u);
    r.cpu.setIrqLine(true);
    EXPECT_EQ(r.run(1), 19);
    EXPECT_EQ(r.cpu.pc(), 0x2000u);
    EXPECT_EQ(r.cpu.sp(), 0x8000u - 12);
    r.cpu.setIrqLine(false);
    EXPECT_EQ(r.run(1), 15);                         // RTI (E set): everything
    EXPECT_EQ(r.cpu.pc(), 0x1007u);
    r.cpu.setNmiLine(true);                          // now armed
    EXPECT_EQ(r.run(1), 19);
    EXPECT_EQ(r.cpu.pc(), 0x2200u);
    EXPECT_TRUE((r.cpu.cc() & (M6809::CC_I | M6809::CC_F)) == (M6809::CC_I | M6809::CC_F));
}

// CWAI stacks the state then waits; the interrupt then takes no second
// stacking. SYNC waits for any line; a masked one just continues
TEST_CASE(m6809_cwai_sync) {
    Rig r;
    r.mem.write8(0xFFF8, 0x20); r.mem.write8(0xFFF9, 0x00);
    r.mem.write8(0x2000, 0x3B);                      // RTI
    r.load(0x1000, {0x10, 0xCE, 0x80, 0x00,         // LDS #$8000
                    0x3C, 0xEF,                     // CWAI #$EF (clears I)
                    0x13,                           // SYNC
                    0x12});
    r.run(1);
    EXPECT_EQ(r.run(1), 20);
    EXPECT_TRUE(r.cpu.isHalted());
    EXPECT_EQ(r.cpu.sp(), 0x8000u - 12);
    r.run(5);                                        // still waiting
    EXPECT_EQ(r.cpu.pc(), 0x1006u);
    r.cpu.setIrqLine(true);
    r.run(1);
    EXPECT_EQ(r.cpu.pc(), 0x2000u);
    EXPECT_EQ(r.cpu.sp(), 0x8000u - 12);             // no second stacking
    r.cpu.setIrqLine(false);
    r.run(1);                                        // RTI
    EXPECT_EQ(r.cpu.pc(), 0x1006u);
    r.cpu.regWrite(M6809::R_CC, r.cpu.cc() | M6809::CC_I);
    r.run(1);                                        // SYNC
    r.run(3);
    EXPECT_EQ(r.cpu.pc(), 0x1007u);
    r.cpu.setIrqLine(true);                          // masked: SYNC ends, the next instruction runs
    r.run(1);
    EXPECT_TRUE(!r.cpu.isHalted());
    EXPECT_EQ(r.cpu.pc(), 0x1008u);
}

// SYNC with I and F set ends a bare program, but not once a machine drives
// the interrupt lines: a masked line still ends SYNC (DragonDOS waits for
// its disk's FIRQ that way, and NitrOS-9's boot stopped there in the CLI)
TEST_CASE(m6809_masked_sync_program_end) {
    Rig r;
    r.load(0x1000, {0x13, 0x12});                   // SYNC; NOP (I and F set by reset)
    r.run(1);
    EXPECT_TRUE(r.cpu.isProgramEnd(&r.mem));        // nothing can wake it
    Rig m;
    m.load(0x1000, {0x13, 0x12});
    m.cpu.setFirqLine(false);                       // a machine's devices drive /FIRQ
    m.run(1);
    EXPECT_TRUE(m.cpu.isHalted());
    EXPECT_TRUE(!m.cpu.isProgramEnd(&m.mem));
    m.cpu.setFirqLine(true);                        // the disk's DRQ: masked, SYNC ends
    m.run(1);
    EXPECT_EQ(m.cpu.pc(), 0x1002u);
}

// The disassembler
TEST_CASE(m6809_disassembler) {
    Rig r;
    r.load(0x1000, {0xA6, 0x9F, 0x12, 0x34, 0x10, 0x8E, 0xAB, 0xCD, 0x34, 0x76, 0x1F, 0x89, 0x26, 0xFE});
    char b[64];
    int n = r.cpu.disassembleOne(&r.mem, 0x1000, b, sizeof b);
    EXPECT_EQ(n, 4); EXPECT_EQ(std::string(b), std::string("LDA [$1234]"));
    n = r.cpu.disassembleOne(&r.mem, 0x1004, b, sizeof b);
    EXPECT_EQ(n, 4); EXPECT_EQ(std::string(b), std::string("LDY #$ABCD"));
    r.cpu.disassembleOne(&r.mem, 0x1008, b, sizeof b);
    EXPECT_EQ(std::string(b), std::string("PSHS A,B,X,Y,U"));
    r.cpu.disassembleOne(&r.mem, 0x100A, b, sizeof b);
    EXPECT_EQ(std::string(b), std::string("TFR A,B"));
    r.cpu.disassembleOne(&r.mem, 0x100C, b, sizeof b);
    EXPECT_EQ(std::string(b), std::string("BNE $100C"));
}

// The bus cycles in order: reads (R), writes (W) and the dead cycles the core
// places among them (D, reported through the dead-cycle hook) as the data
// sheet and XRoar's cycle-by-cycle core have them. Dead cycles after the
// last access (a TST's, SUBD's ALU cycle) are the rest of step()'s count.
namespace {
struct LogBus : FlatMemoryBus {
    std::string log;
    LogBus() : FlatMemoryBus("system", 16) {}
    uint8_t read8(uint32_t a) override { log += 'R'; return FlatMemoryBus::read8(a); }
    void write8(uint32_t a, uint8_t v) override { log += 'W'; FlatMemoryBus::write8(a, v); }
};
// One instruction at $1000 (X = $2000, DP = $00): its bus cycles, and step()'s count
std::string cyclesOf(const std::vector<uint8_t>& code, int& total, bool hook = true) {
    LogBus bus;
    M6809 cpu;
    cpu.setDataBus(&bus);
    for (size_t i = 0; i < code.size(); ++i) bus.FlatMemoryBus::write8((uint32_t)(0x1000 + i), code[i]);
    bus.FlatMemoryBus::write8(0x2000, 0x20);                   // a pointer for [,X]: $2010
    bus.FlatMemoryBus::write8(0x2001, 0x10);
    cpu.setPc(0x1000);
    cpu.regWrite(M6809::R_X, 0x2000);
    cpu.regWrite(M6809::R_DP, 0x00);
    if (hook) cpu.setDeadCycleHook([&bus]() { bus.log += 'D'; });
    bus.log.clear();
    total = cpu.step();
    return bus.log;
}
}  // namespace

TEST_CASE(m6809_dead_cycles_in_place) {
    struct Case { std::vector<uint8_t> code; const char* bus; int cycles; };
    const Case cases[] = {
        {{0x96, 0x22}, "RRDR", 4},                         // LDA <$22: the read on the 4th cycle
        {{0xB6, 0xFF, 0x22}, "RRRDR", 5},                  // LDA $FF22: on the 5th
        {{0xD7, 0x20}, "RRDW", 4},                         // STB <$20
        {{0xB7, 0xFF, 0x20}, "RRRDW", 5},                  // STA $FF20
        {{0xA6, 0x84}, "RRDR", 4},                         // LDA ,X
        {{0xA6, 0x1F}, "RRDDR", 5},                        // LDA -1,X
        {{0xA6, 0x89, 0x00, 0x10}, "RRRRDDDR", 8},         // LDA $10,X
        {{0xA6, 0x94}, "RRDRRDR", 7},                      // LDA [,X]: the pointer, a dead cycle, the operand
        {{0xA6, 0x9F, 0x20, 0x00}, "RRRRDRRDR", 9},        // LDA [$2000]
        {{0x0F, 0x20}, "RRDRDW", 6},                       // CLR <$20: read, dead, write
        {{0x76, 0x20, 0x00}, "RRRDRDW", 7},                // ROR $2000
        {{0x6C, 0x84}, "RRDRDW", 6},                       // INC ,X
        {{0x7D, 0x20, 0x00}, "RRRDRD", 7},                 // TST $2000 (one more dead cycle after)
        {{0xFC, 0x20, 0x00}, "RRRDRR", 6},                 // LDD $2000
        {{0xDD, 0x20}, "RRDWW", 5},                        // STD <$20
        {{0xB3, 0x20, 0x00}, "RRRDRR", 7},                 // SUBD $2000 (then the ALU's dead cycle)
        {{0x10, 0xBE, 0x20, 0x00}, "RRRRDRR", 7},          // LDY $2000
        {{0x11, 0xB3, 0x20, 0x00}, "RRRRDRR", 8},          // CMPU $2000
        {{0x95, 0x22}, "RRDR", 4},                         // BITA <$22
    };
    for (const auto& c : cases) {
        int total = 0;
        const std::string got = cyclesOf(c.code, total);
        if (got != c.bus) std::printf("  opcode $%02X: %s, want %s\n", c.code[0], got.c_str(), c.bus);
        EXPECT_TRUE(got == c.bus);
        EXPECT_EQ(total, c.cycles);
        // Without the hook: the same accesses and count
        int plain = 0;
        std::string want = c.bus;
        want.erase(std::remove(want.begin(), want.end(), 'D'), want.end());
        EXPECT_TRUE(cyclesOf(c.code, plain, false) == want);
        EXPECT_EQ(plain, c.cycles);
    }
}
