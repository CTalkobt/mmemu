#include "test_harness.h"
#include "plugins/6502/main/assembler_6502.h"
#include "libmem/main/memory_bus.h"
#include <cstring>

TEST_CASE(assembler6502_basic) {
    Assembler6502 assem;
    uint8_t buf[16];
    int sz;

    // Immediate
    sz = assem.assembleLine("LDA #$42", buf, sizeof(buf));
    ASSERT(sz == 2);
    ASSERT(buf[0] == 0xA9);
    ASSERT(buf[1] == 0x42);

    // Zero Page
    sz = assem.assembleLine("STA $80", buf, sizeof(buf));
    ASSERT(sz == 2);
    ASSERT(buf[0] == 0x85);
    ASSERT(buf[1] == 0x80);

    // Absolute
    sz = assem.assembleLine("JMP $1234", buf, sizeof(buf));
    ASSERT(sz == 3);
    ASSERT(buf[0] == 0x4C);
    ASSERT(buf[1] == 0x34);
    ASSERT(buf[2] == 0x12);

    // Implicit
    sz = assem.assembleLine("INX", buf, sizeof(buf));
    ASSERT(sz == 1);
    ASSERT(buf[0] == 0xE8);
}

TEST_CASE(assembler6502_addressing_modes) {
    Assembler6502 assem;
    uint8_t buf[16];
    int sz;

    // Accumulator
    sz = assem.assembleLine("ASL A", buf, sizeof(buf));
    ASSERT(sz == 1);
    ASSERT(buf[0] == 0x0A);

    // Indexed X
    sz = assem.assembleLine("LDA $1234,X", buf, sizeof(buf));
    ASSERT(sz == 3);
    ASSERT(buf[0] == 0xBD);

    // Indexed Y
    sz = assem.assembleLine("LDX $1234,Y", buf, sizeof(buf));
    ASSERT(sz == 3);
    ASSERT(buf[0] == 0xBE);

    // Indirect
    sz = assem.assembleLine("JMP ($1234)", buf, sizeof(buf));
    ASSERT(sz == 3);
    ASSERT(buf[0] == 0x6C);

    // Indexed Indirect (X)
    sz = assem.assembleLine("LDA ($80,X)", buf, sizeof(buf));
    ASSERT(sz == 2);
    ASSERT(buf[0] == 0xA1);

    // Indirect Indexed (Y)
    sz = assem.assembleLine("LDA ($80),Y", buf, sizeof(buf));
    ASSERT(sz == 2);
    ASSERT(buf[0] == 0xB1);
}

TEST_CASE(assembler6502_relative) {
    Assembler6502 assem;
    uint8_t buf[16];
    int sz;

    // Forward branch
    // BEQ $100A from $1000. PC after BEQ is $1002. Offset = 100A - 1002 = 8.
    sz = assem.assembleLine("BEQ $100A", buf, sizeof(buf), 0x1000);
    ASSERT(sz == 2);
    ASSERT(buf[0] == 0xF0);
    ASSERT(buf[1] == 0x08);

    // Backward branch
    // BNE $1000 from $1006. PC after BNE is $1008. Offset = 1000 - 1008 = -8 (0xF8).
    sz = assem.assembleLine("BNE $1000", buf, sizeof(buf), 0x1006);
    ASSERT(sz == 2);
    ASSERT(buf[0] == 0xD0);
    ASSERT(buf[1] == 0xF8);
}

TEST_CASE(assembler6502_illegals) {
    Assembler6502 assem;
    uint8_t buf[16];
    int sz;

    // SLO ZP
    sz = assem.assembleLine("SLO $10", buf, sizeof(buf));
    ASSERT(sz == 2);
    ASSERT(buf[0] == 0x07);

    // LAX ABS
    sz = assem.assembleLine("LAX $1234", buf, sizeof(buf));
    ASSERT(sz == 3);
    ASSERT(buf[0] == 0xAF);
}

#include "plugins/6502/main/disassembler_6502.h"
#include <cstdio>

// Every 65C02 opcode the disassembler names assembles back to the same bytes
TEST_CASE(assembler65c02_round_trips_every_opcode) {
    Assembler6502 assem(true);
    Disassembler6502 dis(nullptr, true);
    ASSERT(assem.isaSupported("65C02"));
    FlatMemoryBus bus("t", 16);
    int checked = 0;
    for (int op = 0; op < 256; op++) {
        const uint32_t pc = 0x1000;
        bus.write8(pc, (uint8_t)op);
        bus.write8(pc + 1, 0x12);
        bus.write8(pc + 2, 0x10);       // BBRn/BBSn: forward branch
        DisasmEntry e;
        int len = dis.disasmEntry(&bus, pc, e);
        if (e.mnemonic == "NOP" && op != 0xEA) continue;   // reserved: multi-byte NOPs
        std::string line = e.mnemonic + (e.operands.empty() ? "" : " " + e.operands);
        uint8_t buf[4] = {};
        int sz = assem.assembleLine(line, buf, sizeof buf, pc);
        if (sz != len || buf[0] != (uint8_t)op ||
            (len > 1 && buf[1] != bus.read8(pc + 1)) || (len > 2 && buf[2] != bus.read8(pc + 2))) {
            printf("    $%02X '%s': assembled %d bytes %02X %02X %02X, expected %d\n",
                   op, line.c_str(), sz, buf[0], buf[1], buf[2], len);
            ASSERT(false);
        }
        checked++;
    }
    ASSERT(checked >= 210);
}

TEST_CASE(assembler6502_nmos_rejects_cmos) {
    Assembler6502 nmos;
    uint8_t buf[4];
    ASSERT_EQ(nmos.assembleLine("STZ $10", buf, sizeof buf), -1);
    ASSERT_EQ(nmos.assembleLine("LDA ($10)", buf, sizeof buf), -1);
    Assembler6502 cmos(true);
    ASSERT_EQ(cmos.assembleLine("LAX $10", buf, sizeof buf), -1);
    ASSERT_EQ(cmos.assembleLine("INC", buf, sizeof buf), 1);
    ASSERT_EQ((int)buf[0], 0x1A);
    ASSERT_EQ(cmos.assembleLine("BBS7 $FF,$0FFF", buf, sizeof buf, 0x1000), 3);
    ASSERT_EQ((int)buf[2], 0xFC);
}
