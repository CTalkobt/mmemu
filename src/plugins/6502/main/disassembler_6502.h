#pragma once

#include "libtoolchain/main/idisasm.h"
#include "cpu6502.h"
#include "cpu65c02.h"
#include "libtoolchain/main/symbol_table.h"
#include <memory>

class Disassembler6502 : public IDisassembler {
public:
    /** `cmos`: disassemble WDC 65C02 code (ISA "65C02"). */
    Disassembler6502(SymbolTable* symbols = nullptr, bool cmos = false);

    [[nodiscard]] const char* isaName() const override { return m_cpu->isaName(); }

    int disasmOne(IBus* bus, uint32_t addr, char* buf, int bufsz) override;
    int disasmEntry(IBus* bus, uint32_t addr, DisasmEntry& entry) override;
    void setSymbolTable(SymbolTable* symbols) override { m_symbols = symbols; }

private:
    std::unique_ptr<MOS6502> m_cpu;
    SymbolTable* m_symbols;

    void resolveSymbols(DisasmEntry& entry);
};
