#pragma once

#include "libtoolchain/main/iassembler.h"
#include <map>
#include <string>
#include <vector>
#include <cstdint>

class Assembler6502 : public IAssembler {
public:
    /** `cmos`: the WDC 65C02 instruction set instead of the NMOS one with its illegal opcodes. */
    explicit Assembler6502(bool cmos = false);

    const char* name() const override { return m_cmos ? "Native65C02" : "Native6502"; }
    bool isaSupported(const std::string& isa) const override { return isa == (m_cmos ? "65C02" : "6502"); }

    AssemblerResult assemble(const std::string& sourcePath, const std::string& outputPath) override;
    int assembleLine(const std::string& line, uint8_t* buf, int bufsz, uint32_t currentAddr = 0) override;

private:
    struct OpcodeInfo {
        uint8_t opcode;
        int size;
    };

    // Mapping: mnemonic -> mode -> info
    // Mode represented as string for simple parsing logic
    std::map<std::string, std::map<std::string, OpcodeInfo>> m_mnemonics;

    bool m_cmos;

    void initTable();
    void initCmos();
    void add(const std::string& mnem, const std::string& mode, uint8_t op, int sz);
};
