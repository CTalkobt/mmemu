// The keyboard matrices of the Model 100's siblings: the NEC PC-8201A and
// the Olivetti M10 (the Kyotronic 85 has the Model 100's), after MAME's
// kyocera.cpp input ports (pc8201a, olivm10; Curt Coder, BSD-3-Clause) and
// VirtualT's io.c update_keys (BSD). mmsim's own code.

#include "m100_system.h"
#include <cctype>
#include <map>
#include <string>

namespace m100 {

namespace {

constexpr int S = KEY_SHIFTED;
constexpr int K(int col, int bit) { return col << 3 | bit; }

/** Where `c` is in a layout: `plain[col][bit]` typed alone, `shifted` with SHIFT ('\0', ' ': nothing). */
int lookChar(const char* const (&plain)[9], const char* const (&shifted)[9], char c) {
    if (c == '\0' || c == ' ') return -1;
    for (int col = 0; col < 9; ++col)
        for (int bit = 0; bit < 8; ++bit) {
            if (plain[col][bit] == c) return K(col, bit);
            if (shifted[col][bit] == c) return K(col, bit) | S;
        }
    return -1;
}

// ---------------------------------------------------------------------------
// NEC PC-8201A (MAME pc8201a):
//   col 0  Z X C V B N M L          col 5  9 0 ; : - [ SPACE PASTE/INS
//   col 1  A S D F G H J K          col 6  BKSP UP DOWN LEFT RIGHT TAB ESC ENTER
//   col 2  Q W E R T Y U I          col 7  f.1-f.5, -, -, STOP
//   col 3  O P @ \ , . / ]          col 8  SHIFT CTRL GRPH - CAPS
//   col 4  1 2 3 4 5 6 7 8
// Shifted: 1-9 ! " # $ % & ' ( ), 0 _, ; +, : *, - =, [ {, @ ^, \ |, ] }.
// ---------------------------------------------------------------------------
const char* const PC_PLAIN[9] = {"zxcvbnml", "asdfghjk", "qwertyui", "op@\\,./]", "12345678",
                                 "90;:-[  ", "        ", "        ", "        "};
const char* const PC_SHIFT[9] = {"ZXCVBNML", "ASDFGHJK", "QWERTYUI", "OP^|<>?}", "!\"#$%&'(",
                                 ")_+*={  ", "        ", "        ", "        "};

int pcChar(char c) {
    switch (c) {
    case ' ': return K(5, 6);
    case '\b': return K(6, 0);
    case '\t': return K(6, 5);
    case 0x1B: return K(6, 6);
    case '\n': case '\r': return K(6, 7);
    default: return lookChar(PC_PLAIN, PC_SHIFT, c);
    }
}

int pcKey(const std::string& n) {
    static const std::map<std::string, int> keys = {
        // By the PC key's place, as MAME's PORT_CODEs: [ is @, ] is \, ' is :, right Alt [
        {"bracket_left", K(3, 2)}, {"lbracket", K(3, 2)}, {"bracket_right", K(3, 3)}, {"rbracket", K(3, 3)},
        {"backslash", K(3, 7)}, {"equal", K(5, 5)}, {"equals", K(5, 5)}, {"kpeq", K(5, 5)},
        {"right_alt", K(5, 5)}, {"altgr", K(5, 5)},
        {"semicolon", K(5, 2)}, {"squote", K(5, 3)}, {"quote", K(5, 3)}, {"apostrophe", K(5, 3)},
        {"comma", K(3, 4)}, {"period", K(3, 5)}, {"slash", K(3, 6)}, {"minus", K(5, 4)},
        {"space", K(5, 6)}, {"insert", K(5, 7)}, {"f9", K(5, 7)}, {"paste", K(5, 7)}, {"ins", K(5, 7)},
        {"delete", K(6, 0)}, {"backspace", K(6, 0)}, {"bksp", K(6, 0)}, {"up", K(6, 1)}, {"down", K(6, 2)},
        {"left", K(6, 3)}, {"right", K(6, 4)}, {"tab", K(6, 5)}, {"run_stop", K(6, 6)}, {"escape", K(6, 6)},
        {"esc", K(6, 6)}, {"return", K(6, 7)}, {"enter", K(6, 7)}, {"kp_enter", K(6, 7)},
        {"f8", K(7, 7)}, {"f12", K(7, 7)}, {"stop", K(7, 7)}, {"pause", K(7, 7)}, {"break", K(7, 7)},
        {"shift", K(8, 0)}, {"left_shift", K(8, 0)}, {"right_shift", K(8, 0)}, {"lshift", K(8, 0)},
        {"rshift", K(8, 0)}, {"control", K(8, 1)}, {"ctrl", K(8, 1)}, {"left_ctrl", K(8, 1)},
        {"right_ctrl", K(8, 1)}, {"alt", K(8, 2)}, {"left_alt", K(8, 2)}, {"graph", K(8, 2)}, {"grph", K(8, 2)},
        {"caps_lock", K(8, 4)}, {"caps", K(8, 4)},
        // The PC's shifted symbols as the GUI names them: the PC-8201A's own where it has them
        {"exclaim", pcChar('!')}, {"at", pcChar('@')}, {"hash", pcChar('#')}, {"dollar", pcChar('$')},
        {"percent", pcChar('%')}, {"caret", pcChar('^')}, {"ampersand", pcChar('&')}, {"asterisk", pcChar('*')},
        {"lparen", pcChar('(')}, {"rparen", pcChar(')')}, {"underscore", pcChar('_')}, {"plus", pcChar('+')},
        {"dquote", pcChar('"')}, {"colon", pcChar(':')}, {"less", pcChar('<')}, {"greater", pcChar('>')},
        {"question", pcChar('?')},
    };
    if (n.size() == 1 && std::isalnum((unsigned char)n[0])) return pcChar(n[0]);
    if (n.size() == 2 && n[0] == 'f' && n[1] >= '1' && n[1] <= '5') return K(7, n[1] - '1');
    auto it = keys.find(n);
    return it == keys.end() ? -1 : it->second;
}

// ---------------------------------------------------------------------------
// Olivetti M10 (MAME olivm10, VirtualT's MODEL_M10). The keys by their
// place on a PC keyboard (MAME's PORT_CODEs):
//   col 0  Q W ` 1 2 3 4 5          col 5  E R T U I O P SPACE
//   col 1  Y 6 7 8 9 0 - =          col 6  LEFT RIGHT UP DOWN BKSP TAB ENTER PASTE
//   col 2  ] [ ; ' / . , M          col 7  F1-F8
//   col 3  A \ Z X C V B N          col 8  LABEL PRINT SHIFT CTRL GRAPH NUM CAPS BREAK
//   col 4  S D F G H J K L
// and what the M10's ROM types with them (an AZERTY layout: the digits
// are shifted). The keys whose legends are national letters type the
// ASCII codes that stand for them (found from the ROM, key by key): plain
// # { } \ @ | for the 3, 4, 7, 9, 0 and ' places, shifted [ ~ ] for the
// -, [ and / places.
// ---------------------------------------------------------------------------
const char* const M10_PLAIN[9] = {"az$#{\"'(", "y-}_\\@)=", "`^m|!:;,", "q<wxcvbn", "sdfghjkl",
                                  "ertuiop ", "        ", "        ", "        "};
const char* const M10_SHIFT[9] = {"AZ*12345", "Y67890[+", "&~M%]/.?", "Q>WXCVBN", "SDFGHJKL",
                                  "ERTUIOP ", "        ", "        ", "        "};

int m10Char(char c) {
    switch (c) {
    case ' ': return K(5, 7);
    case '\b': return K(6, 4);
    case '\t': return K(6, 5);
    case '\n': case '\r': return K(6, 6);
    default: return lookChar(M10_PLAIN, M10_SHIFT, c);
    }
}

int m10Key(const std::string& n) {
    // The letter and digit keys by their PC place ('.': another key)
    static const char PLACE[6][9] = {"qw.12345", "y67890..", ".......m", "a.zxcvbn", "sdfghjkl", "ertuiop."};
    static const std::map<std::string, int> keys = {
        {"grave", K(0, 2)}, {"backquote", K(0, 2)}, {"tilde", K(0, 2)}, {"minus", K(1, 6)},
        {"equal", K(1, 7)}, {"equals", K(1, 7)}, {"kpeq", K(1, 7)},
        {"bracket_right", K(2, 0)}, {"rbracket", K(2, 0)}, {"bracket_left", K(2, 1)}, {"lbracket", K(2, 1)},
        {"semicolon", K(2, 2)}, {"squote", K(2, 3)}, {"quote", K(2, 3)}, {"apostrophe", K(2, 3)},
        {"slash", K(2, 4)}, {"period", K(2, 5)}, {"comma", K(2, 6)},
        {"backslash", K(3, 1)}, {"less", K(3, 1)}, {"space", K(5, 7)},
        {"left", K(6, 0)}, {"right", K(6, 1)}, {"up", K(6, 2)}, {"down", K(6, 3)},
        {"delete", K(6, 4)}, {"backspace", K(6, 4)}, {"bksp", K(6, 4)}, {"tab", K(6, 5)},
        {"return", K(6, 6)}, {"enter", K(6, 6)}, {"kp_enter", K(6, 6)},
        {"f9", K(6, 7)}, {"paste", K(6, 7)}, {"f10", K(8, 0)}, {"label", K(8, 0)},
        {"f11", K(8, 1)}, {"print", K(8, 1)},
        {"shift", K(8, 2)}, {"left_shift", K(8, 2)}, {"right_shift", K(8, 2)}, {"lshift", K(8, 2)},
        {"rshift", K(8, 2)}, {"control", K(8, 3)}, {"ctrl", K(8, 3)}, {"left_ctrl", K(8, 3)},
        {"right_ctrl", K(8, 3)}, {"alt", K(8, 4)}, {"left_alt", K(8, 4)}, {"graph", K(8, 4)},
        {"right_alt", K(8, 5)}, {"altgr", K(8, 5)}, {"home", K(8, 5)}, {"num", K(8, 5)},
        {"caps_lock", K(8, 6)}, {"caps", K(8, 6)}, {"f12", K(8, 7)}, {"pause", K(8, 7)}, {"break", K(8, 7)},
        // (the M10 has no ESC key)
    };
    if (n.size() == 1 && std::isalnum((unsigned char)n[0])) {
        for (int col = 0; col < 6; ++col)
            for (int bit = 0; bit < 8; ++bit)
                if (PLACE[col][bit] == n[0]) return K(col, bit);
        return -1;
    }
    if (n.size() == 2 && n[0] == 'f' && n[1] >= '1' && n[1] <= '8') return K(7, n[1] - '1');
    auto it = keys.find(n);
    return it == keys.end() ? -1 : it->second;
}

const KeyLayout PC8201_KEYS = {K(8, 0), K(8, 4), pcChar, pcKey};
const KeyLayout M10_KEYS = {K(8, 2), K(8, 6), m10Char, m10Key};

}  // namespace

const KeyLayout* keyLayout(Model m) {
    switch (m) {
    case Model::PC8201: return &PC8201_KEYS;
    case Model::M10: return &M10_KEYS;
    default: return nullptr;            // the Model 100's (also the 102, 200 and Kyotronic 85)
    }
}

}  // namespace m100
