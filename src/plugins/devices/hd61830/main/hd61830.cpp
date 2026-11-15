// HD61830 LCD controller, after the Hitachi data sheet; MAME's hd61830.cpp
// and VirtualT's T200_Disp consulted for behaviour. mmsim's own code.

#include "hd61830.h"

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

void Hd61830::setRamSize(size_t bytes) {
    size_t n = 1;
    while (n < bytes && n < 0x10000) n <<= 1;
    m_ram.assign(n, 0);
    m_mask = (uint16_t)(n - 1);
}

void Hd61830::reset() {
    m_mcr &= (uint8_t)~(MODE_MASTER | MODE_DISPLAY_ON);
    m_hp = 6;
    m_busyLeft = 0;
}

uint8_t Hd61830::read(int offset) {
    return (offset & 1) ? status() : readData();
}

void Hd61830::write(int offset, uint8_t v) {
    if (offset & 1) writeInstruction(v);
    else writeData(v);
}

uint8_t Hd61830::readData() {
    const uint8_t v = m_dor;
    m_dor = m_ram[m_cac & m_mask];
    stepCursor();
    return v;
}

void Hd61830::writeData(uint8_t v) {
    if (m_busyLeft) return;
    switch (m_ir) {
    case 0x0: m_mcr = v; break;
    case 0x1:
        m_hp = (v & 0x07) + 1;
        m_vp = (v >> 4) + 1;
        break;
    case 0x2:
        m_hn = (v & 0x7F) + 1;
        if (m_hn & 1) ++m_hn;
        break;
    case 0x3: m_nx = (v & 0x7F) + 1; break;
    case 0x4: m_cp = v & 0x0F; break;
    case 0x8: m_dsa = (uint16_t)((m_dsa & 0xFF00) | v); break;
    case 0x9: m_dsa = (uint16_t)((v << 8) | (m_dsa & 0x00FF)); break;
    case 0xA:
        if ((m_cac & 0x80) && !(v & 0x80)) m_cac = (uint16_t)((((m_cac >> 8) + 1) << 8) | v);
        else m_cac = (uint16_t)((m_cac & 0xFF00) | v);
        break;
    case 0xB: m_cac = (uint16_t)((v << 8) | (m_cac & 0x00FF)); break;
    case 0xC:
        m_ram[m_cac & m_mask] = v;
        stepCursor();
        break;
    case 0xE:
        m_ram[m_cac & m_mask] &= (uint8_t)~(1 << (v & 7));
        stepCursor();
        break;
    case 0xF:
        m_ram[m_cac & m_mask] |= (uint8_t)(1 << (v & 7));
        stepCursor();
        break;
    default:
        return;    // $5-$7 undefined; $D takes no data (reads do the work)
    }
    m_busyLeft = m_clockScale ? (uint64_t)busyClocks(m_ir) * m_clockScale : m_busyTime;
}

int Hd61830::busyClocks(uint8_t ir) {
    // MAME hd61830.cpp CYCLES[]: 4 for the set-up instructions, 6 for data
    // write/read, 36 for clear/set bit
    static const int CYCLES[16] = {4, 4, 4, 4, 4, 0, 0, 0, 4, 4, 4, 4, 6, 6, 36, 36};
    return CYCLES[ir & 0x0F];
}

void Hd61830::frame() {
    if (++m_frames >= 32) {
        m_frames = 0;
        m_blinkOn = !m_blinkOn;
    }
}

uint8_t Hd61830::charLine(uint8_t code, int line) const {
    if (m_mcr & MODE_EXTERNAL_CG) return externalCg ? externalCg(code, line) : 0;
    if (code >= 0x20 && code < 0x80 && line < 7) return FONT_5X7[code - 0x20][line];
    return 0;
}

void Hd61830::render(uint8_t* out, int w, int h) const {
    for (int i = 0; i < w * h; ++i) out[i] = 0;
    if (!(m_mcr & MODE_DISPLAY_ON)) return;
    auto dot = [&](int x, int y, bool on) {
        if (x >= 0 && x < w && y >= 0 && y < h) out[y * w + x] = on ? 1 : 0;
    };
    if (m_mcr & MODE_GRAPHIC) {
        // 2 x nx lines of hn bytes, the lower half continuing the upper one
        uint16_t a = m_dsa;
        for (int y = 0; y < 2 * m_nx; ++y)
            for (int c = 0; c < m_hn; ++c, ++a) {
                const uint8_t b = m_ram[a & m_mask];
                for (int bit = 0; bit < m_hp; ++bit) dot(c * m_hp + bit, y, (b >> bit) & 1);
            }
        return;
    }
    const int rows = m_vp ? m_nx / m_vp : 0;
    const bool cursor = m_mcr & MODE_CURSOR, blink = m_mcr & MODE_BLINK;
    uint16_t a = (uint16_t)(m_dsa & 0x0FFF);    // character mode: 12-bit start address
    for (int row = 0; row < 2 * rows; ++row)
        for (int c = 0; c < m_hn; ++c, ++a) {
            const uint8_t code = m_ram[a & m_mask];
            const bool atCursor = (a & m_mask) == (m_cac & m_mask);
            for (int line = 0; line < m_vp; ++line) {
                uint8_t bits = charLine(code, line);
                if (atCursor) {
                    if (cursor && line == m_cp) bits = (!blink || m_blinkOn) ? 0xFF : 0x00;
                    else if (!cursor && blink && m_blinkOn) bits = 0xFF;
                }
                for (int x = 0; x < m_hp; ++x) dot(c * m_hp + x, row * m_vp + line, (bits >> x) & 1);
            }
        }
}

std::vector<uint8_t> Hd61830::render(int w, int h) const {
    std::vector<uint8_t> v((size_t)(w * h));
    render(v.data(), w, h);
    return v;
}

std::vector<uint8_t> Hd61830::saveState() const {
    std::vector<uint8_t> v;
    put(v, (uint32_t)m_ram.size());
    v.insert(v.end(), m_ram.begin(), m_ram.end());
    put(v, m_ir); put(v, m_mcr); put(v, m_dor);
    put(v, (uint8_t)m_hp); put(v, (uint8_t)m_vp); put(v, (uint8_t)m_hn); put(v, (uint8_t)m_nx);
    put(v, (uint8_t)m_cp);
    put(v, m_dsa); put(v, m_cac);
    put(v, m_busyLeft);
    put(v, (uint8_t)m_frames); put(v, (uint8_t)m_blinkOn);
    return v;
}

void Hd61830::loadState(const std::vector<uint8_t>& s) {
    const uint8_t* p = s.data();
    const uint8_t* e = p + s.size();
    uint32_t n = 0;
    get(p, e, n);
    if (n == 0 || (size_t)(e - p) < n) return;
    setRamSize(n);
    for (auto& b : m_ram) b = *p++;
    uint8_t hp = 6, vp = 8, hn = 40, nx = 64, cp = 7, frames = 0, blink = 0;
    get(p, e, m_ir); get(p, e, m_mcr); get(p, e, m_dor);
    get(p, e, hp); get(p, e, vp); get(p, e, hn); get(p, e, nx); get(p, e, cp);
    get(p, e, m_dsa); get(p, e, m_cac);
    get(p, e, m_busyLeft);
    get(p, e, frames); get(p, e, blink);
    m_hp = hp; m_vp = vp; m_hn = hn; m_nx = nx; m_cp = cp; m_frames = frames; m_blinkOn = blink;
}
