// The Model 100's ten-HD44102 LCD, as MAME's kyocera_v.cpp and VirtualT's
// display.cpp wire it. mmsim's own code.

#include "m100_lcd.h"

uint8_t M100Lcd::read(int offset) {
    uint8_t v = 0;
    for (int i = 0; i < CHIPS; ++i)
        if (m_select & (1 << i)) v |= m_chips[i].read(offset);
    return v;
}

void M100Lcd::write(int offset, uint8_t v) {
    for (int i = 0; i < CHIPS; ++i)
        if (m_select & (1 << i)) m_chips[i].write(offset, v);
}

void M100Lcd::reset() {
    for (auto& c : m_chips) c.reset();
}

void M100Lcd::tick(uint64_t units) {
    for (auto& c : m_chips) c.tick(units);
}

void M100Lcd::setBusyTime(uint32_t units) {
    for (auto& c : m_chips) c.setBusyTime(units);
}

bool M100Lcd::pixel(int x, int y) const {
    if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) return false;
    const int i = (y / 32) * 5 + x / 50;
    return m_chips[i].pixel(x % 50, y % 32);
}

void M100Lcd::render(uint8_t* out) const {
    for (int y = 0; y < HEIGHT; ++y)
        for (int x = 0; x < WIDTH; ++x) out[y * WIDTH + x] = pixel(x, y) ? 1 : 0;
}

std::vector<uint8_t> M100Lcd::render() const {
    std::vector<uint8_t> v(WIDTH * HEIGHT);
    render(v.data());
    return v;
}

std::vector<uint8_t> M100Lcd::saveState() const {
    std::vector<uint8_t> v;
    v.push_back((uint8_t)m_select);
    v.push_back((uint8_t)(m_select >> 8));
    for (const auto& c : m_chips) {
        const auto s = c.saveState();
        v.push_back((uint8_t)s.size());
        v.insert(v.end(), s.begin(), s.end());
    }
    return v;
}

void M100Lcd::loadState(const std::vector<uint8_t>& s) {
    size_t p = 0;
    if (s.size() < 2) return;
    m_select = (uint16_t)((s[0] | (s[1] << 8)) & 0x3FF);
    p = 2;
    for (auto& c : m_chips) {
        if (p >= s.size()) break;
        const size_t n = s[p++];
        if (p + n > s.size()) break;
        c.loadState(std::vector<uint8_t>(s.begin() + p, s.begin() + p + n));
        p += n;
    }
}
