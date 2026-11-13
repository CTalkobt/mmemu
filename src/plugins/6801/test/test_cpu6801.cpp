// Motorola MC6801/MC6803 core (#333): instruction groups with the data
// sheet's flags and cycle counts, interrupts, the timer and the SCI.

#include "test_harness.h"
#include "plugins/6801/main/cpu6801.h"
#include "libmem/main/memory_bus.h"
#include "libtoolchain/main/idisasm.h"
#include <string>
#include <vector>

namespace {
struct Rig {
    FlatMemoryBus mem{"system", 16};
    M6801 cpu;
    Rig() { cpu.setDataBus(&mem); }
    void load(uint16_t at, std::vector<uint8_t> code) {
        for (size_t i = 0; i < code.size(); ++i) mem.write8((uint32_t)(at + i), code[i]);
        mem.write8(0xFFFE, at >> 8);
        mem.write8(0xFFFF, at & 0xFF);
        cpu.reset();
    }
    void vec(uint16_t v, uint16_t to) { mem.write8(v, to >> 8); mem.write8(v + 1u, to & 0xFF); }
    int run(int n) { int t = 0; for (int i = 0; i < n; ++i) t += cpu.step(); return t; }
    uint8_t cc() const { return cpu.cc() & 0x2F; }   // H N Z V C (not I)
};
constexpr uint8_t C = M6801::CC_C, V = M6801::CC_V, Z = M6801::CC_Z, N = M6801::CC_N,
                  I = M6801::CC_I, H = M6801::CC_H;
}  // namespace

// Reset, the accumulators, H/V/N/C, and the 6801's 16-bit and MUL/ABX
TEST_CASE(m6801_arithmetic) {
    Rig r;
    r.load(0x1000, {0x86, 0x7F,             // LDAA #$7F     2
                    0x8B, 0x01,             // ADDA #1       2   -> $80: N V H
                    0xC6, 0x10,             // LDAB #$10     2
                    0x3D,                   // MUL          10   -> D = $0800, C = bit 7 of B
                    0xCC, 0x12, 0x34,       // LDD #$1234    3
                    0xC3, 0xED, 0xCC,       // ADDD #$EDCC   4   -> 0: Z C
                    0x83, 0x00, 0x01,       // SUBD #1       4   -> $FFFF: N C
                    0xCE, 0x10, 0x00,       // LDX #$1000    3
                    0xC6, 0xFF,             // LDAB #$FF     2
                    0x3A,                   // ABX           3   -> $10FF
                    0x8C, 0x20, 0x00});     // CPX #$2000    4   -> N C (the 6801's full flags)
    EXPECT_EQ(r.cpu.pc(), 0x1000u);
    EXPECT_TRUE(r.cpu.cc() & I);
    EXPECT_EQ(r.run(2), 4);
    EXPECT_EQ(r.cpu.a(), 0x80);
    EXPECT_EQ(r.cc(), N | V | H);
    EXPECT_EQ(r.run(2), 12);
    EXPECT_EQ(r.cpu.d(), 0x0800);
    EXPECT_EQ(r.cc() & C, 0);
    EXPECT_EQ(r.run(2), 7);
    EXPECT_EQ(r.cpu.d(), 0x0000);
    EXPECT_EQ(r.cc() & (N | Z | V | C), Z | C);
    EXPECT_EQ(r.run(1), 4);
    EXPECT_EQ(r.cpu.d(), 0xFFFF);
    EXPECT_EQ(r.cc() & (N | Z | V | C), N | C);
    EXPECT_EQ(r.run(3), 8);
    EXPECT_EQ(r.cpu.x(), 0x10FF);
    EXPECT_EQ(r.run(1), 4);
    EXPECT_EQ(r.cc() & (N | Z | V | C), N | C);
    // MUL sets C from bit 7 of the product
    r.cpu.regWrite(M6801::R_A, 0x10);
    r.cpu.regWrite(M6801::R_B, 0x08);
    r.mem.write8(r.cpu.pc(), 0x3D);
    r.run(1);
    EXPECT_EQ(r.cpu.d(), 0x0080);
    EXPECT_TRUE(r.cc() & C);
}

