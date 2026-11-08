// Motorola 6809 core (#13). The instruction set was also checked against
// GCC6809's simulator (b-em mc6809nc) on random states: registers, memory and
// cycles agree, except where mc6809nc departs from the data sheet (DAA adding
// $60 twice).

#include "test_harness.h"
#include "plugins/6809/main/cpu6809.h"
#include "libmem/main/memory_bus.h"
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
