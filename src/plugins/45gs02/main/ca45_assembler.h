#pragma once

#include "../../../libtoolchain/main/iassembler.h"

/**
 * Ca45 external assembler backend.
 * Invokes the ca45 binary to assemble .s source files into .prg binaries;
 * single lines go through libca45 in-process (assembleLine).
 * Supports 45GS02, 6502, and 65CE02 ISAs.
 */
class Ca45AssemblerBackend : public IAssembler {
public:
    Ca45AssemblerBackend();

    const char* name() const override { return "ca45"; }
    bool isaSupported(const std::string& isa) const override;
    AssemblerResult assemble(const std::string& sourcePath, const std::string& outputPath) override;
    /**
     * One line through libca45 (ca45's assembler as a library, linked when the
     * build finds it: CA45_DIR in the Makefile), so the dot-assembly knows the
     * 45GS02's own instructions (MAP, EOM, NEG, ASR, quad and [zp],z modes, 16-bit
     * branches). Without libca45 it returns -1 and the caller falls back to the
     * 6502 assembler.
     */
    int assembleLine(const std::string& line, uint8_t* buf, int bufsz, uint32_t currentAddr = 0) override;

    /** Why the last assembleLine() returned -1 ("" after a success). */
    const std::string& lastError() const { return m_lastError; }

private:
    std::string m_lastError;
};
