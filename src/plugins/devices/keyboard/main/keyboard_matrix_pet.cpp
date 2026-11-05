#include "keyboard_matrix_pet.h"
#include "include/mmemu_plugin_api.h"
#include <algorithm>
#include <cstring>
#include <cstdio>

PetKeyboardMatrix::PetKeyboardMatrix(Layout layout) : m_layout(layout) {
    std::memset(m_matrix, 0xFF, sizeof(m_matrix));
    initKeyMap();
}

void PetKeyboardMatrix::initKeyMap() {
    m_keyMap.clear();
    m_shifted.clear();

    if (m_layout == Layout::GRAPHICS) {
        // PET 2001N Graphics keyboard matrix derived from BASIC 1 Editor ROM decode table.
        // Port A bits 0-3 select rows 0-9 via 74145 decoder.
        // Port B bit 7 = column 0 (first table entry), bit 0 = column 7 (last entry).
        // ROM table stored rows 9..0 (highest first); decode confirmed against ROM bytes.

        // Row 0 (Port A = 0xF0): cursor-right, HOME, ←char, (, &, %, #, !
        m_keyMap["right"]         = {0, 0x80};  // cursor right (0x1D)
        m_keyMap["home"]          = {0, 0x40};  // HOME/CLR   (0x13)
        m_keyMap["leftarrow"]     = {0, 0x20};  // ← graphical char (0x5F)
        m_keyMap["lparen"]        = {0, 0x10};  // (
        m_keyMap["ampersand"]     = {0, 0x08};  // &
        m_keyMap["percent"]       = {0, 0x04};  // %
        m_keyMap["hash"]          = {0, 0x02};  // #
        m_keyMap["exclaim"]       = {0, 0x01};  // !

        // Row 1 (Port A = 0xF1): DEL, cursor-down, (no key), ), £char, ', $, "
        m_keyMap["delete"]        = {1, 0x80};  // DEL/backspace (0x14)
        m_keyMap["down"]          = {1, 0x40};  // cursor down   (0x11)
        // {1, 0x20} = no key (0xFF)
        m_keyMap["rparen"]        = {1, 0x10};  // )
        // {1, 0x08} = £ graphical char (0x5C) — no PC equivalent
        m_keyMap["squote"]        = {1, 0x04};  // ' (single quote)
        m_keyMap["dollar"]        = {1, 0x02};  // $
        m_keyMap["dquote"]        = {1, 0x01};  // "

        // Row 2 (Port A = 0xF2): 9, 7, ^uparrow, O, U, T, E, Q
        m_keyMap["9"]             = {2, 0x80};
        m_keyMap["7"]             = {2, 0x40};
        m_keyMap["uparrow"]       = {2, 0x20};  // ↑ character key (0x5E)
        m_keyMap["o"]             = {2, 0x10};
        m_keyMap["u"]             = {2, 0x08};
        m_keyMap["t"]             = {2, 0x04};
        m_keyMap["e"]             = {2, 0x02};
        m_keyMap["q"]             = {2, 0x01};

        // Row 3 (Port A = 0xF3): /, 8, (no key), P, I, Y, R, W
        m_keyMap["slash"]         = {3, 0x80};
        m_keyMap["8"]             = {3, 0x40};
        // {3, 0x20} = no key
        m_keyMap["p"]             = {3, 0x10};
        m_keyMap["i"]             = {3, 0x08};
        m_keyMap["y"]             = {3, 0x04};
        m_keyMap["r"]             = {3, 0x02};
        m_keyMap["w"]             = {3, 0x01};

        // Row 4 (Port A = 0xF4): 6, 4, (no key), L, J, G, D, A
        m_keyMap["6"]             = {4, 0x80};
        m_keyMap["4"]             = {4, 0x40};
        // {4, 0x20} = no key
        m_keyMap["l"]             = {4, 0x10};
        m_keyMap["j"]             = {4, 0x08};
        m_keyMap["g"]             = {4, 0x04};
        m_keyMap["d"]             = {4, 0x02};
        m_keyMap["a"]             = {4, 0x01};

        // Row 5 (Port A = 0xF5): *, 5, (no key), :, K, H, F, S
        m_keyMap["asterisk"]      = {5, 0x80};  // * (dedicated key)
        m_keyMap["5"]             = {5, 0x40};
        // {5, 0x20} = no key
        m_keyMap["colon"]         = {5, 0x10};  // :
        m_keyMap["k"]             = {5, 0x08};
        m_keyMap["h"]             = {5, 0x04};
        m_keyMap["f"]             = {5, 0x02};
        m_keyMap["s"]             = {5, 0x01};

        // Row 6 (Port A = 0xF6): 3, 1, RETURN, ;, M, B, C, Z
        m_keyMap["3"]             = {6, 0x80};
        m_keyMap["1"]             = {6, 0x40};
        m_keyMap["return"]        = {6, 0x20};
        m_keyMap["semicolon"]     = {6, 0x10};  // ;
        m_keyMap["m"]             = {6, 0x08};
        m_keyMap["b"]             = {6, 0x04};
        m_keyMap["c"]             = {6, 0x02};
        m_keyMap["z"]             = {6, 0x01};

        // Row 7 (Port A = 0xF7): +, 2, (no key), ?, comma, N, V, X
        m_keyMap["plus"]          = {7, 0x80};  // + (dedicated key)
        m_keyMap["2"]             = {7, 0x40};
        // {7, 0x20} = no key
        m_keyMap["question"]      = {7, 0x10};  // ? (dedicated key)
        m_keyMap["comma"]         = {7, 0x08};
        m_keyMap["n"]             = {7, 0x04};
        m_keyMap["v"]             = {7, 0x02};
        m_keyMap["x"]             = {7, 0x01};

        // Row 8 (Port A = 0xF8): -, 0, LSHIFT, > (right cursor?), (no key), ], @, RSHIFT
        m_keyMap["minus"]         = {8, 0x80};
        m_keyMap["0"]             = {8, 0x40};
        m_keyMap["left_shift"]    = {8, 0x20};  // LEFT SHIFT (null char = modifier only)
        // {8, 0x10} = '>' (cursor right or graphical)
        // {8, 0x08} = no key
        m_keyMap["bracket_right"] = {8, 0x04};  // ]
        m_keyMap["at"]            = {8, 0x02};  // @
        m_keyMap["right_shift"]   = {8, 0x01};  // RIGHT SHIFT (null char = modifier only)

        // Row 9 (Port A = 0xF9): =, ., (no key), RUN/STOP, <, SPACE, [, RVS
        m_keyMap["kpeq"]          = {9, 0x80};  // = (keypad equals)
        m_keyMap["period"]        = {9, 0x40};
        // {9, 0x20} = no key
        m_keyMap["run_stop"]      = {9, 0x10};  // RUN/STOP
        m_keyMap["less"]          = {9, 0x08};  // < key
        m_keyMap["space"]         = {9, 0x04};
        m_keyMap["bracket_left"]  = {9, 0x02};  // [
        m_keyMap["rvs"]           = {9, 0x01};  // RVS ON

    } else {
        // PET Business (B) keyboard (8032) from the 8032 editor ROM's decode
        // table at $E6D1 (LDA $E6D0,X, X = 80..1), the same layout rule as the
        // graphics table: ROM row i is matrix row 9 - i, entry j is bit 7 - j.
        // Every position cross-checked against VICE's business keymap
        // (data/PET/gtk3_buuk_pos.vkm). Plain digits are the main row; the
        // keypad's are stored with bit 7 set. Codes $04 $05 $06 $0E $0F $15
        // $16 $19 have no PC key here.

        // Row 0: (05) (0E) CRSR-right KP8 - 8 5 2
        m_keyMap["right"]         = {0, 0x20};  // cursor right (0x1D)
        m_keyMap["kp8"]           = {0, 0x10};
        m_keyMap["minus"]         = {0, 0x08};  // - (shifted: =)
        m_keyMap["8"]             = {0, 0x04};
        m_keyMap["5"]             = {0, 0x02};
        m_keyMap["2"]             = {0, 0x01};
        // Row 1: KP9 (06) ^ KP7 0 7 4 1
        m_keyMap["kp9"]           = {1, 0x80};
        m_keyMap["uparrow"]       = {1, 0x20};  // the up-arrow key (0xDE)
        m_keyMap["kp7"]           = {1, 0x10};
        m_keyMap["0"]             = {1, 0x08};
        m_keyMap["7"]             = {1, 0x04};
        m_keyMap["4"]             = {1, 0x02};
        m_keyMap["1"]             = {1, 0x01};
        // Row 2: KP5 ; K ] H F S ESC
        m_keyMap["kp5"]           = {2, 0x80};
        m_keyMap["semicolon"]     = {2, 0x40};
        m_keyMap["k"]             = {2, 0x20};
        m_keyMap["bracket_right"] = {2, 0x10};
        m_keyMap["h"]             = {2, 0x08};
        m_keyMap["f"]             = {2, 0x04};
        m_keyMap["s"]             = {2, 0x02};
        m_keyMap["escape"]        = {2, 0x01};  // ESC (0x9B)
        // Row 3: KP6 @ L RETURN J G D A
        m_keyMap["kp6"]           = {3, 0x80};
        m_keyMap["at"]            = {3, 0x40};
        m_keyMap["l"]             = {3, 0x20};
        m_keyMap["return"]        = {3, 0x10};
        m_keyMap["j"]             = {3, 0x08};
        m_keyMap["g"]             = {3, 0x04};
        m_keyMap["d"]             = {3, 0x02};
        m_keyMap["a"]             = {3, 0x01};
        // Row 4: DEL P I \ Y R W TAB
        m_keyMap["delete"]        = {4, 0x80};  // INST/DEL (0x14)
        m_keyMap["p"]             = {4, 0x40};
        m_keyMap["i"]             = {4, 0x20};
        m_keyMap["backslash"]     = {4, 0x10};
        m_keyMap["y"]             = {4, 0x08};
        m_keyMap["r"]             = {4, 0x04};
        m_keyMap["w"]             = {4, 0x02};
        m_keyMap["tab"]           = {4, 0x01};
        // Row 5: KP4 [ O CRSR-down U T E Q
        m_keyMap["kp4"]           = {5, 0x80};
        m_keyMap["bracket_left"]  = {5, 0x40};
        m_keyMap["o"]             = {5, 0x20};
        m_keyMap["down"]          = {5, 0x10};  // cursor down (0x11)
        m_keyMap["u"]             = {5, 0x08};
        m_keyMap["t"]             = {5, 0x04};
        m_keyMap["e"]             = {5, 0x02};
        m_keyMap["q"]             = {5, 0x01};
        // Row 6: KP3 RSHIFT (19) KP. . B C LSHIFT
        m_keyMap["kp3"]           = {6, 0x80};
        m_keyMap["right_shift"]   = {6, 0x40};
        m_keyMap["kpperiod"]      = {6, 0x10};
        m_keyMap["period"]        = {6, 0x08};
        m_keyMap["b"]             = {6, 0x04};
        m_keyMap["c"]             = {6, 0x02};
        m_keyMap["left_shift"]    = {6, 0x01};
        // Row 7: KP2 REPEAT (0F) KP0 , N V Z
        m_keyMap["kp2"]           = {7, 0x80};
        m_keyMap["repeat"]        = {7, 0x40};
        m_keyMap["kp0"]           = {7, 0x10};
        m_keyMap["comma"]         = {7, 0x08};
        m_keyMap["n"]             = {7, 0x04};
        m_keyMap["v"]             = {7, 0x02};
        m_keyMap["z"]             = {7, 0x01};
        // Row 8: KP1 / (15) HOME M SPACE X RVS
        m_keyMap["kp1"]           = {8, 0x80};
        m_keyMap["slash"]         = {8, 0x40};
        m_keyMap["home"]          = {8, 0x10};  // HOME/CLR (0x13)
        m_keyMap["m"]             = {8, 0x08};
        m_keyMap["space"]         = {8, 0x04};
        m_keyMap["x"]             = {8, 0x02};
        m_keyMap["rvs"]           = {8, 0x01};  // RVS ON (0x12)
        // Row 9: (16) (04) : STOP 9 6 3 <-
        m_keyMap["colon"]         = {9, 0x20};  // : (shifted: *)
        m_keyMap["run_stop"]      = {9, 0x10};  // STOP (0x03)
        m_keyMap["9"]             = {9, 0x08};
        m_keyMap["6"]             = {9, 0x04};
        m_keyMap["3"]             = {9, 0x02};
        m_keyMap["leftarrow"]     = {9, 0x01};  // the left-arrow key (0xDF)

        // Characters typed with SHIFT on this keyboard
        m_shifted = {
            {"exclaim", "1"}, {"dquote", "2"}, {"hash", "3"}, {"dollar", "4"}, {"percent", "5"},
            {"ampersand", "6"}, {"squote", "7"}, {"lparen", "8"}, {"rparen", "9"},
            {"asterisk", "colon"}, {"equal", "minus"}, {"kpeq", "minus"}, {"plus", "semicolon"},
            {"less", "comma"}, {"greater", "period"}, {"question", "slash"},
        };
    }
}