// Addressing modes and their cycles: direct (zero page), indexed, extended
TEST_CASE(m6801_addressing_cycles) {
    Rig r;
    r.mem.write8(0x0042, 0x11);
    r.mem.write8(0x3005, 0x22);
    r.mem.write8(0x4000, 0x33);
    r.load(0x1000, {0x96, 0x42,             // LDAA $42       3
                    0xCE, 0x30, 0x00,       // LDX #$3000     3
                    0xA6, 0x05,             // LDAA 5,X       4
                    0xB6, 0x40, 0x00,       // LDAA $4000     4
                    0x97, 0x50,             // STAA $50       3
                    0xA7, 0x10,             // STAA $10,X     4
                    0xB7, 0x50, 0x00,       // STAA $5000     4
                    0xDE, 0x42,             // LDX $42        4
                    0xEE, 0x00,             // LDX 0,X        5
                    0xFE, 0x30, 0x05,       // LDX $3005      5
                    0xDD, 0x60,             // STD $60        4
                    0xED, 0x00,             // STD 0,X        5
                    0xFD, 0x50, 0x10});     // STD $5010      5
    EXPECT_EQ(r.run(1), 3);
    EXPECT_EQ(r.cpu.a(), 0x11);
    EXPECT_EQ(r.run(2), 7);
    EXPECT_EQ(r.cpu.a(), 0x22);
    EXPECT_EQ(r.run(1), 4);
    EXPECT_EQ(r.cpu.a(), 0x33);
    EXPECT_EQ(r.run(1), 3);
    EXPECT_EQ(r.mem.read8(0x50), 0x33);
    EXPECT_EQ(r.run(1), 4);
    EXPECT_EQ(r.mem.read8(0x3010), 0x33);
    EXPECT_EQ(r.run(1), 4);
    EXPECT_EQ(r.mem.read8(0x5000), 0x33);
    EXPECT_EQ(r.run(1), 4);
    EXPECT_EQ(r.cpu.x(), 0x1100);           // $42 $43 (the $43 is 0)
    EXPECT_EQ(r.run(1), 5);
    EXPECT_EQ(r.run(1), 5);
    EXPECT_EQ(r.cpu.x(), 0x2200);
    EXPECT_EQ(r.run(1), 4);
    EXPECT_EQ(r.run(1), 5);
    EXPECT_EQ(r.run(1), 5);
    EXPECT_EQ(r.mem.read8(0x5010), r.cpu.a());
}

