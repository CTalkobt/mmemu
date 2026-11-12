// The SM83 line assembler: the instruction set's x/y/z field encoding (as the
// disassembler decodes it), from the operands' kinds.

#include "sm83_asm.h"

#include <cctype>
#include <vector>

namespace {

enum class Kind {
    R8,        // B C D E H L (HL) A: reg 0-7
    Rp,        // BC DE HL SP: reg 0-3
    Af,
    MemBc, MemDe, MemHli, MemHld,
    MemC,      // ($FF00+C), (C)
    Cond,      // NZ Z NC (C is an R8 that can also be one)
    Imm,       // a number
    MemImm,    // (number)
    SpOff,     // SP+e, SP-e
};

struct Operand {
    Kind kind;
    int reg = 0;
    int32_t val = 0;
};

/** A number: $FF, 0xFF, FFh, %1010, 255, with a leading - or +. */
bool parseNumber(const std::string& s, int32_t& out) {
    if (s.empty()) return false;
    size_t i = 0;
    bool neg = false;
    if (s[0] == '-' || s[0] == '+') { neg = s[0] == '-'; i = 1; }
    std::string d = s.substr(i);
    int base = 10;
    if (!d.empty() && d[0] == '$') { base = 16; d = d.substr(1); }
    else if (d.size() > 2 && d[0] == '0' && d[1] == 'X') { base = 16; d = d.substr(2); }
    else if (!d.empty() && d[0] == '%') { base = 2; d = d.substr(1); }
    else if (d.size() > 1 && d.back() == 'H' && std::isdigit((unsigned char)d[0])) { base = 16; d.pop_back(); }
    if (d.empty() || d.size() > 8) return false;
    int64_t v = 0;
    for (char c : d) {
        int n;
        if (c >= '0' && c <= '9') n = c - '0';
        else if (c >= 'A' && c <= 'F') n = 10 + c - 'A';
        else return false;
        if (n >= base) return false;
        v = v * base + n;
    }
    if (v > 0xFFFF) return false;
    out = (int32_t)(neg ? -v : v);
    return true;
}

/** One operand, upper case with no spaces. */
bool parseOperand(const std::string& s, Operand& o) {
    static const char* const R8[8] = {"B", "C", "D", "E", "H", "L", "(HL)", "A"};
    static const char* const RP[4] = {"BC", "DE", "HL", "SP"};
    for (int i = 0; i < 8; ++i) if (s == R8[i]) { o = {Kind::R8, i}; return true; }
    for (int i = 0; i < 4; ++i) if (s == RP[i]) { o = {Kind::Rp, i}; return true; }
    if (s == "AF") { o = {Kind::Af}; return true; }
    if (s == "NZ") { o = {Kind::Cond, 0}; return true; }
    if (s == "Z") { o = {Kind::Cond, 1}; return true; }
    if (s == "NC") { o = {Kind::Cond, 2}; return true; }
    if (s == "(BC)") { o = {Kind::MemBc}; return true; }
    if (s == "(DE)") { o = {Kind::MemDe}; return true; }
    if (s == "(HL+)" || s == "(HLI)") { o = {Kind::MemHli}; return true; }
    if (s == "(HL-)" || s == "(HLD)") { o = {Kind::MemHld}; return true; }
    if (s == "(C)") { o = {Kind::MemC}; return true; }
    if (s.size() > 3 && s.compare(0, 2, "SP") == 0 && (s[2] == '+' || s[2] == '-')) {
        o.kind = Kind::SpOff;
        return parseNumber(s.substr(2), o.val);
    }
    if (s.size() > 2 && s.front() == '(' && s.back() == ')') {
        const std::string in = s.substr(1, s.size() - 2);
        if (in.size() > 2 && in.compare(in.size() - 2, 2, "+C") == 0) {
            int32_t base;
            if (parseNumber(in.substr(0, in.size() - 2), base) && base == 0xFF00) { o = {Kind::MemC}; return true; }
            return false;
        }
        o.kind = Kind::MemImm;
        return parseNumber(in, o.val);
    }
    o.kind = Kind::Imm;
    return parseNumber(s, o.val);
}

bool isR8(const Operand& o, int reg = -1) { return o.kind == Kind::R8 && (reg < 0 || o.reg == reg); }
bool isA(const Operand& o) { return isR8(o, 7); }
/** A condition: NZ Z NC, or C (the register's name). */
bool cond(const Operand& o, int& cc) {
    if (o.kind == Kind::Cond) { cc = o.reg; return true; }
    if (isR8(o, 1)) { cc = 3; return true; }
    return false;
}
bool fits8(int32_t v) { return v >= -128 && v <= 0xFF; }
bool fitsSigned8(int32_t v) { return v >= -128 && v <= 127; }

class Out {
public:
    Out(uint8_t* buf, int size) : m_buf(buf), m_size(size) {}
    int put(std::initializer_list<int> bytes) {
        if ((int)bytes.size() > m_size) return -1;
        int n = 0;
        for (int b : bytes) m_buf[n++] = (uint8_t)b;
        return n;
    }
    int put16(int op, int32_t v) { return put({op, v & 0xFF, (v >> 8) & 0xFF}); }
    uint8_t* m_buf;
    int m_size;
};

}  // namespace

