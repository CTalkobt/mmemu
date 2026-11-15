// The 8085 line assembler: the line is matched against the opcode table the
// disassembler writes from (opcodes8085.cpp), so the two always agree.

#include "asm8085.h"
#include "opcodes8085.h"

#include <cctype>
#include <vector>

namespace {

/** A number: $FF, 0xFF, 0FFH, %1010, 255, with a leading - or +. */
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
    else if (d.size() > 1 && d.back() == 'B' && std::isdigit((unsigned char)d[0]) &&
             d.find_first_not_of("01", 0) == d.size() - 1) { base = 2; d.pop_back(); }
    if (d.empty() || d.size() > 16) return false;
    int64_t v = 0;
    for (char c : d) {
        int n;
        if (c >= '0' && c <= '9') n = c - '0';
        else if (c >= 'A' && c <= 'F') n = 10 + c - 'A';
        else return false;
        if (n >= base) return false;
        v = v * base + n;
        if (v > 0xFFFF) return false;
    }
    out = (int32_t)(neg ? -v : v);
    return true;
}

/** Other names for the undocumented instructions (and the 8080's for none). */
std::string canonical(const std::string& m) {
    static const struct { const char* alias; const char* name; } ALIASES[] = {
        {"ASHR", "ARHL"}, {"RLDE", "RDEL"}, {"LDEH", "LDHI"}, {"LDES", "LDSI"},
        {"JNX5", "JNK"}, {"JNUI", "JNK"}, {"JX5", "JK"}, {"JUI", "JK"}, {"OVRST8", "RSTV"},
    };
    for (const auto& a : ALIASES) if (m == a.alias) return a.name;
    return m;
}

std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> parts;
    if (s.empty()) return parts;
    size_t from = 0;
    while (true) {
        const size_t comma = s.find(',', from);
        parts.push_back(s.substr(from, comma == std::string::npos ? std::string::npos : comma - from));
        if (comma == std::string::npos) break;
        from = comma + 1;
    }
    return parts;
}

}  // namespace

AssemblerResult Assembler8085::assemble(const std::string& sourcePath, const std::string& outputPath) {
    (void)sourcePath; (void)outputPath;
    return {false, "File assembly not supported by the 8085 line assembler", "", "", "", 0, 0};
}

int Assembler8085::assembleLine(const std::string& line, uint8_t* buf, int bufsz, uint32_t currentAddr) {
    (void)currentAddr;
    // Upper case, the comment off; the mnemonic, then the operands with no spaces
    std::string text;
    for (char c : line) {
        if (c == ';') break;
        text += (char)std::toupper((unsigned char)c);
    }
    const size_t start = text.find_first_not_of(" \t");
    if (start == std::string::npos) return 0;
    const size_t end = text.find_first_of(" \t", start);
    const std::string mnem = canonical(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
    std::string rest;
    if (end != std::string::npos) for (char c : text.substr(end)) if (c != ' ' && c != '\t') rest += c;
    const std::vector<std::string> ops = split(rest);

    // Data
    if (mnem == "DB" || mnem == "DW") {
        const int each = mnem == "DB" ? 1 : 2;
        if (ops.empty() || (int)ops.size() * each > bufsz) return -1;
        int at = 0;
        for (const auto& o : ops) {
            int32_t v;
            if (!parseNumber(o, v) || (each == 1 && (v < -128 || v > 0xFF))) return -1;
            buf[at++] = (uint8_t)v;
            if (each == 2) buf[at++] = (uint8_t)(v >> 8);
        }
        return at;
    }

    // RST takes 0-7 or the address (0, 8, ... $38)
    std::vector<std::string> args = ops;
    if (mnem == "RST" && args.size() == 1) {
        int32_t v;
        if (!parseNumber(args[0], v)) return -1;
        if (v >= 8 && v <= 0x38 && (v & 7) == 0) v /= 8;
        if (v < 0 || v > 7) return -1;
        args[0] = std::string(1, (char)('0' + v));
    }

    for (int op = 0; op < 256; ++op) {
        const I8085Opcode& o = i8085Opcode((uint8_t)op);
        if (o.mnemonic != mnem) continue;
        const std::vector<std::string> tmpl = split(o.operands);
        if (tmpl.size() != args.size()) continue;
        uint8_t bytes[3] = {(uint8_t)op, 0, 0};
        int len = 1;
        bool ok = true;
        for (size_t i = 0; i < tmpl.size() && ok; ++i) {
            if (tmpl[i] == "n" || tmpl[i] == "nn") {
                int32_t v;
                if (!parseNumber(args[i], v)) { ok = false; break; }
                if (tmpl[i] == "n") {
                    if (v < -128 || v > 0xFF) { ok = false; break; }
                    bytes[len++] = (uint8_t)v;
                } else {
                    bytes[len++] = (uint8_t)v;
                    bytes[len++] = (uint8_t)(v >> 8);
                }
            } else if (tmpl[i] != args[i]) {
                ok = false;
            }
        }
        if (!ok) continue;
        if (len > bufsz) return -1;
        for (int i = 0; i < len; ++i) buf[i] = bytes[i];
        return len;
    }
    return -1;
}
