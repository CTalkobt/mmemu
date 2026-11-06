#include "test_harness.h"
#include "plugins/z80/main/cpu_z80.h"
#include "libmem/main/memory_bus.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace {
struct Rig {
    FlatMemoryBus mem{"system", 16};
    FlatMemoryBus io{"io", 16};
    Z80 cpu;
    Rig() { cpu.setDataBus(&mem); cpu.setIoBus(&io); cpu.reset(); }
    void load(uint16_t at, std::vector<uint8_t> code) {
        for (size_t i = 0; i < code.size(); ++i) mem.write8((uint32_t)(at + i), code[i]);
        cpu.setPc(at);
    }
    int run(int n) { int t = 0; for (int i = 0; i < n; ++i) t += cpu.step(); return t; }
};
}  // namespace

// Loads, ALU flags, 16-bit arithmetic and T-states
TEST_CASE(z80_basic_instructions) {
    Rig r;
    r.load(0x0000, {0x3E, 0x7F,          // LD A,$7F         7
                    0xC6, 0x01,          // ADD A,1          7  -> $80: S, H, V
                    0x21, 0x34, 0x12,    // LD HL,$1234     10
                    0x01, 0xFF, 0xFF,    // LD BC,$FFFF     10
                    0x09,                // ADD HL,BC       11  -> $1233, C
                    0x76});              // HALT
    EXPECT_EQ(r.run(2), 14);
    EXPECT_EQ(r.cpu.a(), 0x80);
    EXPECT_EQ(r.cpu.f() & 0xD7, Z80::FS | Z80::FH | Z80::FP);
    EXPECT_EQ(r.run(3), 31);
    EXPECT_EQ(r.cpu.hl(), 0x1233);
    EXPECT_EQ(r.cpu.f() & Z80::FC, Z80::FC);
    r.run(1);
    ASSERT(r.cpu.isHalted());
}

// IX with a displacement, the undocumented IXH, DDCB with a register copy
TEST_CASE(z80_index_registers) {
    Rig r;
    r.mem.write8(0x2005, 0x41);
    r.load(0x0000, {0xDD, 0x21, 0x00, 0x20,   // LD IX,$2000
                    0xDD, 0x7E, 0x05,         // LD A,(IX+5)       19
                    0xDD, 0x26, 0x99,         // LD IXH,$99
                    0xDD, 0xCB, 0x05, 0x00,   // RLC (IX+5),B      23
                    0xDD, 0x36, 0xFE, 0x55}); // LD (IX-2),$55     19
    r.run(1);
    EXPECT_EQ(r.run(1), 19);
    EXPECT_EQ(r.cpu.a(), 0x41);
    r.run(1);
    EXPECT_EQ(r.cpu.regRead(Z80::R_IX), 0x9900);
    // IX is now $9900: RLC ($9905)
    r.mem.write8(0x9905, 0x81);
    EXPECT_EQ(r.run(1), 23);
    EXPECT_EQ(r.mem.peek8(0x9905), 0x03);
    EXPECT_EQ(r.cpu.bc() >> 8, 0x03);
    EXPECT_EQ(r.run(1), 19);
    EXPECT_EQ(r.mem.peek8(0x98FE), 0x55);
}

