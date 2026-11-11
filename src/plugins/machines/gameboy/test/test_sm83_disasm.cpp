// The SM83 disassembler (sm83_disasm.h): a table of opcodes and their text,
// the lengths of all 256 opcodes, and the call/return/branch flags.

#include "test_harness.h"
#include "plugins/machines/gameboy/main/sm83_disasm.h"
#include "libmem/main/memory_bus.h"
#include "libtoolchain/main/symbol_table.h"

#include <string>
#include <vector>

namespace {

std::string dis(const std::vector<uint8_t>& bytes, uint16_t at = 0x0100, int* len = nullptr, DisasmEntry* out = nullptr,
                const SymbolTable* syms = nullptr) {
    FlatMemoryBus bus("t", 16);
    for (size_t i = 0; i < bytes.size(); ++i) bus.write8((uint32_t)(at + i), bytes[i]);
    DisasmEntry e;
    const int n = gb::sm83Disassemble(&bus, at, e, syms);
    if (len) *len = n;
    if (out) *out = e;
    return e.complete;
}

}  // namespace

TEST_CASE(sm83_disasm_table) {
    struct Row { std::vector<uint8_t> bytes; const char* text; int len; };
    const Row rows[] = {
        {{0x00}, "NOP", 1},
        {{0x01, 0x34, 0x12}, "LD BC,$1234", 3},
        {{0x02}, "LD (BC),A", 1},
        {{0x08, 0x00, 0xC0}, "LD ($C000),SP", 3},
        {{0x09}, "ADD HL,BC", 1},
        {{0x0A}, "LD A,(BC)", 1},
        {{0x10, 0x00}, "STOP", 2},
        {{0x18, 0xFE}, "JR $0100", 2},
        {{0x20, 0x05}, "JR NZ,$0107", 2},
        {{0x38, 0x80}, "JR C,$0082", 2},
        {{0x22}, "LD (HL+),A", 1},
        {{0x2A}, "LD A,(HL+)", 1},
        {{0x32}, "LD (HL-),A", 1},
        {{0x3A}, "LD A,(HL-)", 1},
        {{0x27}, "DAA", 1},
        {{0x2F}, "CPL", 1},
        {{0x33}, "INC SP", 1},
        {{0x3B}, "DEC SP", 1},
        {{0x34}, "INC (HL)", 1},
        {{0x36, 0x99}, "LD (HL),$99", 2},
        {{0x3E, 0x42}, "LD A,$42", 2},
        {{0x41}, "LD B,C", 1},
        {{0x76}, "HALT", 1},
        {{0x7E}, "LD A,(HL)", 1},
        {{0x86}, "ADD A,(HL)", 1},
        {{0x8F}, "ADC A,A", 1},
        {{0x90}, "SUB B", 1},
        {{0x9E}, "SBC A,(HL)", 1},
        {{0xA7}, "AND A", 1},
        {{0xAF}, "XOR A", 1},
        {{0xB1}, "OR C", 1},
        {{0xBE}, "CP (HL)", 1},
        {{0xC0}, "RET NZ", 1},
        {{0xC1}, "POP BC", 1},
        {{0xC2, 0x00, 0x40}, "JP NZ,$4000", 3},
        {{0xC3, 0x50, 0x01}, "JP $0150", 3},
        {{0xC4, 0x00, 0x20}, "CALL NZ,$2000", 3},
        {{0xC5}, "PUSH BC", 1},
        {{0xC6, 0x01}, "ADD A,$01", 2},
        {{0xC7}, "RST $00", 1},
        {{0xC9}, "RET", 1},
        {{0xCD, 0x34, 0x12}, "CALL $1234", 3},
        {{0xCE, 0x10}, "ADC A,$10", 2},
        {{0xD6, 0x01}, "SUB $01", 2},
        {{0xD9}, "RETI", 1},
        {{0xDC, 0x00, 0x30}, "CALL C,$3000", 3},
        {{0xDE, 0x02}, "SBC A,$02", 2},
        {{0xE0, 0x40}, "LDH ($FF40),A", 2},
        {{0xE2}, "LD ($FF00+C),A", 1},
        {{0xE6, 0x0F}, "AND $0F", 2},
        {{0xE8, 0xFE}, "ADD SP,-$02", 2},
        {{0xE9}, "JP HL", 1},
        {{0xEA, 0x00, 0xC0}, "LD ($C000),A", 3},
        {{0xEE, 0xFF}, "XOR $FF", 2},
        {{0xF0, 0x44}, "LDH A,($FF44)", 2},
        {{0xF1}, "POP AF", 1},
        {{0xF2}, "LD A,($FF00+C)", 1},
        {{0xF3}, "DI", 1},
        {{0xF5}, "PUSH AF", 1},
        {{0xF6, 0x80}, "OR $80", 2},
        {{0xF8, 0x05}, "LD HL,SP+$05", 2},
        {{0xF9}, "LD SP,HL", 1},
        {{0xFA, 0x10, 0xC0}, "LD A,($C010)", 3},
        {{0xFB}, "EI", 1},
        {{0xFE, 0x90}, "CP $90", 2},
        {{0xFF}, "RST $38", 1},
        {{0xD3}, "DB $D3", 1},
        {{0xFD}, "DB $FD", 1},
        // CB prefixed
        {{0xCB, 0x00}, "RLC B", 2},
        {{0xCB, 0x0E}, "RRC (HL)", 2},
        {{0xCB, 0x11}, "RL C", 2},
        {{0xCB, 0x1F}, "RR A", 2},
        {{0xCB, 0x27}, "SLA A", 2},
        {{0xCB, 0x2A}, "SRA D", 2},
        {{0xCB, 0x37}, "SWAP A", 2},
        {{0xCB, 0x3F}, "SRL A", 2},
        {{0xCB, 0x46}, "BIT 0,(HL)", 2},
        {{0xCB, 0x7C}, "BIT 7,H", 2},
        {{0xCB, 0x87}, "RES 0,A", 2},
        {{0xCB, 0xBE}, "RES 7,(HL)", 2},
        {{0xCB, 0xC0}, "SET 0,B", 2},
        {{0xCB, 0xFF}, "SET 7,A", 2},
    };
    for (const auto& r : rows) {
        int len = 0;
        const std::string t = dis(r.bytes, 0x0100, &len);
        if (t != r.text || len != r.len)
            std::cerr << "  $" << std::hex << (int)r.bytes[0] << std::dec << ": got '" << t << "' (" << len << ")\n";
        EXPECT_EQ(t, std::string(r.text));
        EXPECT_EQ(len, r.len);
    }
}

