#include "assembler_6502.h"
#include <sstream>
#include <algorithm>
#include <iostream>
#include <iomanip>
#include <cstdint>
#include <vector>
#include <regex>

Assembler6502::Assembler6502(bool cmos) : m_cmos(cmos) {
    initTable();
    if (m_cmos) initCmos();
}

void Assembler6502::add(const std::string& mnem, const std::string& mode, uint8_t op, int sz) {
    m_mnemonics[mnem][mode] = {op, sz};
}

void Assembler6502::initTable() {
    // Standard 6502 Opcodes
    auto addStd = [&](const std::string& m, uint8_t imm, uint8_t zp, uint8_t zpx, uint8_t abs, uint8_t absx, uint8_t absy, uint8_t indx, uint8_t indy) {
        if (imm)  add(m, "imm", imm, 2);
        if (zp)   add(m, "zp", zp, 2);
        if (zpx)  add(m, "zpx", zpx, 2);
        if (abs)  add(m, "abs", abs, 3);
        if (absx) add(m, "absx", absx, 3);
        if (absy) add(m, "absy", absy, 3);
        if (indx) add(m, "zpxind", indx, 2);
        if (indy) add(m, "indzpy", indy, 2);
    };

    addStd("ADC", 0x69, 0x65, 0x75, 0x6D, 0x7D, 0x79, 0x61, 0x71);
    addStd("AND", 0x29, 0x25, 0x35, 0x2D, 0x3D, 0x39, 0x21, 0x31);
    addStd("CMP", 0xC9, 0xC5, 0xD5, 0xCD, 0xDD, 0xD9, 0xC1, 0xD1);
    addStd("EOR", 0x49, 0x45, 0x55, 0x4D, 0x5D, 0x59, 0x41, 0x51);
    addStd("LDA", 0xA9, 0xA5, 0xB5, 0xAD, 0xBD, 0xB9, 0xA1, 0xB1);
    addStd("ORA", 0x09, 0x05, 0x15, 0x0D, 0x1D, 0x19, 0x01, 0x11);
    addStd("SBC", 0xE9, 0xE5, 0xF5, 0xED, 0xFD, 0xF9, 0xE1, 0xF1);
    addStd("STA", 0,    0x85, 0x95, 0x8D, 0x9D, 0x99, 0x81, 0x91);

    auto addShift = [&](const std::string& m, uint8_t acc, uint8_t zp, uint8_t zpx, uint8_t abs, uint8_t absx) {
        if (acc)  add(m, "acc", acc, 1);
        if (zp)   add(m, "zp", zp, 2);
        if (zpx)  add(m, "zpx", zpx, 2);
        if (abs)  add(m, "abs", abs, 3);
        if (absx) add(m, "absx", absx, 3);
    };

    addShift("ASL", 0x0A, 0x06, 0x16, 0x0E, 0x1E);
    addShift("LSR", 0x4A, 0x46, 0x56, 0x4E, 0x5E);
    addShift("ROL", 0x2A, 0x26, 0x36, 0x2E, 0x3E);
    addShift("ROR", 0x6A, 0x66, 0x76, 0x6E, 0x7E);

    add("BIT", "zp", 0x24, 2); add("BIT", "abs", 0x2C, 3);
    
    add("CPX", "imm", 0xE0, 2); add("CPX", "zp", 0xE4, 2); add("CPX", "abs", 0xEC, 3);
    add("CPY", "imm", 0xC0, 2); add("CPY", "zp", 0xC4, 2); add("CPY", "abs", 0xCC, 3);
    
    add("DEC", "zp", 0xC6, 2); add("DEC", "zpx", 0xD6, 2); add("DEC", "abs", 0xCE, 3); add("DEC", "absx", 0xDE, 3);
    add("INC", "zp", 0xE6, 2); add("INC", "zpx", 0xF6, 2); add("INC", "abs", 0xEE, 3); add("INC", "absx", 0xFE, 3);

    add("LDX", "imm", 0xA2, 2); add("LDX", "zp", 0xA6, 2); add("LDX", "zpy", 0xB6, 2); add("LDX", "abs", 0xAE, 3); add("LDX", "absy", 0xBE, 3);
    add("LDY", "imm", 0xA0, 2); add("LDY", "zp", 0xA4, 2); add("LDY", "zpx", 0xB4, 2); add("LDY", "abs", 0xAC, 3); add("LDY", "absx", 0xBC, 3);
    add("STX", "zp", 0x86, 2); add("STX", "zpy", 0x96, 2); add("STX", "abs", 0x8E, 3);
    add("STY", "zp", 0x84, 2); add("STY", "zpx", 0x94, 2); add("STY", "abs", 0x8C, 3);

    add("JMP", "abs", 0x4C, 3); add("JMP", "ind", 0x6C, 3);
    add("JSR", "abs", 0x20, 3);

    add("BCC", "rel", 0x90, 2); add("BCS", "rel", 0xB0, 2); add("BEQ", "rel", 0xF0, 2);
    add("BMI", "rel", 0x30, 2); add("BNE", "rel", 0xD0, 2); add("BPL", "rel", 0x10, 2);
    add("BVC", "rel", 0x50, 2); add("BVS", "rel", 0x70, 2);

    add("BRK", "imp", 0x00, 1);
    add("CLC", "imp", 0x18, 1); add("CLD", "imp", 0xD8, 1); add("CLI", "imp", 0x58, 1); add("CLV", "imp", 0xB8, 1);
    add("SEC", "imp", 0x38, 1); add("SED", "imp", 0xF8, 1); add("SEI", "imp", 0x78, 1);
    add("DEX", "imp", 0xCA, 1); add("DEY", "imp", 0x88, 1); add("INX", "imp", 0xE8, 1); add("INY", "imp", 0xC8, 1);
    add("TAX", "imp", 0xAA, 1); add("TAY", "imp", 0xA8, 1); add("TXA", "imp", 0x8A, 1); add("TYA", "imp", 0x98, 1);
    add("TSX", "imp", 0xBA, 1); add("TXS", "imp", 0x9A, 1);
    add("PHA", "imp", 0x48, 1); add("PHP", "imp", 0x08, 1); add("PLA", "imp", 0x68, 1); add("PLP", "imp", 0x28, 1);
    add("RTI", "imp", 0x40, 1); add("RTS", "imp", 0x60, 1);
    add("NOP", "imp", 0xEA, 1);

    if (m_cmos) return;     // the NMOS illegal opcodes are NOPs on the 65C02

    // Illegal Opcodes (subset)
    add("SLO", "zp", 0x07, 2); add("SLO", "zpx", 0x17, 2); add("SLO", "abs", 0x0F, 3); add("SLO", "absx", 0x1F, 3); add("SLO", "absy", 0x1B, 3); add("SLO", "zpxind", 0x03, 2); add("SLO", "indzpy", 0x13, 2);
    add("RLA", "zp", 0x27, 2); add("RLA", "zpx", 0x37, 2); add("RLA", "abs", 0x2F, 3); add("RLA", "absx", 0x3F, 3); add("RLA", "absy", 0x3B, 3); add("RLA", "zpxind", 0x23, 2); add("RLA", "indzpy", 0x33, 2);
    add("SRE", "zp", 0x47, 2); add("SRE", "zpx", 0x57, 2); add("SRE", "abs", 0x4F, 3); add("SRE", "absx", 0x5F, 3); add("SRE", "absy", 0x5B, 3); add("SRE", "zpxind", 0x43, 2); add("SRE", "indzpy", 0x53, 2);
    add("RRA", "zp", 0x67, 2); add("RRA", "zpx", 0x77, 2); add("RRA", "abs", 0x6F, 3); add("RRA", "absx", 0x7F, 3); add("RRA", "absy", 0x7B, 3); add("RRA", "zpxind", 0x63, 2); add("RRA", "indzpy", 0x73, 2);
    add("SAX", "zp", 0x87, 2); add("SAX", "zpy", 0x97, 2); add("SAX", "abs", 0x8F, 3); add("SAX", "zpxind", 0x83, 2);
    add("LAX", "zp", 0xA7, 2); add("LAX", "zpy", 0xB7, 2); add("LAX", "abs", 0xAF, 3); add("LAX", "absy", 0xBF, 3); add("LAX", "zpxind", 0xA3, 2); add("LAX", "indzpy", 0xB3, 2);
    add("DCP", "zp", 0xC7, 2); add("DCP", "zpx", 0xD7, 2); add("DCP", "abs", 0xCF, 3); add("DCP", "absx", 0xDF, 3); add("DCP", "absy", 0xDB, 3); add("DCP", "zpxind", 0xC3, 2); add("DCP", "indzpy", 0xD3, 2);
    add("ISC", "zp", 0xE7, 2); add("ISC", "zpx", 0xF7, 2); add("ISC", "abs", 0xEF, 3); add("ISC", "absx", 0xFF, 3); add("ISC", "absy", 0xFB, 3); add("ISC", "zpxind", 0xE3, 2); add("ISC", "indzpy", 0xF3, 2);
}