// Subroutines and the stack: BSR 6, JSR 5/6/6, RTS 5, PSH/PUL, PSHX 4, PULX 5, TSX/TXS
TEST_CASE(m6801_stack) {
    Rig r;
    r.mem.write8(0x2000, 0x39);             // RTS
    r.mem.write8(0x0070, 0x39);
    r.load(0x1000, {0x8E, 0x01, 0xFF,       // LDS #$01FF     3
                    0x8D, 0x10,             // BSR +$10       6  -> $1015
                    0x9D, 0x70,             // JSR $70        5
                    0xCE, 0x20, 0x00,       // LDX #$2000
                    0xAD, 0x00,             // JSR 0,X        6
                    0xBD, 0x20, 0x00,       // JSR $2000      6
                    0x36, 0x37,             // PSHA PSHB      3 3
                    0x3C,                   // PSHX           4
                    0x38,                   // PULX           5
                    0x30,                   // TSX            3
                    0x35});                 // TXS            3
    r.mem.write8(0x1015, 0x39);
    EXPECT_EQ(r.run(1), 3);
    EXPECT_EQ(r.run(1), 6);
    EXPECT_EQ(r.cpu.pc(), 0x1015u);
    EXPECT_EQ(r.mem.read8(0x01FF), 0x05);   // return address low byte at the top
    EXPECT_EQ(r.mem.read8(0x01FE), 0x10);
    EXPECT_EQ(r.cpu.sp(), 0x01FDu);
    EXPECT_EQ(r.run(1), 5);                 // RTS
    EXPECT_EQ(r.cpu.pc(), 0x1005u);
    EXPECT_EQ(r.run(1), 5);                 // JSR direct
    EXPECT_EQ(r.cpu.pc(), 0x0070u);
    r.run(2);
    EXPECT_EQ(r.run(1), 6);                 // JSR indexed
    EXPECT_EQ(r.cpu.pc(), 0x2000u);
    r.run(1);
    EXPECT_EQ(r.run(1), 6);                 // JSR extended
    r.run(1);
    r.cpu.regWrite(M6801::R_D, 0xABCD);
    EXPECT_EQ(r.run(2), 6);
    EXPECT_EQ(r.mem.read8(0x01FF), 0xAB);
    EXPECT_EQ(r.mem.read8(0x01FE), 0xCD);
    EXPECT_EQ(r.run(1), 4);                 // PSHX: low byte first
    EXPECT_EQ(r.mem.read8(0x01FD), 0x00);
    EXPECT_EQ(r.mem.read8(0x01FC), 0x20);
    r.cpu.regWrite(M6801::R_X, 0);
    EXPECT_EQ(r.run(1), 5);
    EXPECT_EQ(r.cpu.x(), 0x2000);
    EXPECT_EQ(r.cpu.sp(), 0x01FDu);
    EXPECT_EQ(r.run(1), 3);
    EXPECT_EQ(r.cpu.x(), 0x01FE);           // TSX: SP + 1
    r.cpu.regWrite(M6801::R_X, 0x0300);
    EXPECT_EQ(r.run(1), 3);
    EXPECT_EQ(r.cpu.sp(), 0x02FFu);         // TXS: X - 1
}

// Shifts, rotates (V = N xor C), the memory forms (6 cycles), TST clearing C, CLR, 16-bit shifts
TEST_CASE(m6801_shifts_rmw) {
    Rig r;
    r.mem.write8(0x3000, 0x81);
    r.load(0x1000, {0x86, 0x40,             // LDAA #$40
                    0x48,                   // ASLA  -> $80: N, C 0, V 1
                    0x48,                   // ASLA  -> $00: Z C, V 1
                    0x79, 0x30, 0x00,       // ROL $3000 (C in) -> $03, C 1   6
                    0x74, 0x30, 0x00,       // LSR $3000 -> $01, C 1, V 1     6
                    0x0D,                   // SEC
                    0x7D, 0x30, 0x00,       // TST $3000 -> C cleared         6
                    0x6F, 0x00,             // CLR 0,X                        6
                    0xCC, 0x80, 0x01,       // LDD #$8001
                    0x04,                   // LSRD -> $4000, C 1, V 1        3
                    0x05,                   // ASLD -> $8000, N, C 0, V 1     3
                    0x47});                 // ASRA -> $C0, N
    r.run(2);
    EXPECT_EQ(r.cpu.a(), 0x80);
    EXPECT_EQ(r.cc() & (N | Z | V | C), N | V);
    r.run(1);
    EXPECT_EQ(r.cc() & (N | Z | V | C), Z | V | C);
    EXPECT_EQ(r.run(1), 6);
    EXPECT_EQ(r.mem.read8(0x3000), 0x03);
    EXPECT_TRUE(r.cc() & C);
    EXPECT_EQ(r.run(1), 6);
    EXPECT_EQ(r.mem.read8(0x3000), 0x01);
    EXPECT_EQ(r.cc() & (N | Z | V | C), V | C);
    r.run(1);
    EXPECT_EQ(r.run(1), 6);
    EXPECT_EQ(r.cc() & (N | Z | V | C), 0);
    r.cpu.regWrite(M6801::R_X, 0x3000);
    EXPECT_EQ(r.run(1), 6);
    EXPECT_EQ(r.mem.read8(0x3000), 0x00);
    EXPECT_EQ(r.cc() & (N | Z | V | C), Z);
    r.run(1);
    EXPECT_EQ(r.run(1), 3);
    EXPECT_EQ(r.cpu.d(), 0x4000);
    EXPECT_EQ(r.cc() & (N | Z | V | C), V | C);
    EXPECT_EQ(r.run(1), 3);
    EXPECT_EQ(r.cpu.d(), 0x8000);
    EXPECT_EQ(r.cc() & (N | Z | V | C), N | V);
    r.run(1);
    EXPECT_EQ(r.cpu.a(), 0xC0);
}

