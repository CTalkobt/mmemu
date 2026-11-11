// The SM83 disassembler (see sm83_disasm.h); the opcode map decoded as x/y/z
// fields, as the Z80 disassembler does.

#include "sm83_disasm.h"
#include "libtoolchain/main/symbol_table.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace gb {

namespace {

const char* const R8[8] = {"B", "C", "D", "E", "H", "L", "(HL)", "A"};
const char* const RP[4] = {"BC", "DE", "HL", "SP"};
const char* const RP2[4] = {"BC", "DE", "HL", "AF"};
const char* const CC[4] = {"NZ", "Z", "NC", "C"};
const char* const ALU[8] = {"ADD A,", "ADC A,", "SUB ", "SBC A,", "AND ", "XOR ", "OR ", "CP "};
const char* const ROT[8] = {"RLC", "RRC", "RL", "RR", "SLA", "SRA", "SWAP", "SRL"};
const char* const ROTA[8] = {"RLCA", "RRCA", "RLA", "RRA", "DAA", "CPL", "SCF", "CCF"};

std::string hex2(unsigned v) { char b[8]; std::snprintf(b, sizeof b, "$%02X", v & 0xFF); return b; }
std::string hex4(unsigned v) { char b[8]; std::snprintf(b, sizeof b, "$%04X", v & 0xFFFF); return b; }
std::string signedHex(int8_t d) {
    char b[8];
    std::snprintf(b, sizeof b, "%c$%02X", d < 0 ? '-' : '+', d < 0 ? -d : d);
    return b;
}

bool illegal(uint8_t op) {
    switch (op) {
    case 0xD3: case 0xDB: case 0xDD: case 0xE3: case 0xE4: case 0xEB:
    case 0xEC: case 0xED: case 0xF4: case 0xFC: case 0xFD: return true;
    default: return false;
    }
}

struct Dis {
    IBus* bus;
    uint32_t pc;
    const SymbolTable* syms;
    uint8_t b[3] = {};
    int len = 0;

    uint8_t at(int i) const { return bus ? bus->peek8((pc + i) & 0xFFFF) : 0; }
    uint8_t n8() const { return b[1]; }
    uint16_t n16() const { return (uint16_t)(b[1] | b[2] << 8); }
    std::string addr(uint16_t a) const {
        if (syms) {
            std::string l = syms->getLabel(a);
            if (!l.empty()) return l;
        }
        return hex4(a);
    }
    uint16_t relTarget() const { return (uint16_t)(pc + 2 + (int8_t)b[1]); }

    std::string cb(uint8_t op) const {
        const int x = op >> 6, y = (op >> 3) & 7, z = op & 7;
        switch (x) {
        case 0: return std::string(ROT[y]) + " " + R8[z];
        case 1: return "BIT " + std::to_string(y) + "," + R8[z];
        case 2: return "RES " + std::to_string(y) + "," + R8[z];
        default: return "SET " + std::to_string(y) + "," + R8[z];
        }
    }

    std::string main(uint8_t op) const {
        if (illegal(op)) return "DB " + hex2(op);
        const int x = op >> 6, y = (op >> 3) & 7, z = op & 7, p = y >> 1, q = y & 1;
        switch (x) {
        case 0:
            switch (z) {
            case 0:
                switch (y) {
                case 0: return "NOP";
                case 1: return "LD (" + addr(n16()) + "),SP";
                case 2: return "STOP";
                case 3: return "JR " + addr(relTarget());
                default: return std::string("JR ") + CC[y - 4] + "," + addr(relTarget());
                }
            case 1:
                if (!q) return std::string("LD ") + RP[p] + "," + hex4(n16());
                return std::string("ADD HL,") + RP[p];
            case 2: {
                static const char* const M[4] = {"(BC)", "(DE)", "(HL+)", "(HL-)"};
                return q ? std::string("LD A,") + M[p] : std::string("LD ") + M[p] + ",A";
            }
            case 3: return std::string(q ? "DEC " : "INC ") + RP[p];
            case 4: return std::string("INC ") + R8[y];
            case 5: return std::string("DEC ") + R8[y];
            case 6: return std::string("LD ") + R8[y] + "," + hex2(n8());
            default: return ROTA[y];
            }
        case 1:
            if (op == 0x76) return "HALT";
            return std::string("LD ") + R8[y] + "," + R8[z];
        case 2:
            return std::string(ALU[y]) + R8[z];
        default:
            switch (z) {
            case 0:
                if (y < 4) return std::string("RET ") + CC[y];
                switch (y) {
                case 4: return "LDH (" + addr((uint16_t)(0xFF00 | n8())) + "),A";
                case 5: return "ADD SP," + signedHex((int8_t)n8());
                case 6: return "LDH A,(" + addr((uint16_t)(0xFF00 | n8())) + ")";
                default: return "LD HL,SP" + signedHex((int8_t)n8());
                }
            case 1:
                if (!q) return std::string("POP ") + RP2[p];
                switch (p) {
                case 0: return "RET";
                case 1: return "RETI";
                case 2: return "JP HL";
                default: return "LD SP,HL";
                }
            case 2:
                if (y < 4) return std::string("JP ") + CC[y] + "," + addr(n16());
                switch (y) {
                case 4: return "LD ($FF00+C),A";
                case 5: return "LD (" + addr(n16()) + "),A";
                case 6: return "LD A,($FF00+C)";
                default: return "LD A,(" + addr(n16()) + ")";
                }
            case 3:
                switch (y) {
                case 0: return "JP " + addr(n16());
                case 1: return cb(n8());
                case 6: return "DI";
                default: return "EI";            // 7 (2-5 are illegal)
                }
            case 4: return std::string("CALL ") + CC[y & 3] + "," + addr(n16());
            case 5:
                if (!q) return std::string("PUSH ") + RP2[p];
                return "CALL " + addr(n16());        // $CD (the others are illegal)
            case 6: return std::string(ALU[y]) + hex2(n8());
            default: return "RST " + hex2((unsigned)(y * 8));
            }
        }
    }
};

}  // namespace

