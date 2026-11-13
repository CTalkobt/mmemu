// The MC6801/MC6803 line assembler: the opcode map by mnemonic and
// addressing mode.

#include "asm6801.h"

#include <cctype>
#include <map>
#include <vector>

namespace {

/** Opcodes of a mnemonic by mode (-1: none). */
struct Op {
    int inh = -1, imm = -1, dir = -1, idx = -1, ext = -1, rel = -1;
    bool wide = false;           // a 16-bit immediate
};

std::map<std::string, Op> buildTable() {
    std::map<std::string, Op> t;
    auto inh = [&](const char* n, int op) { t[n].inh = op; };
    // Inherent
    const std::pair<const char*, int> INH[] = {
        {"NOP", 0x01}, {"LSRD", 0x04}, {"ASLD", 0x05}, {"LSLD", 0x05}, {"TAP", 0x06}, {"TPA", 0x07},
        {"INX", 0x08}, {"DEX", 0x09}, {"CLV", 0x0A}, {"SEV", 0x0B}, {"CLC", 0x0C}, {"SEC", 0x0D},
        {"CLI", 0x0E}, {"SEI", 0x0F}, {"SBA", 0x10}, {"CBA", 0x11}, {"TAB", 0x16}, {"TBA", 0x17},
        {"DAA", 0x19}, {"ABA", 0x1B}, {"TSX", 0x30}, {"INS", 0x31}, {"PULA", 0x32}, {"PULB", 0x33},
        {"DES", 0x34}, {"TXS", 0x35}, {"PSHA", 0x36}, {"PSHB", 0x37}, {"PULX", 0x38}, {"RTS", 0x39},
        {"ABX", 0x3A}, {"RTI", 0x3B}, {"PSHX", 0x3C}, {"MUL", 0x3D}, {"WAI", 0x3E}, {"SWI", 0x3F},
    };
    for (const auto& i : INH) inh(i.first, i.second);
    // Read-modify-write: A, B, n,X, nnnn
    const std::pair<const char*, int> RMW[] = {
        {"NEG", 0x0}, {"COM", 0x3}, {"LSR", 0x4}, {"ROR", 0x6}, {"ASR", 0x7}, {"ASL", 0x8}, {"LSL", 0x8},
        {"ROL", 0x9}, {"DEC", 0xA}, {"INC", 0xC}, {"TST", 0xD}, {"CLR", 0xF},
    };
    for (const auto& r : RMW) {
        inh((std::string(r.first) + "A").c_str(), 0x40 + r.second);
        inh((std::string(r.first) + "B").c_str(), 0x50 + r.second);
        t[r.first].idx = 0x60 + r.second;
        t[r.first].ext = 0x70 + r.second;
    }
    t["JMP"].idx = 0x6E;
    t["JMP"].ext = 0x7E;
    // Branches
    const char* const BR[16] = {"BRA", "BRN", "BHI", "BLS", "BCC", "BCS", "BNE", "BEQ",
                                "BVC", "BVS", "BPL", "BMI", "BGE", "BLT", "BGT", "BLE"};
    for (int i = 0; i < 16; ++i) t[BR[i]].rel = 0x20 + i;
    t["BHS"].rel = 0x24;
    t["BLO"].rel = 0x25;
    t["BSR"].rel = 0x8D;
    // The accumulator group: imm, dir, idx, ext at base, +$10, +$20, +$30
    auto group = [&](const std::string& n, int base, bool imm, bool wide) {
        Op& o = t[n];
        if (imm) o.imm = base;
        o.dir = base + 0x10;
        o.idx = base + 0x20;
        o.ext = base + 0x30;
        o.wide = wide;
    };
    const char* const ACC[12] = {"SUB", "CMP", "SBC", nullptr, "AND", "BIT", "LDA", "STA", "EOR", "ADC", "ORA", "ADD"};
    for (int i = 0; i < 12; ++i) {
        if (!ACC[i]) continue;
        const bool imm = i != 7;
        group(std::string(ACC[i]) + "A", 0x80 + i, imm, false);
        group(std::string(ACC[i]) + "B", 0xC0 + i, imm, false);
    }
    // LDA/STA/ORA and B as short forms of LDAA/STAA/ORAA
    for (const char* s : {"LDA", "STA", "ORA"}) {
        t[s] = t[std::string(s) + "A"];
        t[std::string(s).substr(0, 2) + "B"] = t[std::string(s) + "B"];
    }
    group("SUBD", 0x83, true, true);
    group("ADDD", 0xC3, true, true);
    group("CPX", 0x8C, true, true);
    group("LDD", 0xCC, true, true);
    group("LDS", 0x8E, true, true);
    group("LDX", 0xCE, true, true);
    group("STD", 0xCD, false, true);
    group("STS", 0x8F, false, true);
    group("STX", 0xCF, false, true);
    group("JSR", 0x8D, false, false);
    return t;
}

/** A number: $FF, 0xFF, FFh, %1010, 255, 'A', with a leading - or +. */
bool parseNumber(const std::string& s, int32_t& out) {
    if (s.empty()) return false;
    if (s.size() == 3 && s[0] == '\'' && s[2] == '\'') { out = (uint8_t)s[1]; return true; }
    size_t i = 0;
    bool neg = false;
    if (s[0] == '-' || s[0] == '+') { neg = s[0] == '-'; i = 1; }
    std::string d = s.substr(i);
    int base = 10;
    if (!d.empty() && d[0] == '$') { base = 16; d = d.substr(1); }
    else if (d.size() > 2 && d[0] == '0' && d[1] == 'X') { base = 16; d = d.substr(2); }
    else if (!d.empty() && d[0] == '%') { base = 2; d = d.substr(1); }
    else if (d.size() > 1 && d.back() == 'H' && std::isdigit((unsigned char)d[0])) { base = 16; d.pop_back(); }
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

std::vector<std::string> splitCommas(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    bool quote = false;
    for (char c : s) {
        if (c == '\'') quote = !quote;
        if (c == ',' && !quote) { out.push_back(cur); cur.clear(); continue; }
        cur += c;
    }
    out.push_back(cur);
    return out;
}

}  // namespace

AssemblerResult Assembler6801::assemble(const std::string& sourcePath, const std::string& outputPath) {
    (void)sourcePath; (void)outputPath;
    return {false, "File assembly not supported by the 6803 line assembler", "", "", "", 0, 0};
}

int Assembler6801::assembleLine(const std::string& line, uint8_t* buf, int bufsz, uint32_t currentAddr) {
    static const std::map<std::string, Op> table = buildTable();
    // Upper case (not inside quotes), the comment off; the mnemonic, then the operand without spaces
    std::string text;
    bool quote = false;
    for (char c : line) {
        if (c == '\'') quote = !quote;
        if (c == ';' && !quote) break;
        text += quote ? c : (char)std::toupper((unsigned char)c);
    }
    const size_t start = text.find_first_not_of(" \t");
    if (start == std::string::npos) return 0;
    size_t sp = text.find_first_of(" \t", start);
    const std::string mn = text.substr(start, sp == std::string::npos ? std::string::npos : sp - start);
    std::string arg;
    if (sp != std::string::npos) {
        for (char c : text.substr(sp)) if (c != ' ' && c != '\t') arg += c;
    }
    int n = 0;
    auto emit = [&](int v) { if (n < bufsz) buf[n] = (uint8_t)v; ++n; };

    if (mn == "FCB" || mn == "DB" || mn == ".BYTE" || mn == "FDB" || mn == "DW" || mn == ".WORD") {
        const bool word = mn == "FDB" || mn == "DW" || mn == ".WORD";
        if (arg.empty()) return -1;
        for (const auto& item : splitCommas(arg)) {
            int32_t v;
            if (!parseNumber(item, v)) return -1;
            if (word) { emit((v >> 8) & 0xFF); emit(v & 0xFF); }
            else { if (v < -128 || v > 255) return -1; emit(v & 0xFF); }
        }
        return n <= bufsz ? n : -1;
    }

    const auto it = table.find(mn);
    if (it == table.end()) return -1;
    const Op& op = it->second;

    if (arg.empty()) {
        if (op.inh < 0) return -1;
        emit(op.inh);
        return n <= bufsz ? n : -1;
    }
    if (op.rel >= 0) {
        int32_t target;
        if (!parseNumber(arg, target)) return -1;
        const int32_t off = (target & 0xFFFF) - (int32_t)((currentAddr + 2) & 0xFFFF);
        int32_t o = off;
        if (o > 0x7FFF) o -= 0x10000;
        if (o < -0x8000) o += 0x10000;
        if (o < -128 || o > 127) return -1;
        emit(op.rel);
        emit(o & 0xFF);
        return n <= bufsz ? n : -1;
    }
    if (arg[0] == '#') {
        int32_t v;
        if (op.imm < 0 || !parseNumber(arg.substr(1), v)) return -1;
        emit(op.imm);
        if (op.wide) { emit((v >> 8) & 0xFF); emit(v & 0xFF); }
        else { if (v < -128 || v > 255) return -1; emit(v & 0xFF); }
        return n <= bufsz ? n : -1;
    }
    const auto comma = arg.rfind(',');
    if (comma != std::string::npos) {
        if (arg.substr(comma + 1) != "X" || op.idx < 0) return -1;
        int32_t v = 0;
        const std::string off = arg.substr(0, comma);
        if (!off.empty() && !parseNumber(off, v)) return -1;
        if (v < 0 || v > 255) return -1;
        emit(op.idx);
        emit(v);
        return n <= bufsz ? n : -1;
    }
    bool forceDir = false, forceExt = false;
    std::string a = arg;
    if (a[0] == '<') { forceDir = true; a = a.substr(1); }
    else if (a[0] == '>') { forceExt = true; a = a.substr(1); }
    else if (a.size() == 5 && a[0] == '$') forceExt = true;     // $0012: written as extended
    int32_t v;
    if (!parseNumber(a, v) || v < 0) return -1;
    if (op.dir >= 0 && !forceExt && (forceDir || v <= 0xFF)) {
        if (v > 0xFF) return -1;
        emit(op.dir);
        emit(v);
    } else {
        if (op.ext < 0 || forceDir) return -1;
        emit(op.ext);
        emit((v >> 8) & 0xFF);
        emit(v & 0xFF);
    }
    return n <= bufsz ? n : -1;
}
