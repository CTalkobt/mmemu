#include "basic_tokenizer.h"
#include <algorithm>
#include <cctype>
#include <map>
#include <sstream>
#include <utility>
#include <cstdlib>
#include <cstring>

namespace basic_tokenizer {

namespace {

using Tokens = std::vector<std::pair<std::string, std::vector<uint8_t>>>;

// BASIC V2 (C64 / VIC-20 / PET): $80-$CB, plus pi ($FF, written {pi})
const Tokens& basic2Table() {
    static const Tokens t = [] {
        static const char* names[] = {
            "END", "FOR", "NEXT", "DATA", "INPUT#", "INPUT", "DIM", "READ", "LET", "GOTO", "RUN", "IF",
            "RESTORE", "GOSUB", "RETURN", "REM", "STOP", "ON", "WAIT", "LOAD", "SAVE", "VERIFY", "DEF",
            "POKE", "PRINT#", "PRINT", "CONT", "LIST", "CLR", "CMD", "SYS", "OPEN", "CLOSE", "GET", "NEW",
            "TAB(", "TO", "FN", "SPC(", "THEN", "NOT", "STEP", "+", "-", "*", "/", "^", "AND", "OR", ">",
            "=", "<", "SGN", "INT", "ABS", "USR", "FRE", "POS", "SQR", "RND", "LOG", "EXP", "COS", "SIN",
            "TAN", "ATN", "PEEK", "LEN", "STR$", "VAL", "ASC", "CHR$", "LEFT$", "RIGHT$", "MID$", "GO",
        };
        Tokens out;
        uint8_t tok = 0x80;
        for (const char* n : names) out.push_back({n, {tok++}});
        return out;
    }();
    return t;
}

// BASIC 10 / BASIC 65 additions (MEGA65 book, appendix B)
const Tokens& basic65Extra() {
    static const Tokens t = {
        {"RGRAPHIC", {0xCC}}, {"RCOLOR", {0xCD}}, {"JOY", {0xCF}}, {"RPEN", {0xD0}}, {"DEC", {0xD1}},
        {"HEX$", {0xD2}}, {"ERR$", {0xD3}}, {"INSTR", {0xD4}}, {"ELSE", {0xD5}}, {"RESUME", {0xD6}},
        {"TRAP", {0xD7}}, {"TRON", {0xD8}}, {"TROFF", {0xD9}}, {"SOUND", {0xDA}}, {"VOL", {0xDB}},
        {"AUTO", {0xDC}}, {"GRAPHIC", {0xDE}}, {"PAINT", {0xDF}}, {"CHAR", {0xE0}}, {"BOX", {0xE1}},
        {"CIRCLE", {0xE2}}, {"LINE", {0xE5}}, {"MERGE", {0xE6}}, {"COLOR", {0xE7}}, {"SCNCLR", {0xE8}},
        {"XOR", {0xE9}}, {"HELP", {0xEA}}, {"DO", {0xEB}}, {"LOOP", {0xEC}}, {"WHILE", {0xED}},
        {"DIR", {0xEE}}, {"DSAVE", {0xEF}}, {"DLOAD", {0xF0}}, {"HEADER", {0xF1}}, {"SCRATCH", {0xF2}},
        {"COLLECT", {0xF3}}, {"COPY", {0xF4}}, {"RENAME", {0xF5}}, {"BACKUP", {0xF6}}, {"DELETE", {0xF7}},
        {"RENUMBER", {0xF8}}, {"KEY", {0xF9}}, {"MONITOR", {0xFA}}, {"USING", {0xFB}}, {"UNTIL", {0xFC}},
        {"EXIT", {0xFD}},
        // $CE xx
        {"POT", {0xCE, 0x02}}, {"BUMP", {0xCE, 0x03}}, {"LPEN", {0xCE, 0x04}}, {"RSPPOS", {0xCE, 0x05}},
        {"RSPRITE", {0xCE, 0x06}}, {"RSPCOLOR", {0xCE, 0x07}}, {"LOG10", {0xCE, 0x08}},
        {"RWINDOW", {0xCE, 0x09}}, {"POINTER", {0xCE, 0x0A}}, {"PIXEL", {0xCE, 0x0C}},
        // $FE xx
        {"BANK", {0xFE, 0x02}}, {"FILTER", {0xFE, 0x03}}, {"PLAY", {0xFE, 0x04}}, {"TEMPO", {0xFE, 0x05}},
        {"MOVSPR", {0xFE, 0x06}}, {"SPRITE", {0xFE, 0x07}}, {"SPRCOLOR", {0xFE, 0x08}}, {"RREG", {0xFE, 0x09}},
        {"ENVELOPE", {0xFE, 0x0A}}, {"SLEEP", {0xFE, 0x0B}}, {"CATALOG", {0xFE, 0x0C}}, {"DOPEN", {0xFE, 0x0D}},
        {"APPEND", {0xFE, 0x0E}}, {"DCLOSE", {0xFE, 0x0F}}, {"BSAVE", {0xFE, 0x10}}, {"BLOAD", {0xFE, 0x11}},
        {"RECORD", {0xFE, 0x12}}, {"CONCAT", {0xFE, 0x13}}, {"DVERIFY", {0xFE, 0x14}}, {"DCLEAR", {0xFE, 0x15}},
        {"SPRSAV", {0xFE, 0x16}}, {"COLLISION", {0xFE, 0x17}}, {"BEGIN", {0xFE, 0x18}}, {"BEND", {0xFE, 0x19}},
        {"WINDOW", {0xFE, 0x1A}}, {"BOOT", {0xFE, 0x1B}}, {"FREAD", {0xFE, 0x1C}}, {"FWRITE", {0xFE, 0x1E}},
        {"DMA", {0xFE, 0x1F}}, {"EDMA", {0xFE, 0x21}}, {"OFF", {0xFE, 0x24}}, {"FAST", {0xFE, 0x25}},
        {"SLOW", {0xFE, 0x26}}, {"TYPE", {0xFE, 0x27}}, {"BVERIFY", {0xFE, 0x28}}, {"ERASE", {0xFE, 0x2A}},
        {"FIND", {0xFE, 0x2B}}, {"CHANGE", {0xFE, 0x2C}}, {"SET", {0xFE, 0x2D}}, {"SCREEN", {0xFE, 0x2E}},
        {"POLYGON", {0xFE, 0x2F}}, {"ELLIPSE", {0xFE, 0x30}}, {"VIEWPORT", {0xFE, 0x31}}, {"PEN", {0xFE, 0x33}},
        {"PALETTE", {0xFE, 0x34}}, {"DMODE", {0xFE, 0x35}}, {"DPAT", {0xFE, 0x36}}, {"FOREGROUND", {0xFE, 0x39}},
        {"BACKGROUND", {0xFE, 0x3B}}, {"BORDER", {0xFE, 0x3C}}, {"HIGHLIGHT", {0xFE, 0x3D}}, {"MOUSE", {0xFE, 0x3E}},
        {"RMOUSE", {0xFE, 0x3F}}, {"DISK", {0xFE, 0x40}}, {"RCURSOR", {0xFE, 0x42}}, {"LOADIFF", {0xFE, 0x43}},
        {"FONT", {0xFE, 0x46}},
    };
    return t;
}

const std::map<std::string, uint8_t>& escapeNames() {
    static const std::map<std::string, uint8_t> m = {
        {"clr", 0x93}, {"home", 0x13}, {"up", 0x91}, {"down", 0x11}, {"left", 0x9D}, {"right", 0x1D},
        {"rvs on", 0x12}, {"rvs off", 0x92}, {"rvon", 0x12}, {"rvof", 0x92}, {"del", 0x14}, {"inst", 0x94},
        {"blk", 0x90}, {"wht", 0x05}, {"red", 0x1C}, {"cyn", 0x9F}, {"pur", 0x9C}, {"grn", 0x1E},
        {"blu", 0x1F}, {"yel", 0x9E}, {"orng", 0x81}, {"brn", 0x95}, {"lred", 0x96}, {"gry1", 0x97},
        {"gry2", 0x98}, {"lgrn", 0x99}, {"lblu", 0x9A}, {"gry3", 0x9B},
        {"f1", 0x85}, {"f3", 0x86}, {"f5", 0x87}, {"f7", 0x88}, {"f2", 0x89}, {"f4", 0x8A},
        {"f6", 0x8B}, {"f8", 0x8C}, {"return", 0x0D}, {"space", 0x20}, {"pi", 0xFF},
    };
    return m;
}

bool iequalsPrefix(const std::string& s, size_t pos, const std::string& kw) {
    if (pos + kw.size() > s.size()) return false;
    for (size_t i = 0; i < kw.size(); ++i)
        if (toupper((unsigned char)s[pos + i]) != (unsigned char)kw[i]) return false;
    return true;
}

}  // namespace

uint16_t defaultStart(Dialect d) { return d == Dialect::Basic65 ? 0x2001 : 0x0801; }

bool parseDialect(const std::string& nameIn, Dialect& out) {
    std::string n = nameIn;
    std::transform(n.begin(), n.end(), n.begin(), ::tolower);
    if (n == "basic2" || n == "v2" || n == "c64" || n == "vic20" || n == "pet") { out = Dialect::Basic2; return true; }
    if (n == "basic65" || n == "65" || n == "mega65" || n == "basic10") { out = Dialect::Basic65; return true; }
    return false;
}

Result tokenize(const std::string& text, Dialect dialect, uint16_t start, const std::string& fileName) {
    Result res;
    Tokens table = basic2Table();
    if (dialect == Dialect::Basic65)
        for (const auto& t : basic65Extra()) table.push_back(t);

    auto error = [&](int srcLine, const std::string& msg) {
        res.errors.push_back(fileName + ":" + std::to_string(srcLine) + ": error: " + msg);
    };

    std::vector<uint8_t> body;   // program bytes from `start`
    long lastLineNo = -1;
    std::istringstream in(text);
    std::string line;
    int srcLine = 0;
    while (std::getline(in, line)) {
        ++srcLine;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t p = line.find_first_not_of(" \t");
        if (p == std::string::npos || line[p] == ';' || line[p] == '#') continue;
        if (!isdigit((unsigned char)line[p])) { error(srcLine, "expected a line number"); continue; }

        long lineNo = 0;
        while (p < line.size() && isdigit((unsigned char)line[p])) {
            lineNo = lineNo * 10 + (line[p] - '0');
            if (lineNo > 63999) break;
            ++p;
        }
        if (lineNo > 63999) { error(srcLine, "line number above 63999"); continue; }
        if (lineNo <= lastLineNo) {
            error(srcLine, "line number " + std::to_string(lineNo) + " does not follow " + std::to_string(lastLineNo));
            continue;
        }
        lastLineNo = lineNo;
        while (p < line.size() && (line[p] == ' ' || line[p] == '\t')) ++p;

        std::vector<uint8_t> out;
        bool inQuote = false, inRem = false, inData = false, lineOk = true;
        while (p < line.size() && lineOk) {
            unsigned char c = (unsigned char)line[p];

            // {escape}
            if (c == '{') {
                size_t close = line.find('}', p);
                if (close == std::string::npos) { error(srcLine, "unterminated {"); lineOk = false; break; }
                std::string name = line.substr(p + 1, close - p - 1);
                int repeat = 1;
                if (size_t star = name.find('*'); star != std::string::npos) {
                    repeat = std::atoi(name.c_str() + star + 1);
                    name.resize(star);
                }
                std::transform(name.begin(), name.end(), name.begin(), ::tolower);
                int value = -1;
                if (!name.empty() && name[0] == '$') value = (int)std::strtol(name.c_str() + 1, nullptr, 16);
                else if (!name.empty() && isdigit((unsigned char)name[0])) value = std::atoi(name.c_str());
                else if (auto it = escapeNames().find(name); it != escapeNames().end()) value = it->second;
                if (value < 0 || value > 255 || repeat < 1) {
                    error(srcLine, "unknown escape {" + line.substr(p + 1, close - p - 1) + "}");
                    lineOk = false;
                    break;
                }
                for (int i = 0; i < repeat; ++i) out.push_back((uint8_t)value);
                p = close + 1;
                continue;
            }
            // UTF-8 pound / pi / arrows
            if (c >= 0x80) {
                static const std::pair<const char*, uint8_t> utf8[] = {
                    {"\xC2\xA3", 0x5C}, {"\xCF\x80", 0xFF}, {"\xE2\x86\x91", 0x5E}, {"\xE2\x86\x90", 0x5F}};
                bool matched = false;
                for (const auto& [seq, v] : utf8)
                    if (line.compare(p, strlen(seq), seq) == 0) { out.push_back(v); p += strlen(seq); matched = true; break; }
                if (!matched) { error(srcLine, "character not in PETSCII"); lineOk = false; }
                continue;
            }

            const bool literal = inQuote || inRem || (inData && c != ':');
            if (!literal) {
                if (c == '?') { out.push_back(0x99); ++p; continue; }   // PRINT
                // Longest keyword match
                size_t bestLen = 0;
                const std::vector<uint8_t>* best = nullptr;
                for (const auto& [kw, tok] : table)
                    if (kw.size() > bestLen && iequalsPrefix(line, p, kw)) { bestLen = kw.size(); best = &tok; }
                if (best) {
                    out.insert(out.end(), best->begin(), best->end());
                    if (best->size() == 1 && (*best)[0] == 0x8F) inRem = true;    // REM
                    if (best->size() == 1 && (*best)[0] == 0x83) inData = true;   // DATA
                    p += bestLen;
                    continue;
                }
            }
            if (c == '"') inQuote = !inQuote;
            if (inData && !inQuote && c == ':') inData = false;
            // ASCII -> PETSCII: letters unshifted, \ = pound, ^ = up arrow, _ = left arrow
            uint8_t pet;
            if (c >= 'a' && c <= 'z') pet = (uint8_t)(c - 'a' + 0x41);
            else if (c == '\\') pet = 0x5C;
            else if (c == '_') pet = 0x5F;
            else if (c == '\t') pet = 0x20;
            else if (c >= 0x20 && c <= 0x5E) pet = c;
            else { error(srcLine, std::string("character '") + (char)c + "' not in PETSCII"); lineOk = false; break; }
            out.push_back(pet);
            ++p;
        }
        if (!lineOk) continue;
        if (out.size() > 250) { error(srcLine, "line too long when tokenised (" + std::to_string(out.size()) + " bytes)"); continue; }

        // link (filled below), line number, tokens, terminator
        size_t lineStart = body.size();
        body.push_back(0); body.push_back(0);
        body.push_back(lineNo & 0xFF); body.push_back((lineNo >> 8) & 0xFF);
        body.insert(body.end(), out.begin(), out.end());
        body.push_back(0);
        uint16_t next = (uint16_t)(start + body.size());
        body[lineStart] = next & 0xFF;
        body[lineStart + 1] = next >> 8;
    }
    if (!res.errors.empty()) return res;
    body.push_back(0); body.push_back(0);   // end of program
    res.prg.push_back(start & 0xFF);
    res.prg.push_back(start >> 8);
    res.prg.insert(res.prg.end(), body.begin(), body.end());
    return res;
}

}  // namespace basic_tokenizer
