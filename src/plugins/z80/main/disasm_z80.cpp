// Z80 disassembler (Zilog mnemonics), decoding as in cpu_z80.cpp.

#include "cpu_z80.h"
#include "disasm_z80.h"
#include <cstdio>
#include <string>

namespace {

const char* const R8[8] = {"B", "C", "D", "E", "H", "L", "(HL)", "A"};
const char* const RP[4] = {"BC", "DE", "HL", "SP"};
const char* const RP2[4] = {"BC", "DE", "HL", "AF"};
const char* const CC[8] = {"NZ", "Z", "NC", "C", "PO", "PE", "P", "M"};
const char* const ALU[8] = {"ADD A,", "ADC A,", "SUB ", "SBC A,", "AND ", "XOR ", "OR ", "CP "};
const char* const ROT[8] = {"RLC", "RRC", "RL", "RR", "SLA", "SRA", "SLL", "SRL"};
const char* const ROTA[8] = {"RLCA", "RRCA", "RLA", "RRA", "DAA", "CPL", "SCF", "CCF"};

std::string hex2(unsigned v) { char b[8]; std::snprintf(b, sizeof b, "$%02X", v & 0xFF); return b; }
std::string hex4(unsigned v) { char b[8]; std::snprintf(b, sizeof b, "$%04X", v & 0xFFFF); return b; }

struct Dis {
    IBus* bus;
    uint32_t pc;
    int len = 0;
    int prefix = 0;                 // 0, 1 IX, 2 IY
    bool dispRead = false;
    int8_t disp = 0;
    uint8_t byte() { return bus ? bus->peek8((pc + len++) & 0xFFFF) : 0; }
    uint16_t word() { uint8_t lo = byte(); return (uint16_t)(lo | byte() << 8); }
    std::string hlName() const { return prefix == 1 ? "IX" : prefix == 2 ? "IY" : "HL"; }
    std::string memHl() {
        if (!prefix) return "(HL)";
        if (!dispRead) { disp = (int8_t)byte(); dispRead = true; }
        char b[16];
        std::snprintf(b, sizeof b, "(%s%c$%02X)", hlName().c_str(), disp < 0 ? '-' : '+', disp < 0 ? -disp : disp);
        return b;
    }
    std::string r8(int r, bool plainHl = false) {
        if (r == 6) return memHl();
        if (prefix && !plainHl && (r == 4 || r == 5)) return hlName() + (r == 4 ? "H" : "L");
        return R8[r];
    }
    std::string rp(int p) { return p == 2 ? hlName() : RP[p]; }
    std::string rp2(int p) { return p == 2 ? hlName() : RP2[p]; }
    std::string rel() { int8_t d = (int8_t)byte(); return hex4((uint32_t)(pc + len + d)); }

    std::string main(uint8_t op) {
        const int x = op >> 6, y = (op >> 3) & 7, z = op & 7, p = y >> 1, q = y & 1;
        switch (x) {
        case 0:
            switch (z) {
            case 0:
                switch (y) {
                case 0: return "NOP";
                case 1: return "EX AF,AF'";
                case 2: return "DJNZ " + rel();
                case 3: return "JR " + rel();
                default: { std::string c = CC[y - 4]; return "JR " + c + "," + rel(); }
                }
            case 1:
                if (!q) { std::string r = rp(p); return "LD " + r + "," + hex4(word()); }
                return "ADD " + hlName() + "," + rp(p);
            case 2:
                switch (y) {
                case 0: return "LD (BC),A";
                case 1: return "LD A,(BC)";
                case 2: return "LD (DE),A";
                case 3: return "LD A,(DE)";
                case 4: return "LD (" + hex4(word()) + ")," + hlName();
                case 5: { std::string a = hex4(word()); return "LD " + hlName() + ",(" + a + ")"; }
                case 6: return "LD (" + hex4(word()) + "),A";
                default: return "LD A,(" + hex4(word()) + ")";
                }
            case 3: return std::string(q ? "DEC " : "INC ") + rp(p);
            case 4: return "INC " + r8(y);
            case 5: return "DEC " + r8(y);
            case 6: { std::string d = r8(y); return "LD " + d + "," + hex2(byte()); }
            default: return ROTA[y];
            }
        case 1:
            if (y == 6 && z == 6) return "HALT";
            {
                bool mem = y == 6 || z == 6;
                std::string d = r8(y, mem), s = r8(z, mem);
                return "LD " + d + "," + s;
            }
        case 2: return std::string(ALU[y]) + r8(z);
        default:
            switch (z) {
            case 0: return std::string("RET ") + CC[y];
            case 1:
                if (!q) return "POP " + rp2(p);
                switch (p) {
                case 0: return "RET";
                case 1: return "EXX";
                case 2: return "JP (" + hlName() + ")";
                default: return "LD SP," + hlName();
                }
            case 2: return std::string("JP ") + CC[y] + "," + hex4(word());
            case 3:
                switch (y) {
                case 0: return "JP " + hex4(word());
                case 2: return "OUT (" + hex2(byte()) + "),A";
                case 3: return "IN A,(" + hex2(byte()) + ")";
                case 4: return "EX (SP)," + hlName();
                case 5: return "EX DE,HL";
                case 6: return "DI";
                default: return "EI";
                }
            case 4: return std::string("CALL ") + CC[y] + "," + hex4(word());
            case 5:
                if (!q) return "PUSH " + rp2(p);
                return "CALL " + hex4(word());
            case 6: return std::string(ALU[y]) + hex2(byte());
            default: return "RST " + hex2((unsigned)y * 8);
            }
        }
    }

