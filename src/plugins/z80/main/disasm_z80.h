#pragma once

#include "cpu_z80.h"
#include "libtoolchain/main/idisasm.h"

/** The Z80 disassembler for the toolchain registry (Zilog mnemonics). */
class DisassemblerZ80 : public IDisassembler {
public:
    const char* isaName() const override { return "z80"; }
    int disasmOne(IBus* bus, uint32_t addr, char* buf, int bufsz) override;
    int disasmEntry(IBus* bus, uint32_t addr, DisasmEntry& entry) override;
    void setSymbolTable(SymbolTable*) override {}
private:
    Z80 m_cpu;
};
