#include "kbd_c64.h"
#include <algorithm>

KbdC64::KbdC64() : m_colPort(this), m_rowPort(this) {
    m_keyMap = {
        {"delete",{0,0}},    {"return",{0,1}},   {"crsr_right",{0,2}},{"f7",{0,3}},
        {"f1",{0,4}},        {"f3",{0,5}},       {"f5",{0,6}},        {"crsr_down",{0,7}},
        {"3",{1,0}},         {"w",{1,1}},        {"a",{1,2}},         {"4",{1,3}},
        {"z",{1,4}},         {"s",{1,5}},        {"e",{1,6}},         {"left_shift",{1,7}},
        {"5",{2,0}},         {"r",{2,1}},        {"d",{2,2}},         {"6",{2,3}},
        {"c",{2,4}},         {"f",{2,5}},        {"t",{2,6}},         {"x",{2,7}},
        {"7",{3,0}},         {"y",{3,1}},        {"g",{3,2}},         {"8",{3,3}},
        {"b",{3,4}},         {"h",{3,5}},        {"u",{3,6}},         {"v",{3,7}},
        {"9",{4,0}},         {"i",{4,1}},        {"j",{4,2}},         {"0",{4,3}},
        {"m",{4,4}},         {"k",{4,5}},        {"o",{4,6}},         {"n",{4,7}},
        {"plus",{5,0}},      {"p",{5,1}},        {"l",{5,2}},         {"minus",{5,3}},
        {"period",{5,4}},    {"colon",{5,5}},    {"at",{5,6}},        {"comma",{5,7}},
        {"pound",{6,0}},     {"asterisk",{6,1}}, {"semicolon",{6,2}}, {"home",{6,3}},
        {"right_shift",{6,4}},{"equal",{6,5}},   {"arrow_up",{6,6}},  {"slash",{6,7}},
        {"1",{7,0}},         {"arrow_left",{7,1}},{"ctrl",{7,2}},     {"2",{7,3}},
        {"space",{7,4}},     {"cbm",{7,5}},      {"q",{7,6}},         {"run_stop",{7,7}},
    };
    KbdC64::clearKeys();
}

void KbdC64::updateMatrix() {
    uint8_t rows = 0xFF;
    for (int col = 0; col < 8; ++col)
        if (!(m_colPort.m_val & (1 << col)))
            rows &= m_matrix[col];
    m_rowVal = rows;
}

bool KbdC64::pressCombo(const std::vector<std::string>& keys, bool down) {
    bool ok = true;
    for (const auto& k : keys) {
        auto it = m_keyMap.find(k);
        if (it != m_keyMap.end()) {
            int col = it->second.first;
            int row = it->second.second;
            if (down) m_matrix[col] &= ~(uint8_t)(1 << row);
            else      m_matrix[col] |=  (uint8_t)(1 << row);
        } else {
            ok = false;
        }
    }
    updateMatrix();
    return ok;
}

bool KbdC64::pressKeyByName(const std::string& keyName, bool down) {
    if (keyName.empty()) return false;

    // Handle uppercase by shifting
    if (keyName.size() == 1 && keyName[0] >= 'A' && keyName[0] <= 'Z') {
        std::string lower(1, (char)std::tolower(keyName[0]));
        return pressCombo({"left_shift", lower}, down);
    }

    std::string lower = keyName;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    if (lower == "exclaim")       return pressCombo({"left_shift", "1"}, down);
    if (lower == "dquote")        return pressCombo({"left_shift", "2"}, down);
    if (lower == "hash")          return pressCombo({"left_shift", "3"}, down);
    if (lower == "dollar")        return pressCombo({"left_shift", "4"}, down);
    if (lower == "percent")       return pressCombo({"left_shift", "5"}, down);
    if (lower == "ampersand")     return pressCombo({"left_shift", "6"}, down);
    if (lower == "squote")        return pressCombo({"left_shift", "7"}, down);
    if (lower == "lparen")        return pressCombo({"left_shift", "8"}, down);
    if (lower == "rparen")        return pressCombo({"left_shift", "9"}, down);
    if (lower == "question")      return pressCombo({"left_shift", "slash"}, down);
    if (lower == "less")          return pressCombo({"left_shift", "comma"}, down);
    if (lower == "greater")       return pressCombo({"left_shift", "period"}, down);
    if (lower == "bracket_left")  return pressCombo({"left_shift", "colon"}, down);
    if (lower == "bracket_right") return pressCombo({"left_shift", "semicolon"}, down);
    
    if (lower == "backslash")     return pressKeyByName("arrow_left", down);

    if (lower == "uparrow")   lower = "arrow_up";
    if (lower == "leftarrow") lower = "arrow_left";
    if (lower == "right")     lower = "crsr_right";
    if (lower == "down")      lower = "crsr_down";
    if (lower == "up")        return pressCombo({"left_shift", "crsr_down"}, down);
    if (lower == "left")      return pressCombo({"left_shift", "crsr_right"}, down);
    
    if (lower == "shift")     lower = "left_shift";

    auto it = m_keyMap.find(lower);
    if (it != m_keyMap.end()) {
        int col = it->second.first;
        int row = it->second.second;
        if (down) m_matrix[col] &= ~(uint8_t)(1 << row);
        else      m_matrix[col] |=  (uint8_t)(1 << row);
        updateMatrix();
        return true;
    }
    return false;
}

