// The 8085 core: each instruction group, the flags (with the undocumented V
// and K), T-states, interrupts, the disassembler and line assembler; and the
// classic 8080/8085 CPU test programs under a CP/M BDOS stub, from
// MMEMU_8080_TESTS (skipped when unset): TST8080, 8080PRE, CPUTEST and
// 8085EXM (Ian Bartholomew's 8080/8085 CPU Exerciser with the CRCs of a real
// 8085), and with MMEMU_8080_EXM=1 also 8080EXM (an 8080's CRCs, reported).

#include "test_harness.h"
#include "plugins/8085/main/cpu8085.h"
#include "plugins/8085/main/asm8085.h"
#include "plugins/8085/main/opcodes8085.h"
#include "libmem/main/memory_bus.h"
#include "libtoolchain/main/idisasm.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace {

struct Rig {
    FlatMemoryBus mem{"system", 16};
    FlatMemoryBus io{"io", 8};
    I8085 cpu;
    Rig() { cpu.setDataBus(&mem); cpu.setIoBus(&io); cpu.reset(); cpu.regWrite(I8085::R_SP, 0xF000); }
    void load(uint16_t at, std::vector<uint8_t> code) {
        for (size_t i = 0; i < code.size(); ++i) mem.write8((uint32_t)(at + i), code[i]);
        cpu.setPc(at);
    }
    int run(int n) { int t = 0; for (int i = 0; i < n; ++i) t += cpu.step(); return t; }
    uint16_t reg(int r) const { return (uint16_t)cpu.regRead(r); }
    void setReg(int r, uint16_t v) { cpu.regWrite(r, v); }
};

#define EXPECT_HEX(a, b) do { \
    const unsigned va_ = (unsigned)(a); const unsigned vb_ = (unsigned)(b); \
    if (va_ != vb_) { std::fprintf(stderr, "  %s = $%X, expected $%X\n", #a, va_, vb_); } \
    ASSERT_EQ(va_, vb_); } while (0)

/** T-states of one instruction at $0100. */
int timeOf(std::vector<uint8_t> code, uint8_t flags = 0) {
    Rig r;
    r.setReg(I8085::R_AF, flags);
    r.setReg(I8085::R_HL, 0x2000);
    r.load(0x0100, code);
    return r.cpu.step();
}

}  // namespace

// Loads, stores, register pairs and their T-states
TEST_CASE(i8085_loads_and_stores) {
    Rig r;
    r.load(0x0100, {0x3E, 0x42,             // MVI A,$42        7
                    0x47,                   // MOV B,A          4
                    0x21, 0x00, 0x30,       // LXI H,$3000     10
                    0x70,                   // MOV M,B          7
                    0x36, 0x99,             // MVI M,$99       10
                    0x4E,                   // MOV C,M          7
                    0x32, 0x01, 0x30,       // STA $3001       13
                    0x2A, 0x00, 0x30,       // LHLD $3000      16
                    0x22, 0x10, 0x30,       // SHLD $3010      16
                    0xEB,                   // XCHG             4
                    0x01, 0x20, 0x30,       // LXI B,$3020     10
                    0x02,                   // STAX B           7
                    0x1A,                   // LDAX D           7
                    0x3A, 0x11, 0x30});     // LDA $3011       13
    EXPECT_EQ(r.run(2), 11);
    EXPECT_HEX(r.reg(I8085::R_BC) >> 8, 0x42);
    EXPECT_EQ(r.run(3), 27);
    EXPECT_EQ(r.run(1), 7);
    EXPECT_HEX(r.reg(I8085::R_BC) & 0xFF, 0x99);
    EXPECT_EQ(r.run(1), 13);
    EXPECT_EQ(r.run(1), 16);
    EXPECT_HEX(r.reg(I8085::R_HL), 0x4299);
    EXPECT_EQ(r.run(1), 16);
    EXPECT_HEX(r.mem.peek8(0x3010), 0x99);
    EXPECT_HEX(r.mem.peek8(0x3011), 0x42);
    EXPECT_EQ(r.run(1), 4);
    EXPECT_HEX(r.reg(I8085::R_DE), 0x4299);
    r.run(2);
    EXPECT_HEX(r.mem.peek8(0x3020), 0x42);
    EXPECT_EQ(r.run(1), 7);
    EXPECT_HEX(r.cpu.a(), 0x00);                         // LDAX D: ($4299)
    EXPECT_EQ(r.run(1), 13);
    EXPECT_HEX(r.cpu.a(), 0x42);
}

