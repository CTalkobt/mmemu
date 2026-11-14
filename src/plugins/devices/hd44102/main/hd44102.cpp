// HD44102 LCD column driver, after the Hitachi data sheet; MAME's
// hd44102.cpp and VirtualT's io.c consulted for behaviour. mmsim's own code.

#include "hd44102.h"

namespace {
template <class T> void put(std::vector<uint8_t>& v, T x) {
    for (size_t i = 0; i < sizeof(T); ++i) v.push_back((uint8_t)((uint64_t)x >> (8 * i)));
}
template <class T> void get(const uint8_t*& p, const uint8_t* e, T& x) {
    uint64_t r = 0;
    for (size_t i = 0; i < sizeof(T) && p < e; ++i) r |= (uint64_t)*p++ << (8 * i);
    x = (T)r;
}
}  // namespace

void Hd44102::reset() {
    m_off = true;
    m_up = true;
    m_startPage = 0;
    m_busyLeft = 0;
}

void Hd44102::setResetPin(bool held) {
    m_resetHeld = held;
    if (held) reset();
}

uint8_t Hd44102::status() const {
    return (uint8_t)((m_busyLeft ? STATUS_BUSY : 0) | (m_up ? STATUS_UP : 0) | (m_off ? STATUS_OFF : 0) |
                     (m_resetHeld ? STATUS_RESET : 0));
}

uint8_t Hd44102::peek(int offset) const {
    return (offset & 1) ? m_latch : status();
}

uint8_t Hd44102::read(int offset) {
    return (offset & 1) ? readData() : status();
}

void Hd44102::write(int offset, uint8_t v) {
    if (offset & 1) writeData(v);
    else writeInstruction(v);
}

void Hd44102::step() {
    if (m_up) m_y = (m_y >= COLUMNS - 1) ? 0 : m_y + 1;
    else m_y = (m_y <= 0) ? COLUMNS - 1 : m_y - 1;
}

void Hd44102::writeInstruction(uint8_t v) {
    if (m_busyLeft || m_resetHeld) return;
    switch (v) {
    case 0x38: m_off = true; break;
    case 0x39: m_off = false; break;
    case 0x3A: m_up = false; break;
    case 0x3B: m_up = true; break;
    default:
        if ((v & 0x3F) == 0x3E) m_startPage = v >> 6;
        else if ((v & 0x3F) < COLUMNS) {
            m_page = v >> 6;
            m_y = v & 0x3F;
        }
        // Y 50-63 (other than the instructions above) is not an address: ignored
        break;
    }
    startBusy();
}

void Hd44102::writeData(uint8_t v) {
    if (m_busyLeft || m_resetHeld) return;
    m_ram[m_page][m_y] = v;
    step();
    startBusy();
}

uint8_t Hd44102::readData() {
    const uint8_t v = m_latch;
    m_latch = m_ram[m_page][m_y];
    step();
    startBusy();
    return v;
}

void Hd44102::tick(uint64_t units) {
    m_busyLeft = m_busyLeft > units ? m_busyLeft - units : 0;
}

bool Hd44102::pixel(int x, int y) const {
    if (m_off || x < 0 || x >= COLUMNS || y < 0 || y >= ROWS) return false;
    // Display row y shows RAM row (y + 8 * start page) mod 32
    const int r = (y + 8 * m_startPage) & 31;
    return (m_ram[r >> 3][x] >> (r & 7)) & 1;
}

void Hd44102::render(uint8_t* out, int stride) const {
    for (int y = 0; y < ROWS; ++y)
        for (int x = 0; x < COLUMNS; ++x) out[y * stride + x] = pixel(x, y) ? 1 : 0;
}

std::vector<uint8_t> Hd44102::saveState() const {
    std::vector<uint8_t> v;
    for (const auto& pg : m_ram) v.insert(v.end(), pg.begin(), pg.end());
    put(v, m_latch);
    put(v, (uint8_t)m_page);
    put(v, (uint8_t)m_y);
    put(v, (uint8_t)m_startPage);
    put(v, (uint8_t)((m_up ? 1 : 0) | (m_off ? 2 : 0) | (m_resetHeld ? 4 : 0)));
    put(v, m_busyLeft);
    return v;
}

void Hd44102::loadState(const std::vector<uint8_t>& s) {
    const uint8_t* p = s.data();
    const uint8_t* e = p + s.size();
    for (auto& pg : m_ram)
        for (auto& b : pg) get(p, e, b);
    uint8_t page = 0, y = 0, start = 0, flags = 0;
    get(p, e, m_latch);
    get(p, e, page);
    get(p, e, y);
    get(p, e, start);
    get(p, e, flags);
    get(p, e, m_busyLeft);
    m_page = page & 3;
    m_y = y < COLUMNS ? y : 0;
    m_startPage = start & 3;
    m_up = flags & 1;
    m_off = flags & 2;
    m_resetHeld = flags & 4;
}
