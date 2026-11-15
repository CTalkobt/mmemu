// A bar code wand and the Code 39 / EAN-13 / UPC-A symbologies (see
// barcode_wand.h). The symbol tables are the published standards' (Code 39:
// ANSI/AIM BC1; EAN-13: ISO/IEC 15420). mmsim's own code.

#include "barcode_wand.h"
#include <cctype>
#include <cstring>

namespace {

// Code 39: nine elements, bar space bar space ... bar; '1' = wide
struct C39 { char c; const char* w; };
const C39 CODE39[] = {
    {'0', "000110100"}, {'1', "100100001"}, {'2', "001100001"}, {'3', "101100000"}, {'4', "000110001"},
    {'5', "100110000"}, {'6', "001110000"}, {'7', "000100101"}, {'8', "100100100"}, {'9', "001100100"},
    {'A', "100001001"}, {'B', "001001001"}, {'C', "101001000"}, {'D', "000011001"}, {'E', "100011000"},
    {'F', "001011000"}, {'G', "000001101"}, {'H', "100001100"}, {'I', "001001100"}, {'J', "000011100"},
    {'K', "100000011"}, {'L', "001000011"}, {'M', "101000010"}, {'N', "000010011"}, {'O', "100010010"},
    {'P', "001010010"}, {'Q', "000000111"}, {'R', "100000110"}, {'S', "001000110"}, {'T', "000010110"},
    {'U', "110000001"}, {'V', "011000001"}, {'W', "111000000"}, {'X', "010010001"}, {'Y', "110010000"},
    {'Z', "011010000"}, {'-', "010000101"}, {'.', "110000100"}, {' ', "011000100"}, {'*', "010010100"},
    {'$', "010101000"}, {'/', "010100010"}, {'+', "010001010"}, {'%', "000101010"},
};
constexpr int WIDE = 3;

const char* c39(char c) {
    for (const auto& e : CODE39)
        if (e.c == c) return e.w;
    return nullptr;
}

// EAN: the left-hand odd (L) set; R is its complement, G R reversed
const char* const EAN_L[10] = {"0001101", "0011001", "0010011", "0111101", "0100011",
                               "0110001", "0101111", "0111011", "0110111", "0001011"};
// The first digit picks L or G for each of the six left digits
const char* const EAN_PARITY[10] = {"LLLLLL", "LLGLGG", "LLGGLG", "LLGGGL", "LGLLGG",
                                    "LGGLLG", "LGGGLL", "LGLGLG", "LGLGGL", "LGGLGL"};

void add(std::vector<bool>& m, const char* bits) {
    for (const char* p = bits; *p; ++p) m.push_back(*p == '1');
}

int eanCheck(const std::string& d12) {
    int sum = 0;
    for (int i = 0; i < 12; ++i) sum += (d12[i] - '0') * (i % 2 ? 3 : 1);
    return (10 - sum % 10) % 10;
}

}  // namespace

std::vector<bool> BarcodeWand::code39(const std::string& text) {
    std::vector<bool> m;
    const std::string full = "*" + text + "*";
    for (size_t i = 0; i < full.size(); ++i) {
        const char c = (char)std::toupper((unsigned char)full[i]);
        if (c == '*' && i != 0 && i != full.size() - 1) return {};
        const char* w = c39(c);
        if (!w) return {};
        if (i) m.push_back(false);                  // the inter-character gap
        for (int e = 0; e < 9; ++e) {
            const bool bar = e % 2 == 0;
            for (int k = 0; k < (w[e] == '1' ? WIDE : 1); ++k) m.push_back(bar);
        }
    }
    return m;
}

std::vector<bool> BarcodeWand::ean13(const std::string& digits) {
    for (char c : digits)
        if (!std::isdigit((unsigned char)c)) return {};
    std::string d = digits;
    if (d.size() == 12) d += (char)('0' + eanCheck(d));
    else if (d.size() != 13 || d[12] - '0' != eanCheck(d)) return {};
    std::vector<bool> m;
    add(m, "101");
    const char* par = EAN_PARITY[d[0] - '0'];
    for (int i = 0; i < 6; ++i) {
        const char* l = EAN_L[d[1 + i] - '0'];
        char code[8] = {};
        for (int k = 0; k < 7; ++k)
            code[k] = par[i] == 'L' ? l[k] : (l[6 - k] == '1' ? '0' : '1');   // G: R reversed
        add(m, code);
    }
    add(m, "01010");
    for (int i = 0; i < 6; ++i) {
        const char* l = EAN_L[d[7 + i] - '0'];
        char code[8] = {};
        for (int k = 0; k < 7; ++k) code[k] = l[k] == '1' ? '0' : '1';       // R: L complemented
        add(m, code);
    }
    add(m, "101");
    return m;
}

std::vector<bool> BarcodeWand::upca(const std::string& digits) {
    if (digits.size() != 11 && digits.size() != 12) return {};
    return ean13("0" + digits);
}

std::vector<bool> BarcodeWand::fromBits(const std::string& bits) {
    std::vector<bool> m;
    for (char c : bits) {
        if (c == '1' || c == '|') m.push_back(true);
        else if (c == '0' || c == '.' || c == ' ' || c == '_') m.push_back(false);
        else return {};
    }
    return m;
}

std::vector<bool> BarcodeWand::parse(const std::string& spec) {
    const auto colon = spec.find(':');
    if (colon == std::string::npos) return {};
    std::string kind = spec.substr(0, colon);
    for (auto& c : kind) c = (char)std::tolower((unsigned char)c);
    const std::string arg = spec.substr(colon + 1);
    if (kind == "code39") return code39(arg);
    if (kind == "ean13" || kind == "ean") return ean13(arg);
    if (kind == "upca" || kind == "upc") return upca(arg);
    if (kind == "bits") return fromBits(arg);
    return {};
}

void BarcodeWand::swipe(const std::vector<bool>& modules) {
    m_modules.assign(QUIET_MODULES, false);
    m_modules.insert(m_modules.end(), modules.begin(), modules.end());
    m_modules.insert(m_modules.end(), QUIET_MODULES, false);
    m_pos = 0;
    m_inModule = 0;
    ++m_swipes;
}

bool BarcodeWand::data() const {
    return !(scanning() && m_modules[m_pos]);
}

void BarcodeWand::tick(uint64_t clocks) {
    if (!scanning()) return;
    m_inModule += (double)clocks / m_hz;
    while (scanning() && m_inModule >= m_moduleSec) {
        m_inModule -= m_moduleSec;
        ++m_pos;
    }
}
