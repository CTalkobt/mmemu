#pragma once

// The MC6801/MC6803 line assembler: every instruction the 6803
// disassembler writes reads back as the same bytes, in Motorola syntax:
//   LDAA #$12   LDAA $12 (direct)   LDAA $1234 (extended)   LDAA $12,X   LDAA ,X
//   <$12 forces direct, >$12 (or four digits, $0012) extended; branches and BSR take the target
//   address; LSLD/LSL = ASLD/ASL; LDA/STA/ORA (and B) = LDAA/STAA/ORAA
//   FCB $12,$34 (DB, .BYTE)   FDB $1234 (DW, .WORD)
// Numbers: $FF, 0xFF, FFh, %1010, 255, 'A', with a leading - where signed.

#include "libtoolchain/main/iassembler.h"

#include <cstdint>
#include <string>

/** The 6803 assembler for the toolchain registry (ISA "6803"): line mode only. */
class Assembler6801 : public IAssembler {
public:
    const char* name() const override { return "Native6803"; }
    bool isaSupported(const std::string& isa) const override { return isa == "6803" || isa == "6801"; }

    AssemblerResult assemble(const std::string& sourcePath, const std::string& outputPath) override;
    /** One instruction at `currentAddr` (branch offsets are from it); its length, 0 for none, -1 on an error. */
    int assembleLine(const std::string& line, uint8_t* buf, int bufsz, uint32_t currentAddr = 0) override;
};