// The 8085's T-states (Intel 8085AH data sheet), where many differ from the 8080's
TEST_CASE(i8085_cycle_counts) {
    EXPECT_EQ(timeOf({0x00}), 4);                     // NOP
    EXPECT_EQ(timeOf({0x41}), 4);                     // MOV B,C (8080: 5)
    EXPECT_EQ(timeOf({0x7E}), 7);                     // MOV A,M
    EXPECT_EQ(timeOf({0x77}), 7);                     // MOV M,A
    EXPECT_EQ(timeOf({0x03}), 6);                     // INX B (8080: 5)
    EXPECT_EQ(timeOf({0x0B}), 6);                     // DCX B
    EXPECT_EQ(timeOf({0x04}), 4);                     // INR B
    EXPECT_EQ(timeOf({0x34}), 10);                    // INR M
    EXPECT_EQ(timeOf({0x09}), 10);                    // DAD B
    EXPECT_EQ(timeOf({0x80}), 4);                     // ADD B
    EXPECT_EQ(timeOf({0x86}), 7);                     // ADD M
    EXPECT_EQ(timeOf({0xC6, 1}), 7);                  // ADI
    EXPECT_EQ(timeOf({0xC5}), 12);                    // PUSH B (8080: 11)
    EXPECT_EQ(timeOf({0xC1}), 10);                    // POP B
    EXPECT_EQ(timeOf({0xC3, 0, 0}), 10);              // JMP
    EXPECT_EQ(timeOf({0xC2, 0, 0}, 0x00), 10);        // JNZ taken
    EXPECT_EQ(timeOf({0xC2, 0, 0}, I8085::FZ), 7);    // JNZ not taken
    EXPECT_EQ(timeOf({0xCD, 0, 0}), 18);              // CALL (8080: 17)
    EXPECT_EQ(timeOf({0xC4, 0, 0}, 0x00), 18);        // CNZ taken
    EXPECT_EQ(timeOf({0xC4, 0, 0}, I8085::FZ), 9);    // CNZ not taken
    EXPECT_EQ(timeOf({0xC9}), 10);                    // RET
    EXPECT_EQ(timeOf({0xC0}, 0x00), 12);              // RNZ taken
    EXPECT_EQ(timeOf({0xC0}, I8085::FZ), 6);          // RNZ not taken
    EXPECT_EQ(timeOf({0xFF}), 12);                    // RST 7
    EXPECT_EQ(timeOf({0xE9}), 6);                     // PCHL
    EXPECT_EQ(timeOf({0xF9}), 6);                     // SPHL
    EXPECT_EQ(timeOf({0xE3}), 16);                    // XTHL
    EXPECT_EQ(timeOf({0xEB}), 4);                     // XCHG
    EXPECT_EQ(timeOf({0xDB, 0}), 10);                 // IN
    EXPECT_EQ(timeOf({0xD3, 0}), 10);                 // OUT
    EXPECT_EQ(timeOf({0x76}), 5);                     // HLT
    EXPECT_EQ(timeOf({0x20}), 4);                     // RIM
    EXPECT_EQ(timeOf({0x30}), 4);                     // SIM
    EXPECT_EQ(timeOf({0xFB}), 4);                     // EI
    EXPECT_EQ(timeOf({0x27}), 4);                     // DAA
    // The undocumented ones
    EXPECT_EQ(timeOf({0x08}), 10);                    // DSUB
    EXPECT_EQ(timeOf({0x10}), 7);                     // ARHL
    EXPECT_EQ(timeOf({0x18}), 10);                    // RDEL
    EXPECT_EQ(timeOf({0x28, 1}), 10);                 // LDHI
    EXPECT_EQ(timeOf({0x38, 1}), 10);                 // LDSI
    EXPECT_EQ(timeOf({0xD9}), 10);                    // SHLX
    EXPECT_EQ(timeOf({0xED}), 10);                    // LHLX
    EXPECT_EQ(timeOf({0xCB}, 0x00), 6);               // RSTV, V clear
    EXPECT_EQ(timeOf({0xCB}, I8085::FV), 12);         // RSTV, V set
    EXPECT_EQ(timeOf({0xDD, 0, 0}, 0x00), 10);        // JNK taken
    EXPECT_EQ(timeOf({0xDD, 0, 0}, I8085::FK), 7);    // JNK not taken
    EXPECT_EQ(timeOf({0xFD, 0, 0}, I8085::FK), 10);   // JK taken
    EXPECT_EQ(timeOf({0xFD, 0, 0}, 0x00), 7);         // JK not taken
}

// The 8-bit arithmetic flags, with V (signed overflow) and K (V xor S)
TEST_CASE(i8085_arithmetic_flags) {
    Rig r;
    r.load(0x0100, {0x3E, 0x7F, 0xC6, 0x01,          // MVI A,$7F; ADI 1     -> $80: S AC V
                    0xD6, 0x01,                      // SUI 1                -> $7F: V K
                    0x3E, 0x01, 0xFE, 0x02,          // MVI A,1; CPI 2       -> S K P CY (A < 2)
                    0x3E, 0x05, 0x37, 0xCE, 0x0A,    // MVI A,5; STC; ACI $0A -> $10: AC
                    0x3E, 0x10, 0x37, 0xDE, 0x00});  // MVI A,$10; STC; SBI 0 -> $0F: P
    r.run(2);
    EXPECT_HEX(r.cpu.a(), 0x80);
    EXPECT_HEX(r.cpu.f(), I8085::FS | I8085::FA | I8085::FV);
    r.run(1);
    EXPECT_HEX(r.cpu.a(), 0x7F);
    EXPECT_HEX(r.cpu.f(), I8085::FK | I8085::FV);
    r.run(2);
    EXPECT_HEX(r.cpu.a(), 0x01);
    EXPECT_HEX(r.cpu.f(), I8085::FS | I8085::FK | I8085::FP | I8085::FC);
    r.run(3);
    EXPECT_HEX(r.cpu.a(), 0x10);
    EXPECT_HEX(r.cpu.f(), I8085::FA);
    r.run(3);
    EXPECT_HEX(r.cpu.a(), 0x0F);
    // $10 + ~0 + 0: no carry out of bit 3 (AC clear), a carry out of bit 7 (no borrow)
    EXPECT_HEX(r.cpu.f(), I8085::FP);
}

