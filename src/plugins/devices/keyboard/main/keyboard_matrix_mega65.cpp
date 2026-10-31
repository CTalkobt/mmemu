#include "keyboard_matrix_mega65.h"
#include <algorithm>

KbdMega65::KbdMega65()
    : m_colPort(this), m_rowPort(this)
{
    clearKeys();

    // MEGA65 Key Mapping (8x8 Matrix)
    // Based on C65 / Mega65 specifications.
    // row 0: DEL, RETURN, LEFT, F7, F1, F3, F5, UP
    // row 1: 3, W, A, 4, Z, S, E, SHIFT_L
    // row 2: 5, R, D, 6, C, F, T, X
    // row 3: 7, Y, G, 8, B, H, U, V
    // row 4: 9, I, J, 0, M, K, O, N
    // row 5: +, P, L, -, ., :, @, ,
    // row 6: POUND, *, ;, HOME, SHIFT_R, =, ^, /
    // row 7: 1, ESC, CTRL, 2, SPACE, COMMODORE, Q, RUNSTOP

    // Format: {row, col}
    m_keyMap["DEL"]       = {0, 0}; m_keyMap["BACKSPACE"] = {0, 0};
    m_keyMap["RETURN"]    = {0, 1}; m_keyMap["ENTER"]     = {0, 1};
    m_keyMap["LEFT"]      = {0, 2}; m_keyMap["RIGHT"]     = {0, 2}; // C64 uses same for both with shift
    m_keyMap["F7"]        = {0, 3}; m_keyMap["F8"]        = {0, 3};
    m_keyMap["F1"]        = {0, 4}; m_keyMap["F2"]        = {0, 4};
    m_keyMap["F3"]        = {0, 5}; m_keyMap["F4"]        = {0, 5};
    m_keyMap["F5"]        = {0, 6}; m_keyMap["F6"]        = {0, 6};
    m_keyMap["UP"]        = {0, 7}; m_keyMap["DOWN"]      = {0, 7};

    m_keyMap["3"]         = {1, 0}; m_keyMap["W"]         = {1, 1};
    m_keyMap["A"]         = {1, 2}; m_keyMap["4"]         = {1, 3};
    m_keyMap["Z"]         = {1, 4}; m_keyMap["S"]         = {1, 5};
    m_keyMap["E"]         = {1, 6}; m_keyMap["SHIFT_L"]   = {1, 7}; m_keyMap["LSHIFT"] = {1, 7};

    m_keyMap["5"]         = {2, 0}; m_keyMap["R"]         = {2, 1};
    m_keyMap["D"]         = {2, 2}; m_keyMap["6"]         = {2, 3};
    m_keyMap["C"]         = {2, 4}; m_keyMap["F"]         = {2, 5};
    m_keyMap["T"]         = {2, 6}; m_keyMap["X"]         = {2, 7};

    m_keyMap["7"]         = {3, 0}; m_keyMap["Y"]         = {3, 1};
    m_keyMap["G"]         = {3, 2}; m_keyMap["8"]         = {3, 3};
    m_keyMap["B"]         = {3, 4}; m_keyMap["H"]         = {3, 5};
    m_keyMap["U"]         = {3, 6}; m_keyMap["V"]         = {3, 7};

    m_keyMap["9"]         = {4, 0}; m_keyMap["I"]         = {4, 1};
    m_keyMap["J"]         = {4, 2}; m_keyMap["0"]         = {4, 3};
    m_keyMap["M"]         = {4, 4}; m_keyMap["K"]         = {4, 5};
    m_keyMap["O"]         = {4, 6}; m_keyMap["N"]         = {4, 7};

    m_keyMap["+"]         = {5, 0}; m_keyMap["P"]         = {5, 1};
    m_keyMap["L"]         = {5, 2}; m_keyMap["-"]         = {5, 3};
    m_keyMap["."]         = {5, 4}; m_keyMap[":"]         = {5, 5};
    m_keyMap["@"]         = {5, 6}; m_keyMap[","]         = {5, 7};

    m_keyMap["POUND"]     = {6, 0}; m_keyMap["*"]         = {6, 1};
    m_keyMap[";"]         = {6, 2}; m_keyMap["HOME"]      = {6, 3}; m_keyMap["CLR"] = {6, 3};
    m_keyMap["SHIFT_R"]   = {6, 4}; m_keyMap["RSHIFT"]    = {6, 4};
    m_keyMap["="]         = {6, 5}; m_keyMap["^"]         = {6, 6};
    m_keyMap["/"]         = {6, 7};

    m_keyMap["1"]         = {7, 0}; m_keyMap["ESC"]       = {7, 1};
    m_keyMap["CTRL"]      = {7, 2}; m_keyMap["2"]         = {7, 3};
    m_keyMap["SPACE"]     = {7, 4}; m_keyMap["COMMODORE"] = {7, 5}; m_keyMap["MEGA"] = {7, 5};
    m_keyMap["Q"]         = {7, 6}; m_keyMap["RUNSTOP"]   = {7, 7};
}

KbdMega65::~KbdMega65() = default;

void KbdMega65::reset() {
    clearKeys();
}

