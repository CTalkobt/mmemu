// The SM83 line assembler (sm83_asm.h): every opcode the disassembler writes
// reads back as the same bytes, the alternative spellings, and what it refuses.

#include "test_harness.h"
#include "plugins/machines/gameboy/main/sm83_asm.h"
#include "plugins/machines/gameboy/main/sm83_disasm.h"
#include "libmem/main/memory_bus.h"

#include <string>
#include <vector>

namespace {

std::vector<uint8_t> as(const std::string& line, uint16_t at = 0x0100) {
    AssemblerSm83 a;
    uint8_t buf[8];
    const int n = a.assembleLine(line, buf, sizeof buf, at);
    if (n < 0) return {0xEE, 0xEE, 0xEE, 0xEE};          // (an error: never a valid encoding)
    return std::vector<uint8_t>(buf, buf + n);
}

}  // namespace

TEST_CASE(sm83_asm_round_trip) {
    // Every opcode and every CB one, disassembled (operands $34 $12, at $0150) and assembled back
    FlatMemoryBus bus("t", 16);
    AssemblerSm83 a;
    int checked = 0;
    for (int prefix = 0; prefix < 2; ++prefix) {
        for (int op = 0; op < 256; ++op) {
            const uint8_t bytes[3] = {prefix ? (uint8_t)0xCB : (uint8_t)op, prefix ? (uint8_t)op : (uint8_t)0x34, 0x12};
            for (int i = 0; i < 3; ++i) bus.write8((uint32_t)(0x150 + i), bytes[i]);
            if (!prefix && op == 0x10) bus.write8(0x151, 0x00);      // STOP $00
            DisasmEntry e;
            const int len = gb::sm83Disassemble(&bus, 0x150, e);
            if (!prefix && op == 0xCB) continue;
            uint8_t buf[4];
            const int n = a.assembleLine(e.complete, buf, sizeof buf, 0x150);
            bool same = n == len;
            for (int i = 0; same && i < n; ++i) same = buf[i] == bus.peek8((uint32_t)(0x150 + i));
            if (!same) std::printf("  %s%02X \"%s\": %d bytes\n", prefix ? "CB " : "", op, e.complete.c_str(), n);
            EXPECT_TRUE(same);
            ++checked;
        }
    }
    EXPECT_EQ(checked, 511);
}

TEST_CASE(sm83_asm_spellings) {
    using V = std::vector<uint8_t>;
    EXPECT_TRUE(as("ld (hli),a") == V{0x22});
    EXPECT_TRUE(as("LDI (HL),A") == V{0x22});
    EXPECT_TRUE(as("LDI A,(HL)") == V{0x2A});
    EXPECT_TRUE(as("LD A,(HLD)") == V{0x3A});
    EXPECT_TRUE(as("LDD (HL),A") == V{0x32});
    EXPECT_TRUE(as("LDH ($44),A") == V({0xE0, 0x44}));
    EXPECT_TRUE(as("LDH A,($FF44)") == V({0xF0, 0x44}));
    EXPECT_TRUE(as("LD (C),A") == V{0xE2});
    EXPECT_TRUE(as("LDH A,(C)") == V{0xF2});
    EXPECT_TRUE(as("LD A,(0xFF00+C)") == V{0xF2});
    EXPECT_TRUE(as("ADD B") == V{0x80});
    EXPECT_TRUE(as("CP A,$10") == V({0xFE, 0x10}));
    EXPECT_TRUE(as("XOR A") == V{0xAF});
    EXPECT_TRUE(as("JP (HL)") == V{0xE9});
    EXPECT_TRUE(as("  ld   a , 0FFh   ; comment") == V({0x3E, 0xFF}));
    EXPECT_TRUE(as("LD A,%1010") == V({0x3E, 0x0A}));
    EXPECT_TRUE(as("LD A,-1") == V({0x3E, 0xFF}));
    EXPECT_TRUE(as("LD SP,57344") == V({0x31, 0x00, 0xE0}));
    EXPECT_TRUE(as("ADD SP,-2") == V({0xE8, 0xFE}));
    EXPECT_TRUE(as("LD HL,SP+5") == V({0xF8, 0x05}));
    EXPECT_TRUE(as("STOP $01") == V({0x10, 0x01}));
    EXPECT_TRUE(as("RET C") == V{0xD8});
    EXPECT_TRUE(as("JP C,$1234") == V({0xDA, 0x34, 0x12}));
    EXPECT_TRUE(as("JR $0100", 0x0100) == V({0x18, 0xFE}));
    EXPECT_TRUE(as("JR NC,$0181", 0x0100) == V({0x30, 0x7F}));
    EXPECT_TRUE(as("DB $D3,1,2") == V({0xD3, 0x01, 0x02}));
    EXPECT_TRUE(as("DW $1234") == V({0x34, 0x12}));
    EXPECT_TRUE(as("") == V{});
    EXPECT_TRUE(as("; only a comment") == V{});
}

TEST_CASE(sm83_asm_errors) {
    AssemblerSm83 a;
    uint8_t buf[4];
    for (const char* bad : {"LD (HL),(HL)", "LD A", "JR $0200", "RST $39", "BIT 8,A", "PUSH SP", "POP BC,DE",
                            "LDH ($FE00),A", "LD B,$100", "ADD SP,200", "FOO", "LD A,(BC", "LD X,1", "INC AF",
                            "JP NZ", "LD HL,SP+$90", "SWAP $10", "DB 256"}) {
        const int n = a.assembleLine(bad, buf, sizeof buf, 0x0100);
        if (n != -1) std::printf("  \"%s\" assembled to %d bytes\n", bad, n);
        EXPECT_EQ(n, -1);
    }
    EXPECT_EQ(a.assembleLine("LD BC,$1234", buf, 2, 0), -1);   // no room
    EXPECT_TRUE(a.isaSupported("sm83"));
    EXPECT_TRUE(!a.isaSupported("z80"));
}