AssemblerResult AssemblerSm83::assemble(const std::string& sourcePath, const std::string& outputPath) {
    (void)sourcePath; (void)outputPath;
    return {false, "File assembly not supported by the SM83 line assembler", "", "", "", 0, 0};
}

int AssemblerSm83::assembleLine(const std::string& line, uint8_t* buf, int bufsz, uint32_t currentAddr) {
    // Upper case, the comment off; the mnemonic, then the operands with no spaces split at commas
    std::string text;
    for (char c : line) {
        if (c == ';') break;
        text += (char)std::toupper((unsigned char)c);
    }
    size_t start = text.find_first_not_of(" \t");
    if (start == std::string::npos) return 0;
    size_t end = text.find_first_of(" \t", start);
    const std::string mnem = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
    std::string rest;
    if (end != std::string::npos) for (char c : text.substr(end)) if (c != ' ' && c != '\t') rest += c;
    std::vector<Operand> ops;
    if (!rest.empty()) {
        size_t from = 0;
        while (true) {
            const size_t comma = rest.find(',', from);
            Operand o;
            if (!parseOperand(rest.substr(from, comma == std::string::npos ? std::string::npos : comma - from), o)) return -1;
            ops.push_back(o);
            if (comma == std::string::npos) break;
            from = comma + 1;
        }
    }
    Out out(buf, bufsz);
    const size_t n = ops.size();

    // Data
    if (mnem == "DB" || mnem == "DW") {
        const int each = mnem == "DB" ? 1 : 2;
        if (n == 0 || (int)n * each > bufsz) return -1;
        int at = 0;
        for (const auto& o : ops) {
            if (o.kind != Kind::Imm || (each == 1 && !fits8(o.val))) return -1;
            buf[at++] = (uint8_t)o.val;
            if (each == 2) buf[at++] = (uint8_t)(o.val >> 8);
        }
        return at;
    }

    // No operands
    static const struct { const char* m; int op; } SIMPLE[] = {
        {"NOP", 0x00}, {"RLCA", 0x07}, {"RRCA", 0x0F}, {"RLA", 0x17}, {"RRA", 0x1F}, {"DAA", 0x27}, {"CPL", 0x2F},
        {"SCF", 0x37}, {"CCF", 0x3F}, {"HALT", 0x76}, {"RETI", 0xD9}, {"DI", 0xF3}, {"EI", 0xFB},
    };
    for (const auto& s : SIMPLE) if (mnem == s.m) return n == 0 ? out.put({s.op}) : -1;
    if (mnem == "STOP") {
        if (n == 0) return out.put({0x10, 0x00});
        return n == 1 && ops[0].kind == Kind::Imm && fits8(ops[0].val) ? out.put({0x10, ops[0].val & 0xFF}) : -1;
    }

    // Jumps, calls, returns
    int cc;
    if (mnem == "RET") {
        if (n == 0) return out.put({0xC9});
        return n == 1 && cond(ops[0], cc) ? out.put({0xC0 + 8 * cc}) : -1;
    }
    if (mnem == "JP" || mnem == "CALL") {
        const bool jp = mnem == "JP";
        if (n == 1 && ops[0].kind == Kind::Imm) return out.put16(jp ? 0xC3 : 0xCD, ops[0].val);
        if (n == 1 && jp && ((ops[0].kind == Kind::Rp && ops[0].reg == 2) || isR8(ops[0], 6))) return out.put({0xE9});
        if (n == 2 && cond(ops[0], cc) && ops[1].kind == Kind::Imm) return out.put16((jp ? 0xC2 : 0xC4) + 8 * cc, ops[1].val);
        return -1;
    }
    if (mnem == "JR") {
        const Operand* target = nullptr;
        int op = 0x18;
        if (n == 1) target = &ops[0];
        else if (n == 2 && cond(ops[0], cc)) { target = &ops[1]; op = 0x20 + 8 * cc; }
        if (!target || target->kind != Kind::Imm) return -1;
        const int32_t d = (int32_t)(target->val & 0xFFFF) - (int32_t)((currentAddr + 2) & 0xFFFF);
        const int32_t off = d > 0x7FFF ? d - 0x10000 : d < -0x8000 ? d + 0x10000 : d;    // (wrapping at 64K)
        return fitsSigned8(off) ? out.put({op, off & 0xFF}) : -1;
    }
    if (mnem == "RST") {
        if (n != 1 || ops[0].kind != Kind::Imm || ops[0].val < 0 || ops[0].val > 0x38 || (ops[0].val & 7)) return -1;
        return out.put({0xC7 + ops[0].val});
    }

    // Stack
    if (mnem == "PUSH" || mnem == "POP") {
        if (n != 1) return -1;
        int p;
        if (ops[0].kind == Kind::Af) p = 3;
        else if (ops[0].kind == Kind::Rp && ops[0].reg < 3) p = ops[0].reg;
        else return -1;
        return out.put({(mnem == "PUSH" ? 0xC5 : 0xC1) + 16 * p});
    }

    // Increments
    if (mnem == "INC" || mnem == "DEC") {
        const bool inc = mnem == "INC";
        if (n != 1) return -1;
        if (isR8(ops[0])) return out.put({(inc ? 0x04 : 0x05) + 8 * ops[0].reg});
        if (ops[0].kind == Kind::Rp) return out.put({(inc ? 0x03 : 0x0B) + 16 * ops[0].reg});
        return -1;
    }

    // Arithmetic and logic: ADD A,x (or ADD x), ADD HL,rp, ADD SP,e, and the rest with or without A,
    static const char* const ALU[8] = {"ADD", "ADC", "SUB", "SBC", "AND", "XOR", "OR", "CP"};
    for (int y = 0; y < 8; ++y) {
        if (mnem != ALU[y]) continue;
        if (y == 0 && n == 2 && ops[0].kind == Kind::Rp && ops[0].reg == 2 && ops[1].kind == Kind::Rp) return out.put({0x09 + 16 * ops[1].reg});
        if (y == 0 && n == 2 && ops[0].kind == Kind::Rp && ops[0].reg == 3 && ops[1].kind == Kind::Imm) {
            return fitsSigned8(ops[1].val) ? out.put({0xE8, ops[1].val & 0xFF}) : -1;
        }
        const Operand* src = nullptr;
        if (n == 1) src = &ops[0];
        else if (n == 2 && isA(ops[0])) src = &ops[1];
        if (!src) return -1;
        if (isR8(*src)) return out.put({0x80 + 8 * y + src->reg});
        if (src->kind == Kind::Imm && fits8(src->val)) return out.put({0xC6 + 8 * y, src->val & 0xFF});
        return -1;
    }

    // CB: rotates and shifts, BIT RES SET
    static const char* const ROT[8] = {"RLC", "RRC", "RL", "RR", "SLA", "SRA", "SWAP", "SRL"};
    for (int y = 0; y < 8; ++y) {
        if (mnem == ROT[y]) return n == 1 && isR8(ops[0]) ? out.put({0xCB, 8 * y + ops[0].reg}) : -1;
    }
    if (mnem == "BIT" || mnem == "RES" || mnem == "SET") {
        if (n != 2 || ops[0].kind != Kind::Imm || ops[0].val < 0 || ops[0].val > 7 || !isR8(ops[1])) return -1;
        const int base = mnem == "BIT" ? 0x40 : mnem == "RES" ? 0x80 : 0xC0;
        return out.put({0xCB, base + 8 * ops[0].val + ops[1].reg});
    }

    // Loads
    if (mnem == "LDI" || mnem == "LDD") {
        // LDI (HL),A / LDI A,(HL): LD (HL+),A / LD A,(HL+)
        if (n != 2) return -1;
        const int base = mnem == "LDI" ? 0x22 : 0x32;
        if (isR8(ops[0], 6) && isA(ops[1])) return out.put({base});
        if (isA(ops[0]) && isR8(ops[1], 6)) return out.put({base + 8});
        return -1;
    }
    if (mnem == "LDH") {
        if (n != 2) return -1;
        const bool store = isA(ops[1]);
        const Operand& m = store ? ops[0] : ops[1];
        if (!store && !isA(ops[0])) return -1;
        if (m.kind == Kind::MemC) return out.put({store ? 0xE2 : 0xF2});
        if (m.kind != Kind::MemImm) return -1;
        int32_t a = m.val;
        if (a >= 0xFF00 && a <= 0xFFFF) a -= 0xFF00;
        if (a < 0 || a > 0xFF) return -1;
        return out.put({store ? 0xE0 : 0xF0, a});
    }
    if (mnem == "LD") {
        if (n != 2) return -1;
        const Operand& d = ops[0];
        const Operand& s = ops[1];
        if (isR8(d) && isR8(s)) return d.reg == 6 && s.reg == 6 ? -1 : out.put({0x40 + 8 * d.reg + s.reg});
        if (isR8(d) && s.kind == Kind::Imm) return fits8(s.val) ? out.put({0x06 + 8 * d.reg, s.val & 0xFF}) : -1;
        if (d.kind == Kind::Rp && d.reg == 3 && s.kind == Kind::Rp && s.reg == 2) return out.put({0xF9});
        if (d.kind == Kind::Rp && d.reg == 2 && s.kind == Kind::SpOff) return fitsSigned8(s.val) ? out.put({0xF8, s.val & 0xFF}) : -1;
        if (d.kind == Kind::Rp && s.kind == Kind::Imm) return out.put16(0x01 + 16 * d.reg, s.val);
        if (d.kind == Kind::MemImm && s.kind == Kind::Rp && s.reg == 3) return out.put16(0x08, d.val);
        static const Kind IND[4] = {Kind::MemBc, Kind::MemDe, Kind::MemHli, Kind::MemHld};
        for (int p = 0; p < 4; ++p) {
            if (d.kind == IND[p] && isA(s)) return out.put({0x02 + 16 * p});
            if (isA(d) && s.kind == IND[p]) return out.put({0x0A + 16 * p});
        }
        if (d.kind == Kind::MemC && isA(s)) return out.put({0xE2});
        if (isA(d) && s.kind == Kind::MemC) return out.put({0xF2});
        if (d.kind == Kind::MemImm && isA(s)) return out.put16(0xEA, d.val);
        if (isA(d) && s.kind == Kind::MemImm) return out.put16(0xFA, s.val);
        return -1;
    }
    return -1;
}
