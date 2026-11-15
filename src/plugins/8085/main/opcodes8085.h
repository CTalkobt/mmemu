#pragma once

#include <cstdint>
#include <string>

/**
 * One 8085 opcode in Intel's mnemonics, as the disassembler writes it and the
 * line assembler matches it. `operands` is a comma-separated template where
 * "n" stands for an 8-bit number and "nn" for a 16-bit one (e.g. "B,n",
 * "H,nn", "nn"), or empty.
 */
struct I8085Opcode {
    std::string mnemonic;
    std::string operands;
    int length;                 // bytes, with the opcode
    bool undocumented;          // DSUB ARHL RDEL LDHI LDSI RSTV SHLX JNK LHLX JK
};

/** The opcode table (all 256 opcodes are defined on the 8085). */
const I8085Opcode& i8085Opcode(uint8_t op);
