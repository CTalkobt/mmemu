// Acorn ARM1/ARM2 core (#13). The instruction set was also checked against
// b-em's arm.c (from Arculator) on random states (see doc/README-ARM.md).

#include "test_harness.h"
#include "plugins/arm/main/cpu_arm2.h"
#include "libmem/main/memory_bus.h"
#include <string>
#include <vector>

namespace {
struct Rig {
    FlatMemoryBus mem{"system", 22};
    Arm2 cpu{false};
    Rig() { cpu.setDataBus(&mem); }
    void load(uint32_t at, std::vector<uint32_t> code) {
        for (size_t i = 0; i < code.size(); ++i)
            for (int b = 0; b < 4; ++b) mem.write8((uint32_t)(at + i * 4 + b), (code[i] >> (8 * b)) & 0xFF);
        cpu.reset();
        cpu.setPc(at);
    }
    int run(int n) { int t = 0; for (int i = 0; i < n; ++i) t += cpu.step(); return t; }
};
}  // namespace

// Data processing, the barrel shifter's carry, conditions, R15 reads 8 ahead
TEST_CASE(arm2_data_processing) {
    Rig r;
    r.load(0x1000, {0xE3A00001,          // MOV R0,#1
                    0xE1B01F80,          // MOVS R1,R0,LSL #31   -> $80000000, N
                    0xE0912001,          // ADDS R2,R1,R1        -> 0, Z C V
                    0x03A03007,          // MOVEQ R3,#7
                    0x13A03009,          // MOVNE R3,#9 (skipped)
                    0xE1A0400F,          // MOV R4,PC            -> $1014 + 8 | PSR
                    0xE3E05000,          // MVN R5,#0
                    0xE1B06065});        // MOVS R6,R5,RRX       -> C in, bit 0 out
    r.run(2);
    EXPECT_EQ(r.cpu.reg(1), 0x80000000u);
    EXPECT_TRUE(r.cpu.psr() & Arm2::FLAG_N);
    r.run(1);
    EXPECT_EQ(r.cpu.reg(2), 0u);
    EXPECT_EQ(r.cpu.psr() & 0xF0000000, Arm2::FLAG_Z | Arm2::FLAG_C | Arm2::FLAG_V);
    r.run(2);
    EXPECT_EQ(r.cpu.reg(3), 7u);
    r.run(1);
    EXPECT_EQ(r.cpu.reg(4) & Arm2::PC_MASK, 0x101Cu);
    EXPECT_EQ(r.cpu.reg(4) & 3, (uint32_t)Arm2::SVC);
    r.run(2);
    EXPECT_EQ(r.cpu.reg(6), 0xFFFFFFFFu);                // C was set: RRX brings it in
    EXPECT_TRUE(r.cpu.psr() & Arm2::FLAG_C);
}

// LDR/STR (rotated unaligned loads, writeback), LDM/STM, BL and MOVS PC,R14
TEST_CASE(arm2_memory_and_branches) {
    Rig r;
    r.mem.write8(0x2000, 0x11); r.mem.write8(0x2001, 0x22); r.mem.write8(0x2002, 0x33); r.mem.write8(0x2003, 0x44);
    r.load(0x1000, {0xE3A01A02,          // MOV R1,#&2000
                    0xE5912001,          // LDR R2,[R1,#1]       -> $11443322 (rotated)
                    0xE5F13004,          // LDRB R3,[R1,#4]!     -> R1 = $2004
                    0xE3A0D902,          // MOV R13,#&8000
                    0xE92D400E,          // STMFD R13!,{R1-R3,R14}
                    0xEB000002,          // BL +2 words ahead (to $1024)
                    0xE8BD400E,          // LDMFD R13!,{R1-R3,R14}
                    0xEAFFFFFE,          // B .
                    0x00000000,
                    0xE3A00042,          // $1024: MOV R0,#&42
                    0xE1B0F00E});        // MOVS PC,R14
    r.run(3);
    EXPECT_EQ(r.cpu.reg(2), 0x11443322u);
    EXPECT_EQ(r.cpu.reg(1), 0x2004u);
    r.run(2);
    EXPECT_EQ(r.cpu.reg(13), 0x8000u - 16);
    EXPECT_EQ(r.mem.peek8(0x8000 - 16), 0x04);           // R1's low byte first
    r.run(1);                                            // BL
    EXPECT_EQ(r.cpu.pc(), 0x1024u);
    EXPECT_EQ(r.cpu.reg(14) & Arm2::PC_MASK, 0x1018u);
    r.run(2);                                            // MOV, MOVS PC,R14
    EXPECT_EQ(r.cpu.reg(0), 0x42u);
    EXPECT_EQ(r.cpu.pc(), 0x1018u);
    r.run(1);
    EXPECT_EQ(r.cpu.reg(13), 0x8000u);
    EXPECT_TRUE(r.cpu.isProgramEnd(&r.mem));
}