// ANA sets AC (the 8085; the 8080 ORs the operands' bit 3), ORA/XRA clear it; K is the sign
TEST_CASE(i8085_logic_flags) {
    Rig r;
    r.load(0x0100, {0x3E, 0xF0, 0xE6, 0x0F,      // MVI A,$F0; ANI $0F -> 0: Z P AC
                    0x3E, 0x80, 0xF6, 0x00,      // MVI A,$80; ORI 0   -> S K
                    0xAF,                        // XRA A
                    0x37, 0x3E, 0xC3, 0xA7});    // STC; MVI A,$C3; ANA A -> S K P AC, CY clear
    r.run(2);
    EXPECT_HEX(r.cpu.f(), I8085::FZ | I8085::FP | I8085::FA);
    r.run(2);
    EXPECT_HEX(r.cpu.f(), I8085::FS | I8085::FK);
    r.run(1);
    EXPECT_HEX(r.cpu.f(), I8085::FZ | I8085::FP);
    r.run(3);
    EXPECT_HEX(r.cpu.f(), I8085::FS | I8085::FK | I8085::FP | I8085::FA);
}

// INR/DCR (CY kept, V only at $7F/$80), DAA, CMA, STC/CMC, the rotates
TEST_CASE(i8085_inr_dcr_daa_rotates) {
    Rig r;
    r.load(0x0100, {0x37, 0x06, 0x7F, 0x04,      // STC; MVI B,$7F; INR B -> $80: S AC V CY
                    0x0E, 0x80, 0x0D,            // MVI C,$80; DCR C      -> $7F: V K CY
                    0x3E, 0x99, 0xB7, 0x3C,      // MVI A,$99; ORA A; INR A -> $9A
                    0x27,                        // DAA                   -> $00, CY Z P AC
                    0x3E, 0x81, 0x07,            // MVI A,$81; RLC        -> $03 CY, V (A+A overflows), K
                    0x0F,                        // RRC                   -> $81 CY, V clear
                    0x2F});                      // CMA                   -> $7E, flags kept
    r.run(3);
    EXPECT_HEX(r.reg(I8085::R_BC) >> 8, 0x80);
    EXPECT_HEX(r.cpu.f(), I8085::FS | I8085::FA | I8085::FV | I8085::FC);
    r.run(2);
    EXPECT_HEX(r.reg(I8085::R_BC) & 0xFF, 0x7F);
    EXPECT_HEX(r.cpu.f(), I8085::FV | I8085::FK | I8085::FC);
    r.run(3);
    EXPECT_HEX(r.cpu.a(), 0x9A);
    r.run(1);
    EXPECT_HEX(r.cpu.a(), 0x00);
    EXPECT_HEX(r.cpu.f(), I8085::FZ | I8085::FP | I8085::FA | I8085::FC);
    r.run(2);
    EXPECT_HEX(r.cpu.a(), 0x03);
    EXPECT_HEX(r.cpu.f() & (I8085::FC | I8085::FV | I8085::FK), I8085::FC | I8085::FV | I8085::FK);
    r.run(1);
    EXPECT_HEX(r.cpu.a(), 0x81);
    EXPECT_HEX(r.cpu.f() & (I8085::FC | I8085::FV), I8085::FC);
    const uint8_t f = r.cpu.f();
    r.run(1);
    EXPECT_HEX(r.cpu.a(), 0x7E);
    EXPECT_HEX(r.cpu.f(), f);

    // DAA's V: only for 7x -> 8x
    Rig d;
    d.load(0x0100, {0x3E, 0x79, 0xC6, 0x01, 0x27});   // $79 + 1 = $7A; DAA -> $80
    d.run(3);
    EXPECT_HEX(d.cpu.a(), 0x80);
    EXPECT_HEX(d.cpu.f(), I8085::FS | I8085::FA | I8085::FV);
}

