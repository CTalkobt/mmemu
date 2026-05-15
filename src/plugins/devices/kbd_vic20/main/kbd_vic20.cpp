#include "kbd_vic20.h"
#include <cstring>
#include <algorithm>

KbdVic20::KbdVic20()
    : m_colPort(this), m_rowPort(this)
{
    m_keyMap = {
        // col 0 (PB0)
        {"1",          {0, 0}}, {"3",          {0, 1}}, {"5",        {0, 2}}, {"7",     {0, 3}},
        {"9",          {0, 4}}, {"plus",       {0, 5}}, {"pound",    {0, 6}}, {"delete",{0, 7}},
        // col 1 (PB1)
        {"arrow_left", {1, 0}}, {"w",          {1, 1}}, {"r",        {1, 2}}, {"y",     {1, 3}},
        {"i",          {1, 4}}, {"p",          {1, 5}}, {"asterisk", {1, 6}}, {"return",{1, 7}},
        // col 2 (PB2)
        {"control",    {2, 0}}, {"a",          {2, 1}}, {"d",        {2, 2}}, {"g",     {2, 3}},
        {"j",          {2, 4}}, {"l",          {2, 5}}, {"semicolon",{2, 6}}, {"right", {2, 7}},
        // col 3 (PB3)
        {"run_stop",   {3, 0}}, {"left_shift", {3, 1}}, {"x",        {3, 2}}, {"v",     {3, 3}},
        {"n",          {3, 4}}, {"comma",      {3, 5}}, {"slash",    {3, 6}}, {"down",  {3, 7}},
        // col 4 (PB4)
        {"space",      {4, 0}}, {"z",          {4, 1}}, {"c",        {4, 2}}, {"b",     {4, 3}},
        {"m",          {4, 4}}, {"period",     {4, 5}}, {"right_shift",{4,6}},{"f1",    {4, 7}},
        // col 5 (PB5)
        {"cbm",        {5, 0}}, {"s",          {5, 1}}, {"f",        {5, 2}}, {"h",     {5, 3}},
        {"k",          {5, 4}}, {"minus",      {5, 5}}, {"equal",    {5, 6}}, {"f3",    {5, 7}},
        // col 6 (PB6)
        {"q",          {6, 0}}, {"e",          {6, 1}}, {"t",        {6, 2}}, {"u",     {6, 3}},
        {"o",          {6, 4}}, {"at",         {6, 5}}, {"up",       {6, 6}}, {"f5",    {6, 7}},
        // col 7 (PB7)
        {"2",          {7, 0}}, {"4",          {7, 1}}, {"6",        {7, 2}}, {"8",     {7, 3}},
        {"0",          {7, 4}}, {"arrow_up",   {7, 5}}, {"home",     {7, 6}}, {"f7",    {7, 7}},
    };
    KbdVic20::clearKeys();
}

void KbdVic20::clearKeys() {
    std::memset(m_matrix, 0xFF, sizeof(m_matrix));
    updateMatrix();
}

void KbdVic20::keyDown(int row, int col) {
    if (row >= 0 && row < 8 && col >= 0 && col < 8) {
        m_matrix[row] &= ~(1 << col);
        updateMatrix();
    }
}

void KbdVic20::keyUp(int row, int col) {
    if (row >= 0 && row < 8 && col >= 0 && col < 8) {
        m_matrix[row] |= (1 << col);
        updateMatrix();
    }
}

bool KbdVic20::pressKeyByName(const std::string& keyName, bool down) {
    if (keyName.empty()) return false;

    // Handle uppercase by shifting
    if (keyName.size() == 1 && keyName[0] >= 'A' && keyName[0] <= 'Z') {
        std::string lower(1, (char)std::tolower(keyName[0]));
        return pressCombo({"left_shift", lower}, down);
    }

    std::string lower = keyName;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    
    // Handle symbolic names that require Shift on VIC-20
    if (lower == "exclaim")   return pressCombo({"left_shift", "1"}, down);
    if (lower == "dquote")    return pressCombo({"left_shift", "2"}, down);
    if (lower == "hash")      return pressCombo({"left_shift", "3"}, down);
    if (lower == "dollar")    return pressCombo({"left_shift", "4"}, down);
    if (lower == "percent")   return pressCombo({"left_shift", "5"}, down);
    if (lower == "ampersand") return pressCombo({"left_shift", "6"}, down);
    if (lower == "squote")    return pressCombo({"left_shift", "7"}, down);
    if (lower == "lparen")    return pressCombo({"left_shift", "8"}, down);
    if (lower == "rparen")    return pressCombo({"left_shift", "9"}, down);
    if (lower == "question")  return pressCombo({"left_shift", "slash"}, down);
    if (lower == "less")      return pressCombo({"left_shift", "comma"}, down);
    if (lower == "greater")   return pressCombo({"left_shift", "period"}, down);
    if (lower == "colon")     return pressCombo({"left_shift", "semicolon"}, down);
    if (lower == "bracket_left")  return pressCombo({"left_shift", "at"}, down);
    
    if (lower == "backslash") return pressKeyByName("arrow_left", down);

    // Aliases
    if (lower == "uparrow")   lower = "arrow_up";
    if (lower == "leftarrow") lower = "arrow_left";
    if (lower == "shift")     lower = "left_shift";

    if (m_keyMap.count(lower)) {
        auto pos = m_keyMap[lower];
        if (down) keyDown(pos.first, pos.second);
        else keyUp(pos.first, pos.second);
        return true;
    }
    return false;
}

bool KbdVic20::pressCombo(const std::vector<std::string>& keys, bool down) {
    bool ok = true;
    for (const auto& k : keys) {
        if (m_keyMap.count(k)) {
            auto pos = m_keyMap[k];
            if (down) keyDown(pos.first, pos.second);
            else keyUp(pos.first, pos.second);
        } else {
            ok = false;
        }
    }
    return ok;
}

void KbdVic20::updateMatrix() {
    uint8_t cols = m_colPort.m_val;
    uint8_t rows = 0xFF;

    for (int i = 0; i < 8; ++i) {
        if (!(cols & (1 << i))) {
            rows &= m_matrix[i];
        }
    }
    m_rowVal = rows;
}

void KbdVic20::enqueueText(const std::string& text) {
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
                if (c >= 'A' && c <= 'Z') name = std::string(1, c);
                else {
                    switch (c) {
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
                        case '^': name = "uparrow"; break;
                        case '_': name = "leftarrow"; break;
                        default:  name = std::string(1, c); break;
                    }
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

void KbdVic20::tick(uint64_t cycles) {
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
