// The REXCPM (see rexcpm.h): after VirtualT's rexC_read / rexC_set8. mmsim's own code.

#include "rexcpm.h"
#include <fstream>

namespace {
const uint8_t KEY[6] = {0xB8, 0xF2, 0xC4, 0xED, 0xA1, 0x98};
}

bool RexCpm::open(const std::string& path) {
    m_path = path;
    if (path.empty()) return true;
    std::ifstream f(path, std::ios::binary);
    if (f) {
        f.read(reinterpret_cast<char*>(m_ram.data()), (std::streamsize)m_ram.size());
        return true;
    }
    m_dirty = true;
    flush();
    return std::ifstream(path, std::ios::binary).good();
}

void RexCpm::flush() {
    if (!m_dirty || m_path.empty()) return;
    if (saveTo(m_path)) m_dirty = false;
}

bool RexCpm::saveTo(const std::string& path) const {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(m_ram.data()), (std::streamsize)m_ram.size());
    return (bool)f;
}

void RexCpm::reset() {
    m_o = 1;
    m_rl = 0;
    m_ru = 1;
    m_wp = true;
    m_state = 15;
    m_keyState = 0;
    m_count = 1;
    m_clear = true;
}

uint8_t RexCpm::ramAt(uint16_t a) const {
    const size_t block = (a & 0xC000) == 0xC000 ? m_ru : m_rl;
    return m_ram[(block << 14) | (a & 0x3FFF)];
}

uint8_t RexCpm::peek(uint16_t a, bool optionRom, const std::vector<uint8_t>& rom) const {
    if (a < 0x8000) return optionRom ? m_ram[((size_t)m_o << 15) | (a & 0x7FFF)] : rom[a & 0x7FFF];
    return ramAt(a);
}

uint8_t RexCpm::read(uint16_t a, bool optionRom, const std::vector<uint8_t>& rom) {
    // Every read is counted; the count restarts at 1 while nothing is under way
    if (m_clear) m_count = 1;
    else m_count = (m_count + 1) & 0xFF;
    if (a < 0x8000) return peek(a, optionRom, rom);
    return command(a);
}

// A RAM read ($8000-$FFFF) through the key / register state machine
uint8_t RexCpm::command(uint16_t a) {
    const uint8_t hb = (uint8_t)(a >> 8);
    const int sel = (a >> 12) & 7;
    auto idle = [this]() { m_state = 15; m_keyState = 0; m_clear = true; };
    switch (m_state) {
    case 15:
        if (hb == KEY[0]) { m_state = 14; m_keyState = 1; m_clear = false; }
        else idle();
        break;
    case 14: case 13: case 12: case 11: case 10: {
        const int at = m_keyState * 4 + 1;
        if (m_count < at) break;
        if (m_count > at || hb != KEY[m_keyState]) { idle(); break; }
        m_state = m_state == 10 ? 0 : m_state - 1;
        ++m_keyState;
        break;
    }
    case 0:                                     // the high bits: the read counted 23 (or 24)
        m_clear = false;
        m_keyState = 0;
        if (m_count < 23) break;
        if (m_count > 24) { m_state = 15; break; }
        m_state = 1;
        if (sel == 1) m_o = (uint8_t)((a & 0x0F00) >> 5);
        else if (sel == 2) m_rl = (uint8_t)((a & 0x0F00) >> 4);
        else if (sel == 3) m_ru = (uint8_t)((a & 0x0F00) >> 4);
        break;
    case 1:                                     // the low bits, and the answer: the read counted 26
        m_clear = false;
        m_keyState = 0;
        if (m_count < 26) break;
        m_state = 15;
        if (m_count > 26) break;
        switch (sel) {
        case 0: m_wp = (a >> 8) & 1; return m_wp ? 1 : 0;
        case 1: m_o = (uint8_t)(m_o + ((a & 0x0E00) >> 9)); return (uint8_t)(m_o << 1);
        case 2: m_rl = (uint8_t)(m_rl + ((a & 0x0F00) >> 8)); return m_rl;
        case 3: m_ru = (uint8_t)(m_ru + ((a & 0x0F00) >> 8)); return m_ru;
        case 4: return MODEL;
        case 5: return (uint8_t)(m_o << 1);
        case 6: return m_rl;
        default: return m_ru;
        }
    default:
        idle();
        break;
    }
    return ramAt(a);
}

void RexCpm::write(uint16_t a, uint8_t v, bool optionRom) {
    if (a < 0x8000) {
        if (!optionRom) return;
        if (m_wp && (m_o >= 4 || m_o == 1)) return;          // 32 KB blocks 1 and 4 up
        m_ram[((size_t)m_o << 15) | (a & 0x7FFF)] = v;
        m_dirty = true;
        return;
    }
    const uint8_t block = (a & 0xC000) == 0xC000 ? m_ru : m_rl;
    if (m_wp && (block >= 8 || block == 2 || block == 3)) return;   // 16 KB blocks 2, 3 and 8 up
    m_ram[((size_t)block << 14) | (a & 0x3FFF)] = v;
    m_dirty = true;
}

std::vector<uint8_t> RexCpm::saveState() const {
    std::vector<uint8_t> v = m_ram;
    v.push_back(m_o);
    v.push_back(m_rl);
    v.push_back(m_ru);
    v.push_back(m_wp);
    v.push_back((uint8_t)m_state);
    v.push_back((uint8_t)m_keyState);
    v.push_back((uint8_t)m_count);
    v.push_back(m_clear);
    return v;
}

void RexCpm::loadState(const std::vector<uint8_t>& s) {
    if (s.size() != SIZE + 8) return;
    std::copy(s.begin(), s.begin() + (long)SIZE, m_ram.begin());
    const uint8_t* p = s.data() + SIZE;
    m_o = p[0]; m_rl = p[1]; m_ru = p[2]; m_wp = p[3];
    m_state = p[4]; m_keyState = p[5]; m_count = p[6]; m_clear = p[7];
    m_dirty = true;
}