TEST_CASE(sm83_disasm_lengths_and_flags) {
    // Every opcode decodes, and its length is what the CPU fetches
    int illegal = 0;
    for (int op = 0; op < 256; ++op) {
        int len = 0;
        DisasmEntry e;
        const std::string t = dis({(uint8_t)op, 0x00, 0x00}, 0x0200, &len, &e);
        EXPECT_TRUE(!t.empty());
        EXPECT_TRUE(len >= 1 && len <= 3);
        EXPECT_EQ(len, gb::sm83Length((uint8_t)op));
        EXPECT_EQ((int)e.rawLen, len);
        if (e.isIllegal) ++illegal;
    }
    EXPECT_EQ(illegal, 11);

    DisasmEntry e;
    dis({0xCD, 0x34, 0x12}, 0x0100, nullptr, &e);
    EXPECT_TRUE(e.isCall);
    EXPECT_EQ(e.targetAddr, 0x1234u);
    dis({0xEF}, 0x0100, nullptr, &e);                 // RST $28
    EXPECT_TRUE(e.isCall);
    EXPECT_EQ(e.targetAddr, 0x28u);
    dis({0xD8}, 0x0100, nullptr, &e);                 // RET C
    EXPECT_TRUE(e.isReturn);
    dis({0xD9}, 0x0100, nullptr, &e);                 // RETI
    EXPECT_TRUE(e.isReturn);
    dis({0x28, 0xFC}, 0x0100, nullptr, &e);           // JR Z,$00FE
    EXPECT_TRUE(e.isBranch);
    EXPECT_EQ(e.targetAddr, 0x00FEu);
    EXPECT_TRUE(!e.isCall && !e.isReturn);
    dis({0xC3, 0x00, 0x80}, 0x0100, nullptr, &e);
    EXPECT_TRUE(e.isBranch);
    EXPECT_EQ(e.targetAddr, 0x8000u);
    EXPECT_EQ(e.mnemonic, std::string("JP"));
    EXPECT_EQ(e.operands, std::string("$8000"));
}

TEST_CASE(sm83_disasm_symbols) {
    SymbolTable syms;
    syms.addSymbol(0xFF40, "LCDC");
    syms.addSymbol(0x1234, "main");
    EXPECT_EQ(dis({0xE0, 0x40}, 0x0100, nullptr, nullptr, &syms), std::string("LDH (LCDC),A"));
    EXPECT_EQ(dis({0xCD, 0x34, 0x12}, 0x0100, nullptr, nullptr, &syms), std::string("CALL main"));
    EXPECT_EQ(dis({0x3E, 0x40}, 0x0100, nullptr, nullptr, &syms), std::string("LD A,$40"));    // not an address

    DisassemblerSm83 d;
    EXPECT_EQ(std::string(d.isaName()), std::string("sm83"));
    FlatMemoryBus bus("t", 16);
    bus.write8(0x10, 0xCB);
    bus.write8(0x11, 0x37);
    char buf[64];
    EXPECT_EQ(d.disasmOne(&bus, 0x10, buf, sizeof buf), 2);
    EXPECT_EQ(std::string(buf), std::string("SWAP A"));
}