    std::string cb() {
        std::string m;
        uint8_t op;
        if (prefix) { memHl(); op = byte(); } else op = byte();
        const int x = op >> 6, y = (op >> 3) & 7, z = op & 7;
        std::string operand = prefix ? memHl() : r8(z);
        switch (x) {
            case 0: m = std::string(ROT[y]) + " " + operand; break;
            case 1: return "BIT " + std::to_string(y) + "," + operand;
            case 2: m = "RES " + std::to_string(y) + "," + operand; break;
            default: m = "SET " + std::to_string(y) + "," + operand; break;
        }
        if (prefix && z != 6) m += std::string(",") + R8[z];   // undocumented copy
        return m;
    }

    std::string ed() {
        uint8_t op = byte();
        const int x = op >> 6, y = (op >> 3) & 7, z = op & 7, p = y >> 1, q = y & 1;
        if (x == 1) {
            switch (z) {
            case 0: return y == 6 ? "IN (C)" : std::string("IN ") + R8[y] + ",(C)";
            case 1: return y == 6 ? "OUT (C),0" : std::string("OUT (C),") + R8[y];
            case 2: return std::string(q ? "ADC HL," : "SBC HL,") + RP[p];
            case 3: { std::string a = hex4(word());
                      return q ? std::string("LD ") + RP[p] + ",(" + a + ")" : "LD (" + a + ")," + RP[p]; }
            case 4: return "NEG";
            case 5: return y == 1 ? "RETI" : "RETN";
            case 6: { static const char* IM[8] = {"0", "0", "1", "2", "0", "0", "1", "2"}; return std::string("IM ") + IM[y]; }
            default: {
                static const char* M[8] = {"LD I,A", "LD R,A", "LD A,I", "LD A,R", "RRD", "RLD", "NOP", "NOP"};
                return M[y];
            }
            }
        }
        if (x == 2 && y >= 4 && z <= 3) {
            static const char* B[4][4] = {{"LDI", "CPI", "INI", "OUTI"}, {"LDD", "CPD", "IND", "OUTD"},
                                          {"LDIR", "CPIR", "INIR", "OTIR"}, {"LDDR", "CPDR", "INDR", "OTDR"}};
            return B[y - 4][z];
        }
        return "NOP";
    }
};

}  // namespace

int Z80::disassembleOne(IBus* bus, uint32_t addr, char* buf, int bufsz) {
    Dis d{bus, addr & 0xFFFF};
    uint8_t op = d.byte();
    while (op == 0xDD || op == 0xFD) {
        d.prefix = op == 0xDD ? 1 : 2;
        op = d.byte();
    }
    std::string s;
    if (op == 0xCB) s = d.cb();
    else if (op == 0xED) { d.prefix = 0; s = d.ed(); }
    else s = d.main(op);
    std::snprintf(buf, (size_t)bufsz, "%s", s.c_str());
    return d.len;
}

int DisassemblerZ80::disasmOne(IBus* bus, uint32_t addr, char* buf, int bufsz) {
    return m_cpu.disassembleOne(bus, addr, buf, bufsz);
}

int DisassemblerZ80::disasmEntry(IBus* bus, uint32_t addr, DisasmEntry& entry) {
    return m_cpu.disassembleEntry(bus, addr, &entry);
}