// Block copy, I/O with the 16-bit port, interrupts in mode 1 and 2
TEST_CASE(z80_block_io_interrupts) {
    Rig r;
    for (int i = 0; i < 4; ++i) r.mem.write8(0x3000 + i, (uint8_t)(0x10 + i));
    r.load(0x0000, {0x21, 0x00, 0x30, 0x11, 0x00, 0x40, 0x01, 0x04, 0x00,   // HL, DE, BC
                    0xED, 0xB0,                                         // LDIR
                    0x01, 0x34, 0x12, 0x3E, 0x77, 0xED, 0x79,           // OUT (C),A at port $1234
                    0xFB, 0xED, 0x56, 0x00, 0x00});                     // EI, IM 1, NOP, NOP
    r.run(3);
    int t = 0;
    while (r.cpu.pc() == 0x0009) t += r.cpu.step();
    EXPECT_EQ(t, 21 * 3 + 16);
    EXPECT_EQ(r.mem.peek8(0x4003), 0x13);
    r.run(3);
    EXPECT_EQ(r.io.peek8(0x1234), 0x77);
    r.cpu.setIrqLine(true);
    r.run(1);                                                // EI: not right after it
    EXPECT_EQ(r.cpu.pc(), 0x0013);
    r.run(1);                                                // IM 1
    EXPECT_EQ(r.run(1), 13);                                 // the interrupt
    EXPECT_EQ(r.cpu.pc(), 0x0038);
    r.cpu.setIrqLine(false);
    // Mode 2: vector at I * 256 + $FF
    Rig r2;
    r2.mem.write8(0x12FF, 0x00); r2.mem.write8(0x1300, 0x50);
    r2.load(0x0000, {0x3E, 0x12, 0xED, 0x47, 0xED, 0x5E, 0xFB, 0x00, 0x00});   // LD A,$12; LD I,A; IM 2; EI
    r2.run(4);
    r2.cpu.setIrqLine(true);
    r2.run(2);                                               // the instruction after EI, then the interrupt
    EXPECT_EQ(r2.cpu.pc(), 0x5000);
}

TEST_CASE(z80_disassembler) {
    Rig r;
    r.load(0x0000, {0xDD, 0xCB, 0xFE, 0x16, 0xED, 0xB0, 0x18, 0xFE, 0xFD, 0x7C});
    char buf[64];
    EXPECT_EQ(r.cpu.disassembleOne(&r.mem, 0, buf, sizeof buf), 4);
    EXPECT_EQ(std::string(buf), std::string("RL (IX-$02)"));
    r.cpu.disassembleOne(&r.mem, 4, buf, sizeof buf);
    EXPECT_EQ(std::string(buf), std::string("LDIR"));
    r.cpu.disassembleOne(&r.mem, 6, buf, sizeof buf);
    EXPECT_EQ(std::string(buf), std::string("JR $0006"));
    r.cpu.disassembleOne(&r.mem, 8, buf, sizeof buf);
    EXPECT_EQ(std::string(buf), std::string("LD A,IYH"));
}

namespace {
// Run a CP/M .com (BDOS 2 and 9 at address 5) to warm boot; the output text
std::string runCpm(const std::string& path, uint64_t maxSteps) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "(missing " + path + ")";
    std::vector<uint8_t> prog((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    Rig r;
    for (size_t i = 0; i < prog.size(); ++i) r.mem.write8((uint32_t)(0x100 + i), prog[i]);
    r.mem.write8(0x0000, 0x76);                              // warm boot: HALT
    r.mem.write8(0x0005, 0xC9);                              // BDOS: RET
    r.mem.write8(0x0006, 0x00); r.mem.write8(0x0007, 0xF0);  // top of the TPA
    r.cpu.regWrite(Z80::R_SP, 0xF000);
    r.cpu.setPc(0x100);
    std::string out;
    for (uint64_t i = 0; i < maxSteps && !r.cpu.isHalted(); ++i) {
        if (r.cpu.pc() == 0x0005) {
            uint8_t fn = (uint8_t)r.cpu.bc();
            if (fn == 2) out += (char)(r.cpu.de() & 0xFF);
            if (fn == 9) for (uint16_t a = r.cpu.de(); r.mem.peek8(a) != '$'; ++a) out += (char)r.mem.peek8(a);
        }
        r.cpu.step();
    }
    return out;
}
}  // namespace

// Frank Cringle's exercisers (tests/z80): every group must say OK. They run
// for minutes, so only with MMEMU_ZEX=1 (MMEMU_ZEX=doc or all for one).
TEST_CASE(z80_zex) {
    const char* env = std::getenv("MMEMU_ZEX");
    if (!env) { printf("  (skipped: set MMEMU_ZEX=1 to run ZEXDOC and ZEXALL)\n"); return; }
    std::string which = env;
    for (std::string name : {"zexdoc", "zexall"}) {
        if (which != "1" && which != name.substr(3)) continue;
        std::string out = runCpm("tests/z80/" + name + ".com", 20000000000ULL);
        printf("%s\n", out.c_str());
        ASSERT(out.find("Tests complete") != std::string::npos);
        ASSERT(out.find("ERROR") == std::string::npos);
    }
}