// Modes and exceptions: SWI to supervisor, TEQP to user mode, IRQ and FIQ
// with their banked registers; the ARM1 has no MUL (undefined)
TEST_CASE(arm2_modes_and_exceptions) {
    Rig r;
    r.load(0x1000, {0xE3A0D901,          // MOV R13,#&4000 (supervisor's R13)
                    0xE33FF000,          // TEQP PC,#0: user mode, interrupts on
                    0xE3A0D902,          // MOV R13,#&8000 (user R13)
                    0xEF000010,          // SWI &10
                    0xE1A00000,          // NOP (MOV R0,R0)
                    0xE1A00000});
    r.load(0x8, {0xE1B0F00E});           // SWI handler at 8: MOVS PC,R14
    r.load(0x18, {0xE25EF004});          // IRQ handler: SUBS PC,R14,#4
    r.load(0x1C, {0xE25EF004});          // FIQ handler
    r.cpu.setPc(0x1000);
    r.run(2);
    EXPECT_EQ(r.cpu.mode(), Arm2::USR);
    r.run(1);
    r.run(1);                                            // SWI
    EXPECT_EQ(r.cpu.mode(), Arm2::SVC);
    EXPECT_EQ(r.cpu.reg(13), 0x4000u);                   // the supervisor's own R13
    EXPECT_EQ(r.cpu.pc(), 0x8u);
    r.run(1);                                            // MOVS PC,R14: back to user mode
    EXPECT_EQ(r.cpu.mode(), Arm2::USR);
    EXPECT_EQ(r.cpu.reg(13), 0x8000u);
    EXPECT_EQ(r.cpu.pc(), 0x1010u);
    r.cpu.setIrqLine(true);
    r.run(1);
    EXPECT_EQ(r.cpu.mode(), Arm2::IRQ);
    EXPECT_EQ(r.cpu.pc(), 0x18u);
    r.cpu.setIrqLine(false);
    r.run(1);                                            // SUBS PC,R14,#4
    EXPECT_EQ(r.cpu.mode(), Arm2::USR);
    EXPECT_EQ(r.cpu.pc(), 0x1010u);
    r.cpu.setFiqLine(true);
    r.run(1);
    EXPECT_EQ(r.cpu.mode(), Arm2::FIQ);
    EXPECT_TRUE(r.cpu.psr() & Arm2::FLAG_F);
    r.cpu.setFiqLine(false);

    // MUL on the ARM2; undefined on the ARM1
    Rig a2;
    a2.load(0x1000, {0xE3A01006, 0xE3A02007, 0xE0030291});   // MOV R1,#6; MOV R2,#7; MUL R3,R1,R2
    a2.run(3);
    EXPECT_EQ(a2.cpu.reg(3), 42u);
    FlatMemoryBus m1("m1", 22);
    Arm2 arm1(true);
    arm1.setDataBus(&m1);
    const uint32_t mul = 0xE0030291;
    for (int b = 0; b < 4; ++b) m1.write8(0x1000 + b, (mul >> (8 * b)) & 0xFF);
    arm1.setPc(0x1000);
    arm1.step();
    EXPECT_EQ(arm1.pc(), 0x4u);                          // the undefined-instruction vector
}

TEST_CASE(arm2_disassembler) {
    Rig r;
    r.load(0x1000, {0xE0912001, 0xE5F13004, 0xE92D400E, 0xEB000002, 0xE33FF000, 0x1A000000});
    char b[96];
    r.cpu.disassembleOne(&r.mem, 0x1000, b, sizeof b); EXPECT_EQ(std::string(b), std::string("ADDS R2,R1,R1"));
    r.cpu.disassembleOne(&r.mem, 0x1004, b, sizeof b); EXPECT_EQ(std::string(b), std::string("LDRB R3,[R1,#&4]!"));
    r.cpu.disassembleOne(&r.mem, 0x1008, b, sizeof b); EXPECT_EQ(std::string(b), std::string("STMDB R13!,{R1,R2,R3,R14}"));
    r.cpu.disassembleOne(&r.mem, 0x100C, b, sizeof b); EXPECT_EQ(std::string(b), std::string("BL &101C"));
    r.cpu.disassembleOne(&r.mem, 0x1010, b, sizeof b); EXPECT_EQ(std::string(b), std::string("TEQP PC,#&0"));
    r.cpu.disassembleOne(&r.mem, 0x1014, b, sizeof b); EXPECT_EQ(std::string(b), std::string("BNE &101C"));
}