// DAD (CY, V, K), INX/DCX (only K: the incrementer's carry), PUSH/POP PSW
TEST_CASE(i8085_16bit_and_stack) {
    Rig r;
    r.setReg(I8085::R_AF, 0x00D5);
    r.load(0x0100, {0x21, 0xFF, 0x7F, 0x01, 0x01, 0x00, 0x09,   // LXI H,$7FFF; LXI B,1; DAD B
                    0x11, 0xFF, 0xFF, 0x13, 0x13,               // LXI D,$FFFF; INX D; INX D
                    0x1B, 0x1B,                                 // DCX D; DCX D
                    0xF5, 0xF1});                               // PUSH PSW; POP PSW
    r.run(3);
    EXPECT_HEX(r.reg(I8085::R_HL), 0x8000);
    EXPECT_HEX(r.cpu.f(), 0xD4 | I8085::FV);               // S Z AC P kept; CY clear; V; K = V ^ 1
    r.run(2);
    EXPECT_HEX(r.reg(I8085::R_DE), 0x0000);
    EXPECT_HEX(r.cpu.f(), 0xD6 | I8085::FK);
    r.run(1);
    EXPECT_HEX(r.cpu.f() & I8085::FK, 0);
    r.run(1);
    EXPECT_HEX(r.reg(I8085::R_DE), 0x0000);
    EXPECT_HEX(r.cpu.f() & I8085::FK, 0);
    r.run(1);
    EXPECT_HEX(r.reg(I8085::R_DE), 0xFFFF);
    EXPECT_HEX(r.cpu.f() & I8085::FK, I8085::FK);
    const uint16_t sp = r.reg(I8085::R_SP);
    r.run(1);
    EXPECT_HEX(r.mem.peek8(sp - 2), r.cpu.f());
    r.mem.write8(sp - 2, 0xFF);                            // bit 3 stays 0
    r.run(1);
    EXPECT_HEX(r.cpu.f(), 0xF7);
    EXPECT_HEX(r.reg(I8085::R_SP), sp);
}

// Jumps, calls, returns, RST, PCHL, XTHL, I/O
TEST_CASE(i8085_control_flow_and_io) {
    Rig r;
    r.load(0x0100, {0xCD, 0x00, 0x02,        // CALL $0200
                    0xDB, 0x10,              // IN $10
                    0x76});
    r.load(0x0200, {0x3E, 0x5A, 0xD3, 0x20,  // MVI A,$5A; OUT $20
                    0xAF, 0xC8,              // XRA A; RZ
                    0x00});
    r.cpu.setPc(0x0100);
    r.io.write8(0x10, 0x77);
    r.run(1);
    EXPECT_HEX(r.reg(I8085::R_PC), 0x0200);
    EXPECT_HEX(r.reg(I8085::R_SP), 0xEFFE);
    r.run(2);
    EXPECT_HEX(r.io.peek8(0x20), 0x5A);
    r.run(2);
    EXPECT_HEX(r.reg(I8085::R_PC), 0x0103);
    r.run(1);
    EXPECT_HEX(r.cpu.a(), 0x77);
    EXPECT_EQ(r.run(1), 5);
    ASSERT(r.cpu.isHalted());
    EXPECT_HEX(r.reg(I8085::R_PC), 0x0106);
    EXPECT_EQ(r.run(1), 4);                                  // halted: idles

    Rig x;
    x.setReg(I8085::R_HL, 0x1234);
    x.load(0x0100, {0xE5, 0x21, 0x78, 0x56, 0xE3,            // PUSH H; LXI H,$5678; XTHL
                    0x21, 0x00, 0x03, 0xE9});                // LXI H,$0300; PCHL
    x.run(3);
    EXPECT_HEX(x.reg(I8085::R_HL), 0x1234);
    EXPECT_HEX(x.mem.peek8(0xEFFE), 0x78);
    x.run(2);
    EXPECT_HEX(x.reg(I8085::R_PC), 0x0300);
    x.load(0x0300, {0xEF});                                  // RST 5
    x.run(1);
    EXPECT_HEX(x.reg(I8085::R_PC), 0x0028);
    EXPECT_HEX(x.mem.peek8(0xEFFC), 0x01);
}

