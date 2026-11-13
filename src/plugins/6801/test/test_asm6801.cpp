// The 6803 line assembler: every documented opcode the disassembler writes
// assembles back to the same bytes; the syntax's alternatives.

#include "test_harness.h"
#include "plugins/6801/main/asm6801.h"
#include "plugins/6801/main/cpu6801.h"
#include "libmem/main/memory_bus.h"
#include "libtoolchain/main/idisasm.h"
#include <string>
#include <vector>

TEST_CASE(asm6801_round_trip) {
    FlatMemoryBus mem("system", 16);
    M6801 cpu;
    Assembler6801 as;
    int checked = 0;
    for (int op = 0; op < 256; ++op) {
        const uint32_t at = 0x2000;
        mem.write8(at, (uint8_t)op);
        mem.write8(at + 1, 0x12);
        mem.write8(at + 2, 0x34);
        DisasmEntry e;
        const int len = cpu.disassembleEntry(&mem, at, &e);
        if (e.isIllegal) continue;
        uint8_t buf[8] = {};
        const int n = as.assembleLine(e.complete, buf, sizeof buf, at);
        if (n != len) std::fprintf(stderr, "asm6801: %02X '%s' -> %d bytes, want %d\n", op, e.complete.c_str(), n, len);
        ASSERT_EQ(n, len);
        for (int i = 0; i < n; ++i) EXPECT_EQ(buf[i], mem.peek8(at + (uint32_t)i));
        ++checked;
    }
    EXPECT_TRUE(checked > 200);
}

TEST_CASE(asm6801_syntax) {
    Assembler6801 as;
    uint8_t b[8];
    EXPECT_EQ(as.assembleLine("  lda #'A'  ; comment", b, 8, 0), 2);
    EXPECT_EQ(b[0], 0x86); EXPECT_EQ(b[1], 0x41);
    EXPECT_EQ(as.assembleLine("LDAB ,X", b, 8, 0), 2);
    EXPECT_EQ(b[0], 0xE6); EXPECT_EQ(b[1], 0x00);
    EXPECT_EQ(as.assembleLine("STAA >$12", b, 8, 0), 3);
    EXPECT_EQ(b[0], 0xB7);
    EXPECT_EQ(as.assembleLine("JMP $12", b, 8, 0), 3);     // no direct JMP
    EXPECT_EQ(b[0], 0x7E);
    EXPECT_EQ(as.assembleLine("LSLD", b, 8, 0), 1);
    EXPECT_EQ(b[0], 0x05);
    EXPECT_EQ(as.assembleLine("BNE $1000", b, 8, 0x1010), 2);
    EXPECT_EQ(b[1], 0xEE);
    EXPECT_EQ(as.assembleLine("BRA $2000", b, 8, 0x1000), -1);   // out of range
    EXPECT_EQ(as.assembleLine("LDD #4660", b, 8, 0), 3);
    EXPECT_EQ(b[1], 0x12); EXPECT_EQ(b[2], 0x34);
    EXPECT_EQ(as.assembleLine("FCB 1,$FF,%101", b, 8, 0), 3);
    EXPECT_EQ(b[2], 5);
    EXPECT_EQ(as.assembleLine("FDB $1234,$5678", b, 8, 0), 4);
    EXPECT_EQ(b[3], 0x78);
    EXPECT_EQ(as.assembleLine("STAA #1", b, 8, 0), -1);
    EXPECT_EQ(as.assembleLine("XYZ", b, 8, 0), -1);
    EXPECT_EQ(as.assembleLine("   ; nothing", b, 8, 0), 0);
}