// Every branch condition, taken and not (3 cycles each, BRN too)
TEST_CASE(m6801_branches) {
    struct Case { uint8_t op; uint8_t cc; bool taken; };
    const Case cases[] = {
        {0x20, 0, true}, {0x21, 0, false}, {0x22, 0, true}, {0x22, Z, false}, {0x23, C, true}, {0x23, 0, false},
        {0x24, 0, true}, {0x24, C, false}, {0x25, C, true}, {0x26, 0, true}, {0x26, Z, false}, {0x27, Z, true},
        {0x28, 0, true}, {0x28, V, false}, {0x29, V, true}, {0x2A, 0, true}, {0x2A, N, false}, {0x2B, N, true},
        {0x2C, N | V, true}, {0x2C, N, false}, {0x2D, N, true}, {0x2D, 0, false}, {0x2E, 0, true}, {0x2E, Z, false},
        {0x2F, Z, true}, {0x2F, V, true}, {0x2F, 0, false},
    };
    for (const auto& c : cases) {
        Rig r;
        r.load(0x1000, {c.op, 0x10});
        r.cpu.regWrite(M6801::R_CC, c.cc);
        EXPECT_EQ(r.run(1), 3);
        EXPECT_EQ(r.cpu.pc(), c.taken ? 0x1012u : 0x1002u);
    }
    // Backwards
    Rig r;
    r.load(0x1000, {0x01, 0x20, 0xFD});      // NOP; BRA $1000
    r.run(2);
    EXPECT_EQ(r.cpu.pc(), 0x1000u);
}

// DAA after ADDA, TAP/TPA, the transfers and inherent flag operations
TEST_CASE(m6801_daa_inherent) {
    Rig r;
    r.load(0x1000, {0x86, 0x19,             // LDAA #$19
                    0x8B, 0x28,             // ADDA #$28 -> $41, H
                    0x19,                   // DAA -> $47
                    0x86, 0x99,
                    0x8B, 0x01,             // $9A
                    0x19,                   // DAA -> $00, C
                    0x86, 0x0F,
                    0x06,                   // TAP: CC = $CF
                    0x07,                   // TPA
                    0x16,                   // TAB
                    0x08,                   // INX
                    0x09, 0x09});           // DEX DEX
    r.run(3);
    EXPECT_EQ(r.cpu.a(), 0x47);
    r.run(3);
    EXPECT_EQ(r.cpu.a(), 0x00);
    EXPECT_TRUE(r.cc() & C);
    EXPECT_TRUE(r.cc() & Z);
    r.run(2);
    EXPECT_EQ(r.cpu.cc(), 0xCF);
    r.cpu.regWrite(M6801::R_A, 0);
    r.run(1);
    EXPECT_EQ(r.cpu.a(), 0xCF);
    r.run(1);
    EXPECT_EQ(r.cpu.b(), 0xCF);
    EXPECT_EQ(r.cc() & (N | Z | V), N);
    r.cpu.regWrite(M6801::R_X, 0xFFFF);
    EXPECT_EQ(r.run(1), 3);
    EXPECT_EQ(r.cpu.x(), 0);
    EXPECT_TRUE(r.cc() & Z);
    r.run(2);
    EXPECT_EQ(r.cpu.x(), 0xFFFE);
    EXPECT_EQ(r.cc() & Z, 0);
}