void PetKeyboardMatrix::keyDown(const std::string& keyName) {
    if (m_logger && m_logNamed) {
        char buf[64]; snprintf(buf, sizeof(buf), "keyDown: %s", keyName.c_str());
        m_logNamed(m_logger, SIM_LOG_DEBUG, buf);
    }
    auto it = m_keyMap.find(keyName);
    if (it != m_keyMap.end()) {
        m_matrix[it->second.col] &= ~it->second.bit;
    }
}

void PetKeyboardMatrix::keyUp(const std::string& keyName) {
    if (m_logger && m_logNamed) {
        char buf[64]; snprintf(buf, sizeof(buf), "keyUp: %s", keyName.c_str());
        m_logNamed(m_logger, SIM_LOG_DEBUG, buf);
    }
    auto it = m_keyMap.find(keyName);
    if (it != m_keyMap.end()) {
        m_matrix[it->second.col] |= it->second.bit;
    }
}

uint8_t PetKeyboardMatrix::readPort() {
    uint8_t result = 0xFF;
    uint8_t rowSelect = m_selectedRow & 0x0F;
    
    // In PET, the 74145 decoder selects a single row (0-9).
    // If the value is 10-15, no row is selected.
    if (rowSelect < 10) {
        result &= m_matrix[rowSelect];
    }
    
    return result;
}

void PetKeyboardMatrix::writePort(uint8_t val) {
    m_selectedRow = val;
}

void PetKeyboardMatrix::setDdr(uint8_t ddr) {
    // DDR handled by PIA
}
