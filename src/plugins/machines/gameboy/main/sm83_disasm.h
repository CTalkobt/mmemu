#pragma once

// The Sharp SM83 (the Game Boy's CPU) disassembler: every opcode and the CB
// prefixed ones, in the usual Game Boy mnemonics (RGBDS's, with parentheses
// for memory operands as the Z80 disassembler writes them):
//   LD (HL+),A   LD A,(HL-)   LDH ($FF44),A   LD ($FF00+C),A
//   ADD SP,-$02  LD HL,SP+$05 JR $0150 (the target)   RST $38   STOP
// The eleven unused opcodes ($D3 $DB $DD $E3 $E4 $EB $EC $ED $F4 $FC $FD,
// which lock the CPU up) are "DB $xx", marked illegal.

#include "libtoolchain/main/idisasm.h"

#include <cstdint>

class SymbolTable;

namespace gb {

/**
 * Disassemble the instruction at `addr` (peeked from `bus`) into `e`:
 * text, length, raw bytes, and the call / return / branch flags and target.
 * `syms`: absolute addresses with a label are written as the label.
 * `withText` false: only the length, flags and bytes (the debugger's hot path).
 * @return its length in bytes (1-3).
 */
int sm83Disassemble(IBus* bus, uint32_t addr, DisasmEntry& e, const SymbolTable* syms = nullptr, bool withText = true);

/** The length of the instruction starting with `op` (1-3; CB-prefixed ones are 2). */
int sm83Length(uint8_t op);

}  // namespace gb

/** The SM83 disassembler for the toolchain registry (ISA "sm83"). */
class DisassemblerSm83 : public IDisassembler {
public:
    const char* isaName() const override { return "sm83"; }
    int disasmOne(IBus* bus, uint32_t addr, char* buf, int bufsz) override;
    int disasmEntry(IBus* bus, uint32_t addr, DisasmEntry& entry) override;
    void setSymbolTable(SymbolTable* symbols) override { m_syms = symbols; }
private:
    SymbolTable* m_syms = nullptr;
};
