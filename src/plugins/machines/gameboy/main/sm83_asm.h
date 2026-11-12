#pragma once

// The Sharp SM83 (the Game Boy's CPU) line assembler: every instruction the
// SM83 disassembler writes reads back as the same bytes, in its mnemonics and
// the usual alternatives:
//   LD (HL+),A = LD (HLI),A = LDI (HL),A     LD A,(HL-) = LD A,(HLD) = LDD A,(HL)
//   LDH ($FF44),A = LDH ($44),A               LD ($FF00+C),A = LD (C),A = LDH (C),A
//   ADD A,B = ADD B (and ADC SUB SBC AND XOR OR CP, with or without A,)
//   JP HL = JP (HL)   JR/JR cc take the target address   STOP (10 00)
//   DB $12,$34   DW $1234
// Numbers: $FF, 0xFF, FFh, %1010, 255, with a leading - where signed.

#include "libtoolchain/main/iassembler.h"

#include <cstdint>
#include <string>

/** The SM83 assembler for the toolchain registry (ISA "sm83"): line mode only. */
class AssemblerSm83 : public IAssembler {
public:
    const char* name() const override { return "NativeSM83"; }
    bool isaSupported(const std::string& isa) const override { return isa == "sm83"; }

    AssemblerResult assemble(const std::string& sourcePath, const std::string& outputPath) override;
    /** One instruction at `currentAddr` (JR's offset is from it); its length, 0 for none, -1 on an error. */
    int assembleLine(const std::string& line, uint8_t* buf, int bufsz, uint32_t currentAddr = 0) override;
};
