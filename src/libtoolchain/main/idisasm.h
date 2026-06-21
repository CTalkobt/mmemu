#pragma once

#include <cstdint>
#include <string>
#include "libmem/main/ibus.h"

/**
 * Metadata for a single disassembled instruction.
 */
struct DisasmEntry {
    uint32_t addr;
    int      bytes;
    std::string mnemonic;
    std::string operands;
    std::string complete; // e.g. "LDA #$42"

    bool     isCall;
    bool     isReturn;
    bool     isBranch;
    bool     isIllegal;  // opcode is unofficial/undocumented
    uint32_t targetAddr;
};

class SymbolTable;

/**
 * Abstract interface for a disassembler.
 */
class IDisassembler {
public:
    virtual ~IDisassembler();

    [[nodiscard]] virtual const char* isaName() const = 0;

    /**
     * Disassemble one instruction into a string buffer.
     * @return Number of bytes consumed.
     */
    virtual int disasmOne(IBus* bus, uint32_t addr, char* buf, int bufsz) = 0;

    /**
     * Disassemble one instruction into a DisasmEntry structure.
     * @return Number of bytes consumed.
     */
    virtual int disasmEntry(IBus* bus, uint32_t addr, DisasmEntry& entry) = 0;

    /**
     * Associate a symbol table with the disassembler for label resolution.
     */
    virtual void setSymbolTable(SymbolTable* symbols) = 0;
};