// IRQ1 (12 cycles, stacking order), RTI (10), SWI (12), the I mask, CLI's
// one-instruction delay, NMI on its edge, WAI (9) then 3 to the handler
TEST_CASE(m6801_interrupts) {
    Rig r;
    r.vec(M6801::VEC_IRQ1, 0x2000);
    r.vec(M6801::VEC_NMI, 0x2100);
    r.vec(M6801::VEC_SWI, 0x2200);
    r.mem.write8(0x2000, 0x3B);             // RTI
    r.mem.write8(0x2100, 0x3B);
    r.mem.write8(0x2200, 0x3B);
    r.load(0x1000, {0x8E, 0x01, 0xFF,       // LDS #$01FF
                    0xCE, 0x12, 0x34,       // LDX #$1234
                    0xCC, 0x56, 0x78,       // LDD #$5678
                    0x0E,                   // CLI
                    0x01,                   // NOP (runs before the IRQ)
                    0x01,
                    0x3F,                   // SWI
                    0x3E,                   // WAI
                    0x01});
    r.cpu.setIrqLine(true);
    r.run(3);
    EXPECT_EQ(r.run(1), 2);                 // CLI
    EXPECT_EQ(r.run(1), 2);                 // NOP: the I bit clears after it
    EXPECT_EQ(r.cpu.pc(), 0x100Bu);
    EXPECT_EQ(r.run(1), 12);                // IRQ1
    EXPECT_EQ(r.cpu.pc(), 0x2000u);
    EXPECT_EQ(r.mem.read8(0x01FF), 0x0B);   // PCL PCH XL XH A B CC
    EXPECT_EQ(r.mem.read8(0x01FE), 0x10);
    EXPECT_EQ(r.mem.read8(0x01FD), 0x34);
    EXPECT_EQ(r.mem.read8(0x01FC), 0x12);
    EXPECT_EQ(r.mem.read8(0x01FB), 0x56);
    EXPECT_EQ(r.mem.read8(0x01FA), 0x78);
    EXPECT_EQ(r.mem.read8(0x01F9) & I, 0);
    EXPECT_EQ(r.cpu.sp(), 0x01F8u);
    EXPECT_TRUE(r.cpu.cc() & I);
    r.cpu.setIrqLine(false);
    EXPECT_EQ(r.run(1), 10);                // RTI
    EXPECT_EQ(r.cpu.pc(), 0x100Bu);
    EXPECT_EQ(r.cpu.x(), 0x1234);
    EXPECT_EQ(r.cpu.d(), 0x5678);
    r.run(1);
    EXPECT_EQ(r.run(1), 12);                // SWI
    EXPECT_EQ(r.cpu.pc(), 0x2200u);
    EXPECT_TRUE(r.cpu.cc() & I);
    r.run(1);                               // RTI: I clear again
    EXPECT_EQ(r.run(1), 9);                 // WAI
    EXPECT_TRUE(r.cpu.isHalted());
    EXPECT_EQ(r.run(1), 1);
    // NMI: the edge, even with I set; the registers are already stacked
    r.cpu.regWrite(M6801::R_CC, I);
    r.cpu.setNmiLine(true);
    EXPECT_EQ(r.run(1), 3);
    EXPECT_EQ(r.cpu.pc(), 0x2100u);
    EXPECT_TRUE(!r.cpu.isHalted());
    r.run(1);                               // RTI
    EXPECT_EQ(r.cpu.pc(), 0x100Eu);
    r.run(1);                               // a held NMI is not taken again
    EXPECT_EQ(r.cpu.pc(), 0x100Fu);
}

