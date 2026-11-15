#pragma once

// The 8085 line assembler: Intel mnemonics, every instruction the 8085
// disassembler writes reads back as the same bytes, including the
// undocumented ones (DSUB ARHL RDEL LDHI LDSI RSTV SHLX JNK LHLX JK) and
// some other names for them (ASHR = ARHL, RLDE = RDEL, LDEH = LDHI,
// LDES = LDSI, JNX5/JNUI = JNK, JX5/JUI = JK, OVRST8 = RSTV).
//   RST 0-7 (or its address: RST 38H)   DB $12,$34   DW $1234
// Numbers: $FF, 0xFF, 0FFH, %1010, 255, with a leading - for 8-bit ones.

#include "libtoolchain/main/iassembler.h"

#include <cstdint>
#include <string>

/** The 8085 assembler for the toolchain registry (ISA "8085"): line mode only. */
class Assembler8085 : public IAssembler {
public:
    const char* name() const override { return "Native8085"; }
    bool isaSupported(const std::string& isa) const override { return isa == "8085" || isa == "8080"; }

    AssemblerResult assemble(const std::string& sourcePath, const std::string& outputPath) override;
    /** One instruction; its length, 0 for none, -1 on an error. */
    int assembleLine(const std::string& line, uint8_t* buf, int bufsz, uint32_t currentAddr = 0) override;
};