void Assembler6502::initCmos() {
    // (zp) indirect without index
    const std::pair<const char*, uint8_t> zpind[] = {
        {"ORA", 0x12}, {"AND", 0x32}, {"EOR", 0x52}, {"ADC", 0x72},
        {"STA", 0x92}, {"LDA", 0xB2}, {"CMP", 0xD2}, {"SBC", 0xF2},
    };
    for (const auto& [m, op] : zpind) add(m, "zpind", op, 2);

    add("BIT", "imm", 0x89, 2); add("BIT", "zpx", 0x34, 2); add("BIT", "absx", 0x3C, 3);
    add("INC", "acc", 0x1A, 1); add("DEC", "acc", 0x3A, 1);
    add("INA", "imp", 0x1A, 1); add("DEA", "imp", 0x3A, 1);
    add("JMP", "absxind", 0x7C, 3);
    add("BRA", "rel", 0x80, 2);
    add("STZ", "zp", 0x64, 2); add("STZ", "zpx", 0x74, 2); add("STZ", "abs", 0x9C, 3); add("STZ", "absx", 0x9E, 3);
    add("TSB", "zp", 0x04, 2); add("TSB", "abs", 0x0C, 3);
    add("TRB", "zp", 0x14, 2); add("TRB", "abs", 0x1C, 3);
    add("PHY", "imp", 0x5A, 1); add("PLY", "imp", 0x7A, 1);
    add("PHX", "imp", 0xDA, 1); add("PLX", "imp", 0xFA, 1);
    add("WAI", "imp", 0xCB, 1); add("STP", "imp", 0xDB, 1);
    // Rockwell/WDC bit instructions: RMBn/SMBn zp, BBRn/BBSn zp,target
    for (int n = 0; n < 8; n++) {
        std::string d(1, (char)('0' + n));
        add("RMB" + d, "zp", (uint8_t)(0x07 + n * 0x10), 2);
        add("SMB" + d, "zp", (uint8_t)(0x87 + n * 0x10), 2);
        add("BBR" + d, "zprel", (uint8_t)(0x0F + n * 0x10), 3);
        add("BBS" + d, "zprel", (uint8_t)(0x8F + n * 0x10), 3);
    }
}