// Internal RAM and registers; the mode in port 2's top bits; RAME off
TEST_CASE(m6801_internal_space) {
    Rig r;
    r.load(0x1000, {0x86, 0x5A,             // LDAA #$5A
                    0x97, 0x80,             // STAA $80   (internal RAM, not the bus)
                    0x96, 0x03,             // LDAA $03   (port 2: mode 2 in bits 7-5)
                    0x7F, 0x00, 0x14,       // CLR $0014  (RAME off)
                    0x86, 0x77,
                    0x97, 0x81});           // STAA $81   (now external)
    r.run(2);
    EXPECT_EQ(r.mem.read8(0x80), 0x00);
    EXPECT_EQ(r.cpu.internalRam()[0], 0x5A);
    uint8_t v;
    EXPECT_TRUE(r.cpu.peekInternal(0x80, v));
    EXPECT_EQ(v, 0x5A);
    r.cpu.port(2).in = 0x1F;
    r.run(1);
    EXPECT_EQ(r.cpu.a() & 0xE0, 0x40);      // PC1 high: mode 2
    r.run(3);
    EXPECT_EQ(r.mem.read8(0x81), 0x77);
    EXPECT_TRUE(!r.cpu.isInternal(0x81));
    // Mode 3: no internal RAM; ports 3/4 registers are external in the expanded modes
    M6801 c3;
    c3.setModePins(3);
    c3.reset();
    EXPECT_TRUE(!c3.isInternal(0x80));
    EXPECT_TRUE(c3.isInternal(0x08));
    EXPECT_TRUE(!c3.isInternal(0x06));
    // The MC6801's ROM in mode 7
    M6801 c7(M6801::Variant::MC6801);
    std::vector<uint8_t> rom(2048, 0x01);
    rom[0x7FE] = 0xF8; rom[0x7FF] = 0x00;
    c7.setInternalRom(rom);
    c7.setModePins(7);
    c7.reset();
    EXPECT_EQ(c7.pc(), 0xF800u);
}

// Ports: DDRs, the data registers, preRead/postWrite
TEST_CASE(m6801_ports) {
    Rig r;
    int writes = 0, reads = 0;
    r.cpu.port(1).postWrite = [&]() { ++writes; };
    r.cpu.port(1).preRead = [&]() { ++reads; r.cpu.port(1).in = 0x3C; };
    r.load(0x1000, {0x86, 0x0F, 0x97, 0x00,   // P1DDR = $0F
                    0x86, 0xA5, 0x97, 0x02,   // P1DR = $A5
                    0x96, 0x02,               // LDAA P1DR
                    0x96, 0x00});             // DDR reads $FF
    writes = 0;
    r.run(4);
    EXPECT_EQ(writes, 2);
    EXPECT_EQ(r.cpu.port(1).out(), 0xF5);     // outputs from data, inputs float high
    r.run(1);
    EXPECT_EQ(reads, 1);
    EXPECT_EQ(r.cpu.a(), 0x35);               // low nibble from the register, high from the pins
    r.run(1);
    EXPECT_EQ(r.cpu.a(), 0xFF);
}