void KbdC64::enqueueText(const std::string& text) {
    for (size_t i = 0; i < text.size(); ++i) {
        std::string name;
        if (text[i] == '\\' && i + 1 < text.size()) {
            i++;
            switch (text[i]) {
                case 'n': case 'r': name = "return"; break;
                case 't':           name = "space"; break; 
                case '\\':          name = "arrow_left"; break;
                default:            name = std::string(1, text[i]); break;
            }
        } else {
            char c = text[i];
            if (c == '\n' || c == '\r') name = "return";
            else if (c == '\t')         name = "space";
            else if (c == '\\')         name = "arrow_left";
            else {
                // On the C64, the default charset is uppercase/graphics.
                // Unshifted letter keys produce uppercase PETSCII,
                // shifted letter keys produce lowercase PETSCII.
                // So ASCII uppercase -> unshifted key (lowercase name),
                //    ASCII lowercase -> shifted key (uppercase name).
                if (c >= 'A' && c <= 'Z') {
                    name = std::string(1, (char)std::tolower(c));
                } else if (c >= 'a' && c <= 'z') {
                    name = std::string(1, (char)std::toupper(c));
                } else switch (c) {
                    case ' ': name = "space"; break;
                    case '!': name = "exclaim"; break;
                    case '"': name = "dquote"; break;
                    case '#': name = "hash"; break;
                    case '$': name = "dollar"; break;
                    case '%': name = "percent"; break;
                    case '&': name = "ampersand"; break;
                    case '\'': name = "squote"; break;
                    case '(': name = "lparen"; break;
                    case ')': name = "rparen"; break;
                    case '*': name = "asterisk"; break;
                    case '+': name = "plus"; break;
                    case ',': name = "comma"; break;
                    case '-': name = "minus"; break;
                    case '.': name = "period"; break;
                    case '/': name = "slash"; break;
                    case ':': name = "colon"; break;
                    case ';': name = "semicolon"; break;
                    case '<': name = "less"; break;
                    case '=': name = "equal"; break;
                    case '>': name = "greater"; break;
                    case '?': name = "question"; break;
                    case '@': name = "at"; break;
                    case '[': name = "bracket_left"; break;
                    case ']': name = "bracket_right"; break;
                    case '^': name = "uparrow"; break;
                    case '_': name = "leftarrow"; break;
                    default:  name = std::string(1, c); break;
                }
            }
        }
        
        if (!name.empty()) {
            m_typeQueue.push_back({name, true});
            m_typeQueue.push_back({name, false});
            m_typeQueue.push_back({"idle", false});
        }
    }
}

void KbdC64::tick(uint64_t cycles) {
    if (m_typeQueue.empty()) return;

    if (m_typeTimer > cycles) {
        m_typeTimer -= cycles;
        return;
    }

    TypeEvent ev = m_typeQueue.front();
    m_typeQueue.erase(m_typeQueue.begin());
    
    if (ev.keyName == "idle") {
        clearKeys();
    } else {
        pressKeyByName(ev.keyName, ev.down);
    }
    
    m_typeTimer = 200000; 
}