AssemblerResult Assembler6502::assemble(const std::string& sourcePath, const std::string& outputPath) {
    (void)sourcePath; (void)outputPath;
    return {false, "File assembly not supported by native mini-assembler", "", "", "", 0, 0};
}

static uint32_t parseValue(const std::string& s) {
    if (s.empty()) return 0;
    if (s[0] == '$') return std::stoul(s.substr(1), nullptr, 16);
    return std::stoul(s);
}

int Assembler6502::assembleLine(const std::string& line, uint8_t* buf, int bufsz, uint32_t currentAddr) {
    if (line.empty()) return 0;

    std::string trimmed = line;
    trimmed.erase(0, trimmed.find_first_not_of(" \t"));
    trimmed.erase(trimmed.find_last_not_of(" \t") + 1);
    if (trimmed.empty()) return 0;

    std::stringstream ss(trimmed);
    std::string mnem;
    ss >> mnem;
    std::transform(mnem.begin(), mnem.end(), mnem.begin(), ::toupper);

    if (m_mnemonics.count(mnem) == 0) return -1;

    std::string oper;
    std::getline(ss, oper);
    oper.erase(0, oper.find_first_not_of(" \t"));
    oper.erase(oper.find_last_not_of(" \t") + 1);
    std::transform(oper.begin(), oper.end(), oper.begin(), ::toupper);

    std::string mode = "imp";
    uint32_t val = 0;

    // Regular expressions for addressing modes
    static std::regex re_imm("^#(\\$[0-9A-F]+|[0-9]+)$");
    static std::regex re_abs("^(\\$[0-9A-F]+|[0-9]+)$");
    static std::regex re_absx("^(\\$[0-9A-F]+|[0-9]+),X$");
    static std::regex re_absy("^(\\$[0-9A-F]+|[0-9]+),Y$");
    static std::regex re_ind("^\\((\\$[0-9A-F]+|[0-9]+)\\)$");
    static std::regex re_indx("^\\((\\$[0-9A-F]+|[0-9]+),X\\)$");
    static std::regex re_indy("^\\((\\$[0-9A-F]+|[0-9]+)\\),Y$");
    static std::regex re_zprel("^(\\$[0-9A-F]+|[0-9]+),(\\$[0-9A-F]+|[0-9]+)$");
    uint32_t target = 0;

    std::smatch match;

    if (oper.empty()) {
        mode = "imp";
    } else if (oper == "A") {
        mode = "acc";
    } else if (std::regex_match(oper, match, re_imm)) {
        mode = "imm";
        val = parseValue(match[1]);
    } else if (m_mnemonics[mnem].count("zprel") && std::regex_match(oper, match, re_zprel)) {
        mode = "zprel";
        val = parseValue(match[1]);
        target = parseValue(match[2]);
    } else if (std::regex_match(oper, match, re_indx)) {
        val = parseValue(match[1]);
        mode = m_mnemonics[mnem].count("absxind") ? "absxind" : "zpxind";
    } else if (std::regex_match(oper, match, re_indy)) {
        mode = "indzpy";
        val = parseValue(match[1]);
    } else if (std::regex_match(oper, match, re_ind)) {
        val = parseValue(match[1]);
        mode = m_mnemonics[mnem].count("zpind") ? "zpind" : "ind";
    } else if (std::regex_match(oper, match, re_absx)) {
        val = parseValue(match[1]);
        mode = (val <= 0xFF && m_mnemonics[mnem].count("zpx")) ? "zpx" : "absx";
    } else if (std::regex_match(oper, match, re_absy)) {
        val = parseValue(match[1]);
        mode = (val <= 0xFF && m_mnemonics[mnem].count("zpy")) ? "zpy" : "absy";
    } else if (std::regex_match(oper, match, re_abs)) {
        val = parseValue(match[1]);
        if (m_mnemonics[mnem].count("rel")) {
            mode = "rel";
        } else {
            mode = (val <= 0xFF && m_mnemonics[mnem].count("zp")) ? "zp" : "abs";
        }
    } else {
        return -1;
    }

    // Special case for Accumulator if 'A' was omitted but required
    if (mode == "imp" && m_mnemonics[mnem].count("acc") && !m_mnemonics[mnem].count("imp")) {
        mode = "acc";
    }

    if (m_mnemonics[mnem].count(mode) == 0) {
        // Fallback checks
        if (mode == "zp" && m_mnemonics[mnem].count("abs")) mode = "abs";
        else if (mode == "zpx" && m_mnemonics[mnem].count("absx")) mode = "absx";
        else if (mode == "zpy" && m_mnemonics[mnem].count("absy")) mode = "absy";
        else return -1;
    }

    const auto& info = m_mnemonics[mnem][mode];
    if (info.size > bufsz) return -1;

    buf[0] = info.opcode;
    if (mode == "zprel") {
        if (val > 0xFF) return -1;
        int32_t offset = (int32_t)target - (int32_t)(currentAddr + 3);
        if (offset < -128 || offset > 127) return -1;
        buf[1] = (uint8_t)val;
        buf[2] = (uint8_t)(offset & 0xFF);
    } else if (mode == "zpind" && val > 0xFF) {
        return -1;
    } else if (mode == "rel") {
        int32_t offset = (int32_t)val - (int32_t)(currentAddr + 2);
        if (offset < -128 || offset > 127) return -1;
        buf[1] = (uint8_t)(offset & 0xFF);
    } else {
        if (info.size > 1) buf[1] = val & 0xFF;
        if (info.size > 2) buf[2] = (val >> 8) & 0xFF;
    }

    return info.size;
}