// The free-running counter: per cycle, preset by a write, TOF and its
// clearing (TCSR then the MSB), OCF and its interrupt, input capture
TEST_CASE(m6801_timer) {
    Rig r;
    r.vec(M6801::VEC_OCF, 0x2000);
    r.vec(M6801::VEC_TOF, 0x2100);
    r.mem.write8(0x2000, 0x3E);             // WAI in the handlers
    r.mem.write8(0x2100, 0x3E);
    r.load(0x1000, {0x8E, 0x01, 0xFF,       // LDS             3
                    0xDC, 0x09,             // LDD $09         4: the counter
                    0x97, 0x09,             // STAA $09        3: preset $FFF8
                    0x96, 0x08,             // LDAA $08 (TCSR)
                    0x01, 0x01, 0x01, 0x01,
                    0x96, 0x08,             // LDAA TCSR: TOF
                    0x96, 0x09,             // LDAA $09: clears TOF
                    0x96, 0x08});
    r.run(1);
    EXPECT_EQ(r.run(1), 4);
    EXPECT_EQ(r.cpu.d(), 6);                // read in its 3rd/4th bus cycles: 3 + 3
    r.run(1);
    EXPECT_EQ(r.cpu.counter(), 0xFFF8);
    r.run(1);
    EXPECT_EQ(r.cpu.a() & M6801::TCSR_TOF, 0);
    r.run(4);
    EXPECT_TRUE(r.cpu.tcsr() & M6801::TCSR_TOF);
    r.run(1);
    EXPECT_TRUE(r.cpu.a() & M6801::TCSR_TOF);
    r.run(1);
    r.run(1);
    EXPECT_EQ(r.cpu.a() & M6801::TCSR_TOF, 0);

    // Output compare: OCF at the match, the interrupt with EOCI
    Rig o;
    o.vec(M6801::VEC_OCF, 0x2000);
    o.mem.write8(0x2000, 0x3E);
    o.load(0x1000, {0x8E, 0x01, 0xFF,       // LDS
                    0xCC, 0x00, 0x40,       // LDD #$0040
                    0xDD, 0x0B,             // STD $0B (OCR)
                    0x86, M6801::TCSR_EOCI | M6801::TCSR_OLVL,
                    0x97, 0x08,             // TCSR: EOCI
                    0x86, 0x02, 0x97, 0x01, // P2DDR: P21 out
                    0x0E,                   // CLI
                    0x20, 0xFE});           // BRA *
    for (int i = 0; i < 40 && o.cpu.pc() != 0x2000u; ++i) o.run(1);
    EXPECT_EQ(o.cpu.pc(), 0x2000u);
    EXPECT_TRUE(o.cpu.tcsr() & M6801::TCSR_OCF);
    EXPECT_TRUE(o.cpu.port(2).data & 0x02);  // OLVL out on P21
    EXPECT_TRUE(o.cpu.cycles() >= 0x40);

    // Input capture on P20's rising edge (IEDG)
    Rig c;
    c.load(0x1000, {0x86, M6801::TCSR_IEDG, 0x97, 0x08, 0x01, 0x01, 0x01});
    c.run(2);
    c.cpu.setPort2Input(0xFE);
    c.run(1);
    EXPECT_EQ(c.cpu.tcsr() & M6801::TCSR_ICF, 0);
    const uint16_t at = c.cpu.counter();
    c.cpu.setPort2Input(0xFF);
    EXPECT_TRUE(c.cpu.tcsr() & M6801::TCSR_ICF);
    EXPECT_EQ(c.cpu.inputCapture(), at);
}