// DSUB ARHL RDEL LDHI LDSI SHLX LHLX JNK JK RSTV
TEST_CASE(i8085_undocumented_instructions) {
    Rig r;
    r.setReg(I8085::R_HL, 0x1000);
    r.setReg(I8085::R_BC, 0x0001);
    r.setReg(I8085::R_AF, 0x1200);
    r.load(0x0100, {0x08});                                  // DSUB -> $0FFF
    r.run(1);
    EXPECT_HEX(r.reg(I8085::R_HL), 0x0FFF);
    EXPECT_HEX(r.cpu.a(), 0x12);
    EXPECT_HEX(r.cpu.f(), I8085::FP);                        // from the high byte: $10 - 0 - 1
    r.setReg(I8085::R_HL, 0x0100);
    r.setReg(I8085::R_BC, 0x0100);
    r.load(0x0100, {0x08});                                  // -> 0
    r.run(1);
    EXPECT_HEX(r.cpu.f(), I8085::FZ | I8085::FA | I8085::FP);
    r.setReg(I8085::R_HL, 0x0005);
    r.setReg(I8085::R_BC, 0x0001);
    r.load(0x0100, {0x08});                                  // -> $0004: Z for all 16 bits (clear)
    r.run(1);
    EXPECT_HEX(r.reg(I8085::R_HL), 0x0004);
    EXPECT_HEX(r.cpu.f() & I8085::FZ, 0);
    r.setReg(I8085::R_HL, 0x0000);
    r.setReg(I8085::R_BC, 0x0001);
    r.load(0x0100, {0x08});                                  // -> $FFFF, CY
    r.run(1);
    EXPECT_HEX(r.cpu.f() & (I8085::FC | I8085::FS), I8085::FC | I8085::FS);

    r.setReg(I8085::R_AF, 0x0000);
    r.setReg(I8085::R_HL, 0x8001);
    r.setReg(I8085::R_DE, 0x4001);
    r.load(0x0100, {0x10,                // ARHL -> $C000, CY
                    0x18,                // RDEL: $4001 << 1 | 1 -> $8003, CY 0, V (bit 15 ^ bit 14)
                    0x21, 0x34, 0x12,    // LXI H,$1234
                    0x28, 0x10,          // LDHI $10 -> DE = $1244
                    0x31, 0x00, 0x20,    // LXI SP,$2000
                    0x38, 0x02,          // LDSI 2 -> DE = $2002
                    0x11, 0x00, 0x30,    // LXI D,$3000
                    0xD9,                // SHLX
                    0x21, 0x00, 0x00,    // LXI H,0
                    0xED});              // LHLX
    r.run(1);
    EXPECT_HEX(r.reg(I8085::R_HL), 0xC000);
    EXPECT_HEX(r.cpu.f(), I8085::FC);
    r.run(1);
    EXPECT_HEX(r.reg(I8085::R_DE), 0x8003);
    EXPECT_HEX(r.cpu.f(), I8085::FV);
    r.run(2);
    EXPECT_HEX(r.reg(I8085::R_DE), 0x1244);
    r.run(2);
    EXPECT_HEX(r.reg(I8085::R_DE), 0x2002);
    r.run(2);
    EXPECT_HEX(r.mem.peek8(0x3000), 0x34);
    EXPECT_HEX(r.mem.peek8(0x3001), 0x12);
    r.run(2);
    EXPECT_HEX(r.reg(I8085::R_HL), 0x1234);

    // JK / JNK after INX's K; RSTV
    Rig j;
    j.load(0x0100, {0x01, 0xFF, 0xFF, 0x03,      // LXI B,$FFFF; INX B (K)
                    0xDD, 0x00, 0x05,            // JNK $0500 (not taken)
                    0xFD, 0x00, 0x04});          // JK $0400
    j.run(3);
    EXPECT_HEX(j.reg(I8085::R_PC), 0x0107);
    j.run(1);
    EXPECT_HEX(j.reg(I8085::R_PC), 0x0400);
    j.load(0x0400, {0x3E, 0x7F, 0xC6, 0x01, 0xCB});    // MVI A,$7F; ADI 1 (V); RSTV
    j.run(2);
    EXPECT_EQ(j.run(1), 12);
    EXPECT_HEX(j.reg(I8085::R_PC), 0x0040);
}