void KbdMega65::tick(uint64_t cycles) {
    if (!m_typeQueue.empty()) {
        if (m_typeTimer > 0) {
            m_typeTimer--;
        } else {
            auto& ev = m_typeQueue.front();
            pressKeyByName(ev.keyName, ev.down);
            m_typeQueue.erase(m_typeQueue.begin());
            m_typeTimer = 20000; // Delay between key events
        }
    }
}

void KbdMega65::keyDown(int row, int col) {
    if (col >= 0 && col < 8 && row >= 0 && row < 8) {
        m_matrix[col] &= ~(uint8_t)(1 << row);
    }
    updateRowVal();
}

void KbdMega65::keyUp(int row, int col) {
    if (col >= 0 && col < 8 && row >= 0 && row < 8) {
        m_matrix[col] |= (uint8_t)(1 << row);
    }
    updateRowVal();
}

void KbdMega65::clearKeys() {
    std::memset(m_matrix, 0xFF, sizeof(m_matrix));
    updateRowVal();
}

bool KbdMega65::pressKeyByName(const std::string& keyName, bool down) {
    std::string name = keyName;
    std::transform(name.begin(), name.end(), name.begin(), ::toupper);

    if (name == "RESTORE") {
        if (down && m_sigRestore) m_sigRestore->pulse();
        return true;
    }

    if (name == "CAPS" || name == "CAPS_LOCK") {
        // MEGA65 CAPS LOCK is a physical toggle switch but usually emulated as a key.
        // We'll just toggle the LED for now if it's pressed.
        if (down && m_sigCapsLockLed) {
            static bool caps = false;
            caps = !caps;
            m_sigCapsLockLed->set(caps);
        }
        // Also map to matrix if it exists (C65 CAPS is not on matrix, it's a port bit)
        // But for compatibility with common software, sometimes it is mapped.
    }

    auto it = m_keyMap.find(name);
    if (it != m_keyMap.end()) {
        if (down) keyDown(it->second.first, it->second.second);
        else      keyUp  (it->second.first, it->second.second);
        return true;
    }

    // Handle shift-combos
    if (name == "!") return pressCombo({"LSHIFT", "1"}, down);
    if (name == "\"") return pressCombo({"LSHIFT", "2"}, down);
    if (name == "#") return pressCombo({"LSHIFT", "3"}, down);
    if (name == "$") return pressCombo({"LSHIFT", "4"}, down);
    if (name == "%") return pressCombo({"LSHIFT", "5"}, down);
    if (name == "&") return pressCombo({"LSHIFT", "6"}, down);
    if (name == "'") return pressCombo({"LSHIFT", "7"}, down);
    if (name == "(") return pressCombo({"LSHIFT", "8"}, down);
    if (name == ")") return pressCombo({"LSHIFT", "9"}, down);
    if (name == "?") return pressCombo({"LSHIFT", "/"}, down);
    if (name == "<") return pressCombo({"LSHIFT", ","}, down);
    if (name == ">") return pressCombo({"LSHIFT", "."}, down);
    if (name == "[") return pressCombo({"LSHIFT", ":"}, down);
    if (name == "]") return pressCombo({"LSHIFT", ";"}, down);
    if (name == "CLR") return pressCombo({"LSHIFT", "HOME"}, down);
    if (name == "DOWN") return pressCombo({"LSHIFT", "UP"}, down);
    if (name == "RIGHT") return pressCombo({"LSHIFT", "LEFT"}, down);

    return false;
}

bool KbdMega65::pressCombo(const std::vector<std::string>& keys, bool down) {
    bool ok = true;
    for (const auto& k : keys) ok &= pressKeyByName(k, down);
    return ok;
}

void KbdMega65::enqueueText(const std::string& text) {
    for (char c : text) {
        std::string s(1, c);
        if (c >= 'a' && c <= 'z') {
            s[0] = std::toupper(s[0]);
            m_typeQueue.push_back({s, true});
            m_typeQueue.push_back({s, false});
        } else if (c >= 'A' && c <= 'Z') {
            m_typeQueue.push_back({"LSHIFT", true});
            m_typeQueue.push_back({s, true});
            m_typeQueue.push_back({s, false});
            m_typeQueue.push_back({"LSHIFT", false});
        } else {
            // Handle symbols, space, etc.
            if (c == ' ') s = "SPACE";
            if (c == '\n') s = "RETURN";
            m_typeQueue.push_back({s, true});
            m_typeQueue.push_back({s, false});
        }
    }
}

IPortDevice* KbdMega65::getPort(int index) {
    return index == 0 ? (IPortDevice*)&m_colPort : (IPortDevice*)&m_rowPort;
}

void KbdMega65::updateRowVal() {
    uint8_t colSelect = m_colPort.m_val;
    uint8_t result = 0xFF;

    for (int i = 0; i < 8; ++i) {
        if (!(colSelect & (1 << i))) {
            result &= m_matrix[i];
        }
    }
    m_rowVal = result;
}

void KbdMega65::ColumnPort::writePort(uint8_t v) {
    m_val = v;
    m_parent->updateRowVal();
}

uint8_t KbdMega65::RowPort::readPort() {
    return m_parent->m_rowVal;
}
