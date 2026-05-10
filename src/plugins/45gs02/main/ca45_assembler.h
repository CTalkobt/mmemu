#pragma once

#include "../../../libtoolchain/main/iassembler.h"

/**
 * Ca45 external assembler backend.
 * Invokes the ca45 binary to assemble .s source files into .prg binaries.
 * Supports 45GS02, 6502, and 65CE02 ISAs.
 */
class Ca45AssemblerBackend : public IAssembler {
public:
    Ca45AssemblerBackend();

    const char* name() const override { return "ca45"; }
    bool isaSupported(const std::string& isa) const override;
    AssemblerResult assemble(const std::string& sourcePath, const std::string& outputPath) override;
    int assembleLine(const std::string& line, uint8_t* buf, int bufsz, uint32_t currentAddr = 0) override {
        return -1;  // Not supported for inline assembly
    }
};
