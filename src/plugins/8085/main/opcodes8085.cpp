// The 8085 opcode table, built from the opcode's fields: op = x(7-6) y(5-3)
// z(2-0), y = p(5-4) q(3). Undocumented mnemonics after Dehnhardt and
// Sorensen, "Unspecified 8085 op codes enhance programming" (Electronics,
// 1979), as MAME's 8085dasm.cpp and most 8085 assemblers spell them.

#include "opcodes8085.h"

#include <array>

namespace {

const char* const R8[8] = {"B", "C", "D", "E", "H", "L", "M", "A"};
const char* const RP[4] = {"B", "D", "H", "SP"};
const char* const RP2[4] = {"B", "D", "H", "PSW"};
const char* const CC[8] = {"NZ", "Z", "NC", "C", "PO", "PE", "P", "M"};
const char* const ALU[8] = {"ADD", "ADC", "SUB", "SBB", "ANA", "XRA", "ORA", "CMP"};
const char* const ALUI[8] = {"ADI", "ACI", "SUI", "SBI", "ANI", "XRI", "ORI", "CPI"};
const char* const ROT[8] = {"RLC", "RRC", "RAL", "RAR", "DAA", "CMA", "STC", "CMC"};

I8085Opcode make(int op) {
    const int x = op >> 6, y = (op >> 3) & 7, z = op & 7, p = y >> 1, q = y & 1;
    auto o = [](std::string m, std::string ops = "", bool undoc = false) {
        int len = 1;
        size_t at = 0;
        // Operand lengths from the template
        while (at <= ops.size()) {
            size_t comma = ops.find(',', at);
            std::string part = ops.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
            if (part == "n") len += 1;
            else if (part == "nn") len += 2;
            if (comma == std::string::npos) break;
            at = comma + 1;
        }
        return I8085Opcode{std::move(m), std::move(ops), len, undoc};
    };
    switch (x) {
    case 0:
        switch (z) {
        case 0: {
            static const char* const Z0[8] = {"NOP", "DSUB", "ARHL", "RDEL", "RIM", "LDHI", "SIM", "LDSI"};
            const bool undoc = y != 0 && y != 4 && y != 6;
            return o(Z0[y], (y == 5 || y == 7) ? "n" : "", undoc);
        }
        case 1: return q ? o("DAD", RP[p]) : o("LXI", std::string(RP[p]) + ",nn");
        case 2: {
            static const char* const Z2[8] = {"STAX", "LDAX", "STAX", "LDAX", "SHLD", "LHLD", "STA", "LDA"};
            return y < 4 ? o(Z2[y], y < 2 ? "B" : "D") : o(Z2[y], "nn");
        }
        case 3: return o(q ? "DCX" : "INX", RP[p]);
        case 4: return o("INR", R8[y]);
        case 5: return o("DCR", R8[y]);
        case 6: return o("MVI", std::string(R8[y]) + ",n");
        default: return o(ROT[y]);
        }
    case 1:
        if (op == 0x76) return o("HLT");
        return o("MOV", std::string(R8[y]) + "," + R8[z]);
    case 2: return o(ALU[y], R8[z]);
    default:
        switch (z) {
        case 0: return o(std::string("R") + CC[y]);
        case 1:
            if (!q) return o("POP", RP2[p]);
            {
                static const char* const Z1[4] = {"RET", "SHLX", "PCHL", "SPHL"};
                return o(Z1[p], "", p == 1);
            }
        case 2: return o(std::string("J") + CC[y], "nn");
        case 3:
            switch (y) {
            case 0: return o("JMP", "nn");
            case 1: return o("RSTV", "", true);
            case 2: return o("OUT", "n");
            case 3: return o("IN", "n");
            case 4: return o("XTHL");
            case 5: return o("XCHG");
            case 6: return o("DI");
            default: return o("EI");
            }
        case 4: return o(std::string("C") + CC[y], "nn");
        case 5:
            if (!q) return o("PUSH", RP2[p]);
            switch (p) {
            case 0: return o("CALL", "nn");
            case 1: return o("JNK", "nn", true);
            case 2: return o("LHLX", "", true);
            default: return o("JK", "nn", true);
            }
        case 6: return o(ALUI[y], "n");
        default: return o("RST", std::string(1, (char)('0' + y)));
        }
    }
}

}  // namespace

const I8085Opcode& i8085Opcode(uint8_t op) {
    static const std::array<I8085Opcode, 256> table = [] {
        std::array<I8085Opcode, 256> t;
        for (int i = 0; i < 256; ++i) t[i] = make(i);
        return t;
    }();
    return table[op];
}