int sm83Length(uint8_t op) {
    if (illegal(op)) return 1;
    switch (op) {
    case 0x01: case 0x11: case 0x21: case 0x31: case 0x08:
    case 0xC2: case 0xCA: case 0xD2: case 0xDA: case 0xC3:
    case 0xC4: case 0xCC: case 0xD4: case 0xDC: case 0xCD:
    case 0xEA: case 0xFA:
        return 3;
    case 0x10: case 0x18: case 0x20: case 0x28: case 0x30: case 0x38:
    case 0xE0: case 0xF0: case 0xE8: case 0xF8: case 0xCB:
        return 2;
    default:
        if ((op & 0xC7) == 0x06 || (op & 0xC7) == 0xC6) return 2;     // LD r,n; ALU n
        return 1;
    }
}

int sm83Disassemble(IBus* bus, uint32_t addr, DisasmEntry& e, const SymbolTable* syms, bool withText) {
    Dis d{bus, addr & 0xFFFF, syms};
    const uint8_t op = d.at(0);
    d.len = sm83Length(op);
    for (int i = 0; i < d.len; ++i) d.b[i] = d.at(i);

    e.addr = addr & 0xFFFF;
    e.bytes = d.len;
    e.isIllegal = illegal(op);
    e.isCall = op == 0xCD || (op & 0xE7) == 0xC4 || (op & 0xC7) == 0xC7;     // CALL, CALL cc, RST
    e.isReturn = op == 0xC9 || op == 0xD9 || (op & 0xE7) == 0xC0;              // RET, RETI, RET cc
    e.isBranch = op == 0xC3 || (op & 0xE7) == 0xC2 || op == 0x18 || (op & 0xE7) == 0x20 || op == 0xE9;
    e.stackOp = StackOp::NONE;
    e.targetAddr = 0;
    if (op == 0x18 || (op & 0xE7) == 0x20) e.targetAddr = d.relTarget();
    else if (op == 0xC3 || op == 0xCD || (op & 0xE7) == 0xC2 || (op & 0xE7) == 0xC4) e.targetAddr = d.n16();
    else if ((op & 0xC7) == 0xC7) e.targetAddr = op & 0x38;
    e.rawLen = (uint8_t)std::min(d.len, (int)DisasmEntry::MAX_RAW);
    std::memcpy(e.raw, d.b, e.rawLen);

    if (withText) {
        e.complete = d.main(op);
        const auto sp = e.complete.find(' ');
        e.mnemonic = sp == std::string::npos ? e.complete : e.complete.substr(0, sp);
        e.operands = sp == std::string::npos ? "" : e.complete.substr(sp + 1);
    } else {
        e.complete.clear();
        e.mnemonic.clear();
        e.operands.clear();
    }
    return d.len;
}

}  // namespace gb

int DisassemblerSm83::disasmOne(IBus* bus, uint32_t addr, char* buf, int bufsz) {
    DisasmEntry e;
    const int n = gb::sm83Disassemble(bus, addr, e, m_syms);
    if (buf && bufsz > 0) std::snprintf(buf, (size_t)bufsz, "%s", e.complete.c_str());
    return n;
}

int DisassemblerSm83::disasmEntry(IBus* bus, uint32_t addr, DisasmEntry& entry) {
    return gb::sm83Disassemble(bus, addr, entry, m_syms);
}