// The SCI: E/16 NRZ, a character out (start, 8 data LSB first, stop) after
// the preamble; TDRE; a character in via the queue, RDRF and its interrupt
TEST_CASE(m6801_sci) {
    Rig r;
    std::vector<uint8_t> sent;
    std::vector<bool> levels;
    r.cpu.onTxByte = [&](uint8_t b) { sent.push_back(b); };
    r.cpu.onTxLine = [&](bool l) { levels.push_back(l); };
    r.vec(M6801::VEC_SCI, 0x2000);
    r.mem.write8(0x2000, 0x96); r.mem.write8(0x2001, 0x11);   // LDAA TRCSR
    r.mem.write8(0x2002, 0xD6); r.mem.write8(0x2003, 0x12);   // LDAB RDR
    r.mem.write8(0x2004, 0x3E);                               // WAI
    r.load(0x1000, {0x8E, 0x01, 0xFF,
                    0x86, 0x04, 0x97, 0x10,          // RMCR: NRZ, E/16
                    0x86, 0x1A, 0x97, 0x11,          // TRCSR: TE RE RIE
                    0x96, 0x11,                      // LDAA TRCSR (TDRE seen)
                    0x86, 0x41, 0x97, 0x13,          // TDR = 'A'
                    0x96, 0x11,                      // TRCSR: TDRE clear
                    0x0E,
                    0x20, 0xFE});
    EXPECT_EQ(r.cpu.sciBitCycles(), 16);
    r.run(9);
    EXPECT_EQ(r.cpu.trcsr() & M6801::TR_TDRE, 0);
    for (int i = 0; i < 400; ++i) r.run(1);
    ASSERT_EQ(sent.size(), (size_t)1);
    EXPECT_EQ(sent[0], 0x41);
    EXPECT_TRUE(r.cpu.trcsr() & M6801::TR_TDRE);
    EXPECT_TRUE(r.cpu.txLine());
    // Receive
    r.cpu.receiveByte(0xC3);
    for (int i = 0; i < 400 && r.cpu.pc() < 0x2000u; ++i) r.run(1);
    EXPECT_EQ(r.cpu.pc(), 0x2000u);
    r.run(1);
    EXPECT_TRUE(r.cpu.a() & M6801::TR_RDRF);
    r.run(1);
    EXPECT_EQ(r.cpu.b(), 0xC3);
    EXPECT_EQ(r.cpu.trcsr() & M6801::TR_RDRF, 0);   // cleared by reading TRCSR then RDR
}

// The undocumented opcodes as XRoar found them
TEST_CASE(m6801_undocumented) {
    Rig r;
    r.load(0x1000, {0xC6, 0x55, 0x00,       // CLB
                    0x0D, 0x02,             // SEC; SEXA -> $FF
                    0x87, 0x12,             // A = $FF
                    0x4B,                   // DCA
                    0x4E});                 // HCF
    r.run(2);
    EXPECT_EQ(r.cpu.b(), 0);
    r.run(2);
    EXPECT_EQ(r.cpu.a(), 0xFF);
    r.cpu.regWrite(M6801::R_A, 0);
    EXPECT_EQ(r.run(1), 2);
    EXPECT_EQ(r.cpu.a(), 0xFF);
    EXPECT_EQ(r.cpu.pc(), 0x1007u);
    r.cpu.regWrite(M6801::R_A, 0x01);
    r.run(1);
    EXPECT_EQ(r.cpu.a(), 0x00);
    EXPECT_TRUE(r.cc() & C);
    r.run(1);
    EXPECT_TRUE(r.cpu.halted());
    const uint32_t pc = r.cpu.pc();
    r.run(5);
    EXPECT_EQ(r.cpu.pc(), pc);
}

// The disassembler
TEST_CASE(m6801_disassembler) {
    Rig r;
    const std::vector<uint8_t> code = {0x86, 0x12, 0x96, 0x34, 0xA6, 0x05, 0xB6, 0x12, 0x34, 0xCC, 0xAB, 0xCD,
                                       0x8D, 0xFE, 0x3D, 0x6E, 0x00, 0x7F, 0x40, 0x00, 0x00};
    for (size_t i = 0; i < code.size(); ++i) r.mem.write8(0x1000 + (uint32_t)i, code[i]);
    const char* want[] = {"LDAA #$12", "LDAA $34", "LDAA $05,X", "LDAA $1234", "LDD #$ABCD", "BSR $100C",
                          "MUL", "JMP $00,X", "CLR $4000", "FCB $00"};
    uint32_t a = 0x1000;
    for (const char* w : want) {
        char buf[64];
        const int n = r.cpu.disassembleOne(&r.mem, a, buf, sizeof buf);
        EXPECT_EQ(std::string(buf), std::string(w));
        a += (uint32_t)n;
    }
    DisasmEntry e;
    r.cpu.disassembleEntry(&r.mem, 0x100C, &e);
    EXPECT_TRUE(e.isCall);
    EXPECT_EQ(e.targetAddr, 0x100Cu);
}