// TRAP > RST 7.5 > RST 6.5 > RST 5.5 > INTR, masks, IE and EI's delay
TEST_CASE(i8085_interrupt_priorities) {
    Rig r;
    std::vector<I8085::Irq> acks;
    r.cpu.setAckHook([&](I8085::Irq s) { acks.push_back(s); });
    for (uint16_t v : {0x2C, 0x34, 0x3C, 0x38}) r.load(v, {0xFB, 0xC9});   // EI; RET
    r.load(0x0100, {0x3E, 0x08, 0x30,            // MVI A,$08; SIM (unmask all)
                    0xFB,                        // EI
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    r.run(3);
    r.cpu.setRst55(true);
    r.cpu.setRst65(true);
    r.cpu.setRst75(true);
    r.cpu.setIntr(true);
    EXPECT_EQ(r.run(1), 4);                       // the instruction after EI runs first
    EXPECT_HEX(r.reg(I8085::R_PC), 0x0105);
    EXPECT_EQ(r.run(1), 12);
    EXPECT_HEX(r.reg(I8085::R_PC), 0x003C);       // RST 7.5
    ASSERT(!r.cpu.interruptsEnabled());
    EXPECT_HEX(r.cpu.rimValue() & I8085::IM_I75, 0);   // the latch is cleared by the acknowledge
    r.run(2);                                     // EI; RET
    EXPECT_HEX(r.reg(I8085::R_PC), 0x0105);
    r.run(1);
    EXPECT_HEX(r.reg(I8085::R_PC), 0x0034);       // RST 6.5
    r.cpu.setRst65(false);
    r.run(3);
    EXPECT_HEX(r.reg(I8085::R_PC), 0x002C);       // RST 5.5
    r.cpu.setRst55(false);
    r.run(3);
    EXPECT_HEX(r.reg(I8085::R_PC), 0x0038);       // INTR: RST 7 from the floating bus
    r.cpu.setIntr(false);
    ASSERT(acks.size() == 4);
    ASSERT(acks[0] == I8085::Irq::Rst75 && acks[1] == I8085::Irq::Rst65);
    ASSERT(acks[2] == I8085::Irq::Rst55 && acks[3] == I8085::Irq::Intr);

    // TRAP: above all, regardless of IE; edge then level
    Rig t;
    t.load(0x0100, {0x00, 0x00, 0x00, 0x00});
    t.cpu.setRst75(true);
    t.cpu.setTrap(true);
    EXPECT_EQ(t.run(1), 12);
    EXPECT_HEX(t.reg(I8085::R_PC), 0x0024);
    t.run(1);                                     // still high: no second TRAP
    EXPECT_HEX(t.reg(I8085::R_PC), 0x0025);
    t.cpu.setTrap(false);
    t.cpu.setTrap(true);
    t.cpu.setTrap(false);                         // gone before it was sampled
    t.run(1);
    EXPECT_HEX(t.reg(I8085::R_PC), 0x0026);
}

// SIM's masks and R7.5, the RST 7.5 latch, RIM, SID/SOD, RIM after a TRAP
TEST_CASE(i8085_rim_sim) {
    Rig r;
    std::vector<bool> sod;
    r.cpu.setSodCallback([&](bool v) { sod.push_back(v); });
    r.cpu.setSid(true);
    // Reset: all masked, IE off
    EXPECT_HEX(r.cpu.rimValue(), I8085::IM_SID | 7);
    r.load(0x0100, {0xFB, 0x00,                  // EI; NOP
                    0x20,                        // RIM
                    0x3E, 0x1F, 0x30,            // MVI A,$1F; SIM: all masked (MSE), R7.5
                    0x20,                        // RIM
                    0x3E, 0x0D, 0x30,            // MVI A,$0D; SIM: only 6.5 unmasked
                    0x3E, 0xC0, 0x30,            // MVI A,$C0; SIM: SOD = 1 (MSE clear: masks kept)
                    0x3E, 0x40, 0x30,            // MVI A,$40; SIM: SOD = 0
                    0x00});
    r.cpu.setRst75(true);                        // latched though masked
    r.cpu.setRst65(true);
    r.run(3);
    EXPECT_HEX(r.cpu.a(), I8085::IM_SID | I8085::IM_I75 | I8085::IM_I65 | I8085::IM_IE | 7);
    EXPECT_HEX(r.reg(I8085::R_PC), 0x0103);      // all masked: nothing taken
    r.run(3);
    EXPECT_HEX(r.cpu.a(), I8085::IM_SID | I8085::IM_I65 | I8085::IM_IE | 7);
    r.run(2);
    EXPECT_HEX(r.cpu.masks(), 5);
    r.run(1);                                    // RST 6.5, before the next instruction
    EXPECT_HEX(r.reg(I8085::R_PC), 0x0034);
    r.cpu.setRst65(false);
    r.cpu.setPc(0x010A);
    r.run(4);
    EXPECT_HEX(r.cpu.masks(), 5);
    ASSERT(sod.size() == 2 && sod[0] && !sod[1]);
    ASSERT(!r.cpu.sod());

    // The first RIM after a TRAP shows IE from before it
    Rig t;
    t.load(0x0024, {0x20, 0x20});                // RIM; RIM
    t.load(0x0100, {0xFB, 0x00, 0x00});
    t.run(2);
    t.cpu.setTrap(true);
    t.run(1);
    ASSERT(!t.cpu.interruptsEnabled());
    t.run(1);
    EXPECT_HEX(t.cpu.a() & I8085::IM_IE, I8085::IM_IE);
    t.run(1);
    EXPECT_HEX(t.cpu.a() & I8085::IM_IE, 0);
}

// HLT until an interrupt (only TRAP when IE is off); INTR's CALL from the data bus
TEST_CASE(i8085_halt_and_intr_vectors) {
    Rig r;
    r.load(0x0100, {0xFB, 0x76, 0x00});          // EI; HLT
    r.run(2);
    ASSERT(r.cpu.isHalted());
    EXPECT_EQ(r.run(3), 12);
    std::vector<uint8_t> bus = {0xCD, 0x00, 0x20};
    size_t at = 0;
    r.cpu.setIntaCallback([&]() { return bus[at++]; });
    r.cpu.setIntr(true);
    EXPECT_EQ(r.run(1), 18);                     // CALL $2000
    ASSERT(!r.cpu.isHalted());
    EXPECT_HEX(r.reg(I8085::R_PC), 0x2000);
    EXPECT_HEX(r.mem.peek8(0xEFFE), 0x02);       // returns past the HLT
    EXPECT_HEX(r.mem.peek8(0xEFFF), 0x01);

    Rig d;
    d.load(0x0100, {0xF3, 0x76});                // DI; HLT
    d.run(2);
    d.cpu.setIntr(true);
    d.cpu.setRst75(true);
    d.cpu.regWrite(I8085::R_IM, 0);              // unmasked, but IE off
    d.run(5);
    ASSERT(d.cpu.isHalted());
    d.cpu.setTrap(true);
    d.run(1);
    ASSERT(!d.cpu.isHalted());
    EXPECT_HEX(d.reg(I8085::R_PC), 0x0024);
}

// Snapshots
TEST_CASE(i8085_state_roundtrip) {
    Rig r;
    r.setReg(I8085::R_AF, 0x12D7);
    r.setReg(I8085::R_BC, 0x3456);
    r.setReg(I8085::R_IM, I8085::IM_IE | 2);
    r.cpu.setRst75(true);
    std::vector<uint8_t> buf(r.cpu.stateSize());
    r.cpu.saveState(buf.data());
    I8085 c;
    c.loadState(buf.data());
    EXPECT_HEX(c.regRead(I8085::R_AF), 0x12D7);
    EXPECT_HEX(c.regRead(I8085::R_BC), 0x3456);
    EXPECT_HEX(c.rimValue() & 0x7F, I8085::IM_I75 | I8085::IM_IE | 2);
    EXPECT_EQ(c.regRead(I8085::R_PC), r.cpu.regRead(I8085::R_PC));
}

// Every opcode disassembles and assembles back to the same bytes
TEST_CASE(i8085_disasm_asm_roundtrip) {
    Rig r;
    Assembler8085 as;
    for (int op = 0; op < 256; ++op) {
        r.mem.write8(0x1000, (uint8_t)op);
        r.mem.write8(0x1001, 0x34);
        r.mem.write8(0x1002, 0x12);
        char text[32];
        const int len = r.cpu.disassembleOne(&r.mem, 0x1000, text, sizeof text);
        uint8_t out[4] = {};
        const int n = as.assembleLine(text, out, sizeof out, 0x1000);
        if (n != len || out[0] != op || (len > 1 && out[1] != 0x34) || (len > 2 && out[2] != 0x12))
            std::fprintf(stderr, "  $%02X: \"%s\" -> %d bytes\n", op, text, n);
        ASSERT_EQ(n, len);
        ASSERT_EQ(out[0], op);
    }
    // Spot checks
    auto dis = [&](std::vector<uint8_t> b) {
        for (size_t i = 0; i < b.size(); ++i) r.mem.write8(0x2000 + (uint32_t)i, b[i]);
        char t[32];
        r.cpu.disassembleOne(&r.mem, 0x2000, t, sizeof t);
        return std::string(t);
    };
    EXPECT_EQ(dis({0x41}), "MOV B,C");
    EXPECT_EQ(dis({0x21, 0x34, 0x12}), "LXI H,$1234");
    EXPECT_EQ(dis({0x36, 0x7F}), "MVI M,$7F");
    EXPECT_EQ(dis({0xF5}), "PUSH PSW");
    EXPECT_EQ(dis({0x08}), "DSUB");
    EXPECT_EQ(dis({0x28, 0x10}), "LDHI $10");
    EXPECT_EQ(dis({0xFD, 0x00, 0x80}), "JK $8000");
    EXPECT_EQ(dis({0xFF}), "RST 7");
    EXPECT_EQ(dis({0x20}), "RIM");
    auto asmb = [&](const char* line) {
        uint8_t b[4] = {};
        const int n = as.assembleLine(line, b, sizeof b, 0);
        std::vector<uint8_t> v(b, b + (n > 0 ? n : 0));
        return n < 0 ? std::vector<uint8_t>{0xEE, 0xEE} : v;
    };
    ASSERT(asmb("mvi a, 0FFh") == (std::vector<uint8_t>{0x3E, 0xFF}));
    ASSERT(asmb("  JX5 1234H ; comment") == (std::vector<uint8_t>{0xFD, 0x34, 0x12}));
    ASSERT(asmb("RST 38H") == (std::vector<uint8_t>{0xFF}));
    ASSERT(asmb("ASHR") == (std::vector<uint8_t>{0x10}));
    ASSERT(asmb("DW $1234,5") == (std::vector<uint8_t>{0x34, 0x12, 0x05, 0x00}));
    ASSERT(asmb("MOV M,M") == (std::vector<uint8_t>{0xEE, 0xEE}));
    ASSERT(asmb("MVI B,256") == (std::vector<uint8_t>{0xEE, 0xEE}));

    // The debugger's view
    DisasmEntry e;
    r.mem.write8(0x2000, 0xCD); r.mem.write8(0x2001, 0x00); r.mem.write8(0x2002, 0x30);
    r.cpu.disassembleEntry(&r.mem, 0x2000, &e);
    ASSERT(e.isCall && !e.isIllegal);
    EXPECT_HEX(e.targetAddr, 0x3000);
    r.mem.write8(0x2000, 0xCB);
    r.cpu.disassembleEntry(&r.mem, 0x2000, &e);
    ASSERT(e.isCall && e.isIllegal);
    r.mem.write8(0x2000, 0xD8);
    ASSERT(r.cpu.isReturnAt(&r.mem, 0x2000));
}

// ---------------------------------------------------------------------------
// The CP/M CPU test programs
// ---------------------------------------------------------------------------

namespace {

/**
 * Run a CP/M .COM at $0100 with a BDOS stub: CALL 5 goes to $FE00 (also the
 * top of the TPA, at $0006), where functions 2 (print E) and 9 (print the
 * $-terminated string at DE) are done before its RET; a jump to 0 ends it.
 */
std::string runCom(const std::string& path, uint64_t maxCycles, uint64_t* cyclesOut = nullptr, bool echo = false) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "(missing)";
    std::vector<uint8_t> com((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    Rig r;
    r.load(0x0100, com);
    r.mem.write8(0x0000, 0x76);                          // warm boot: HLT
    r.mem.write8(0x0005, 0xC3); r.mem.write8(0x0006, 0x00); r.mem.write8(0x0007, 0xFE);
    r.mem.write8(0xFE00, 0xC9);
    r.setReg(I8085::R_SP, 0xFDF0);
    std::string out;
    auto put = [&](char c) { out += c; if (echo) { std::fputc(c, stdout); std::fflush(stdout); } };
    uint64_t t = 0;
    while (t < maxCycles) {
        const uint16_t pc = (uint16_t)r.cpu.pc();
        if (pc == 0x0000) break;
        if (pc == 0xFE00) {
            const uint16_t bc = r.reg(I8085::R_BC);
            const uint16_t de = r.reg(I8085::R_DE);
            if ((bc & 0xFF) == 2) put((char)(de & 0xFF));
            else if ((bc & 0xFF) == 9) {
                for (uint16_t a = de; r.mem.peek8(a) != '$' && out.size() < 1000000; ++a) put((char)r.mem.peek8(a));
            }
        }
        t += (uint64_t)r.cpu.step();
        if (r.cpu.isHalted()) break;
    }
    if (cyclesOut) *cyclesOut = t;
    return out;
}

std::string testDir() {
    const char* v = std::getenv("MMEMU_8080_TESTS");
    return v ? v : "";
}

}  // namespace

// Kelly Smith's TST8080, the exerciser's preliminary tests, SuperSoft's CPUTEST (its 8080-model phase reported)
TEST_CASE(i8085_cpm_diagnostics) {
    const std::string dir = testDir();
    if (dir.empty()) { std::printf("  (skipped: MMEMU_8080_TESTS not set)\n"); return; }
    const std::string tst = runCom(dir + "/TST8080.COM", 100000000);
    if (tst.find("CPU IS OPERATIONAL") == std::string::npos) std::printf("%s\n", tst.c_str());
    ASSERT(tst.find("CPU IS OPERATIONAL") != std::string::npos);
    const std::string pre = runCom(dir + "/8080PRE.COM", 100000000);
    if (pre.find("complete") == std::string::npos) std::printf("%s\n", pre.c_str());
    ASSERT(pre.find("8080 Preliminary tests complete") != std::string::npos);
    // CPUTEST: the lettered tests A-Z must pass. Its exhaustive phase after the
    // timing test checks against an 8080 model with flag bit 1 always 1; on the
    // 8085 bit 1 is V, so it reports (e.g.) INR B from $16 with F bit 1 clear.
    // That result is printed, not asserted.
    std::string cpu = runCom(dir + "/CPUTEST.COM", 1000000000ull);
    for (auto& c : cpu) if (c == '\r') c = ' ';
    ASSERT(cpu.find("ABCDEFGHIJKLMNOPQRSTUVWXYZ") != std::string::npos);
    ASSERT(cpu.find("CPU IS 8080/8085") != std::string::npos);
    const size_t at = cpu.find("END TIMING TEST");
    std::printf("  CPUTEST after the timing test: %s\n",
                at == std::string::npos ? "(not reached)" : cpu.substr(at + 15).c_str());
}

// Ian Bartholomew's 8080/8085 CPU Exerciser with a real 8085's CRCs (flag mask $D5)
TEST_CASE(i8085_cpm_exerciser) {
    const std::string dir = testDir();
    if (dir.empty()) { std::printf("  (skipped: MMEMU_8080_TESTS not set)\n"); return; }
    const auto start = std::chrono::steady_clock::now();
    uint64_t t = 0;
    const std::string out = runCom(dir + "/8085EXM.COM", 100000000000ull, &t, std::getenv("MMEMU_8080_SHOW") != nullptr);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("  8085EXM: %llu T-states in %.1f s\n", (unsigned long long)t, secs);
    if (out.find("ERROR") != std::string::npos || out.find("Tests complete") == std::string::npos)
        std::printf("%s\n", out.c_str());
    ASSERT(out.find("Tests complete") != std::string::npos);
    ASSERT(out.find("ERROR") == std::string::npos);

    // The 8080's CRCs (all flag bits): reported, not asserted
    if (std::getenv("MMEMU_8080_EXM")) {
        const std::string exm = runCom(dir + "/8080EXM.COM", 100000000000ull);
        std::printf("  8080EXM (8080 CRCs):\n%s\n", exm.c_str());
    }
}
