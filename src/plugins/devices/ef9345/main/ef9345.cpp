// Thomson EF9345: the behaviour after MAME's ef9345.cpp (Daniel Coulom,
// Sandro Ronco; GPL-2.0+); mmsim's own code.

#include "ef9345.h"
#include <algorithm>
#include <cstring>

namespace {

inline bool bit(unsigned v, int n) { return (v >> n) & 1; }

// The service row and the busy times (ns), from MAME
constexpr uint64_t ROW_DELAY_NS = 104000;
constexpr uint64_t BLINK_NS = 500000000;

template <class T> void put(std::vector<uint8_t>& v, const T& x) {
    const auto* p = reinterpret_cast<const uint8_t*>(&x);
    v.insert(v.end(), p, p + sizeof(T));
}
template <class T> void get(const uint8_t*& p, const uint8_t* end, T& x) {
    if (p + sizeof(T) > end) return;
    std::memcpy(&x, p, sizeof(T));
    p += sizeof(T);
}

}  // namespace

Ef9345::Ef9345()
    : m_vram(VRAM_SIZE, 0), m_charset(CHARSET_SIZE, 0), m_accented(CHARSET_SIZE, 0),
      m_out((size_t)W80 * OUT_H, 0) {
    setClock(1e6);
    reset();
}

void Ef9345::setCharset(const std::vector<uint8_t>& rom) {
    std::fill(m_charset.begin(), m_charset.end(), 0);
    std::copy_n(rom.begin(), std::min(rom.size(), CHARSET_SIZE), m_charset.begin());
    m_haveCharset = rom.size() >= CHARSET_SIZE;
    // The accented letters: G0's lower case (at $0600) with the accents drawn on
    for (int j = 0; j < 0x10; ++j)
        for (int i = 0; i < 0x200; ++i) m_accented[(j << 9) + i] = m_charset[0x0600 + i];
    for (int j = 0; j < 0x200; j += 0x40) {
        for (int i = 0; i < 4; ++i) {
            uint8_t* a = m_accented.data() + j + i;
            a[0x0200 + 4] |= 0x1c;                         // tilde
            a[0x0400 + 4] |= 0x10; a[0x0400 + 8] |= 0x08;  // acute
            a[0x0600 + 4] |= 0x04; a[0x0600 + 8] |= 0x08;  // grave
            a[0x0a00 + 4] |= 0x1c;
            a[0x0c00 + 4] |= 0x10; a[0x0c00 + 8] |= 0x08;
            a[0x0e00 + 4] |= 0x04; a[0x0e00 + 8] |= 0x08;
            a[0x1200 + 4] |= 0x08;                         // point
            a[0x1400 + 4] |= 0x14;                         // trema
            a[0x1600 + 32] |= 0x08; a[0x1600 + 36] |= 0x04; // cedilla
            a[0x1a00 + 4] |= 0x08;
            a[0x1c00 + 4] |= 0x14;
            a[0x1e00 + 32] |= 0x08; a[0x1e00 + 36] |= 0x04;
        }
    }
}

void Ef9345::setClock(double hz) {
    m_hz = hz > 0 ? hz : 1e6;
    m_lineTicks = std::max<uint64_t>(1, nsToTicks(64000));
}

void Ef9345::reset() {
    m_tgs = m_mat = m_pat = m_dor = m_ror = 0;
    m_state = 0;
    m_busy = false;
    m_busyLeft = 0;
    m_block = 0;
    m_blink = 0;
    m_latchC0 = m_latchM = m_latchI = m_latchU = 0;
    std::memset(m_lastDial, 0, sizeof m_lastDial);
    std::memset(m_reg, 0, sizeof m_reg);
    std::memset(m_ramBase, 0, sizeof m_ramBase);
    std::fill(m_out.begin(), m_out.end(), 0);
    m_line = 0;
    m_lineLeft = m_lineTicks;
    m_blinkLeft = nsToTicks(BLINK_NS);
    setVideoMode();
}

// ---------------------------------------------------------------------------
// The host's side
// ---------------------------------------------------------------------------
uint8_t Ef9345::peek(uint8_t offset) const {
    if (offset & 7) return m_reg[offset & 7];
    return m_busy ? (uint8_t)(m_state | 0x80) : (uint8_t)(m_state & 0x7f);
}

uint8_t Ef9345::read(uint8_t offset) {
    const uint8_t v = peek(offset);
    if (!(offset & 7)) m_state = v;
    return v;
}

void Ef9345::write(uint8_t offset, uint8_t v) {
    m_reg[offset & 7] = v;
    if (offset & 8) exec(m_reg[0]);
}

void Ef9345::setBusy(uint64_t ns) {
    m_busy = true;
    m_busyLeft = std::max<uint64_t>(1, nsToTicks(ns));
}

void Ef9345::advance(uint64_t ticks) {
    while (ticks) {
        uint64_t d = std::min(ticks, m_lineLeft);
        if (m_busy) {
            if (m_busyLeft <= d) { m_busy = false; m_busyLeft = 0; }
            else m_busyLeft -= d;
        }
        if (m_blinkLeft <= d) {
            m_blink = (uint8_t)((m_blink + 3) & 3);   // 11 10 01 00: flash 0.5 Hz (bit 1), cursor 1 Hz (bit 0)
            m_blinkLeft = nsToTicks(BLINK_NS);
        } else {
            m_blinkLeft -= d;
        }
        m_lineLeft -= d;
        ticks -= d;
        if (m_lineLeft == 0) {
            m_lineLeft = m_lineTicks;
            m_line = (m_line + 1) % LINES;
            if (m_line == 0) ++m_frames;
            if (m_line % 10 == 0 && m_line <= 260) scanline(m_line);
        }
    }
}

// ---------------------------------------------------------------------------
// Addresses
// ---------------------------------------------------------------------------
// The video RAM's address for pointer R(r-1) (Y) / R(r) (X)
uint16_t Ef9345::indexRam(int r) const {
    const uint8_t x = m_reg[r];
    uint8_t y = m_reg[r - 1];
    if (y < 8) y &= 1;
    return (uint16_t)((x & 0x3f) | ((x & 0x40) << 6) | ((x & 0x80) << 4) | ((y & 0x1f) << 6) | ((y & 0x20) << 8));
}

void Ef9345::incX(int r) {
    uint8_t i = (uint8_t)((m_reg[r] & 0x3f) + 1);
    if (i > 39) {
        i -= 40;
        m_state |= 0x40;
    }
    m_reg[r] = (uint8_t)((m_reg[r] & 0xc0) | i);
}

void Ef9345::incY(int r) {
    uint8_t i = (uint8_t)((m_reg[r] & 0x1f) + 1);
    if (i > 31) i -= 24;
    m_reg[r] = (uint8_t)((m_reg[r] & 0xe0) | i);
}

void Ef9345::setVideoMode() {
    switch ((bit(m_pat, 7) << 2) | (bit(m_tgs, 7) << 1) | bit(m_tgs, 6)) {
        case 1: m_mode = Mode::Var40; break;
        case 4: m_mode = Mode::Short40; break;
        case 3: m_mode = Mode::Bits12x80; break;
        case 2: m_mode = Mode::Bits8x80; break;
        default: m_mode = Mode::Long40; break;
    }
    std::memset(m_border, m_mat & 0x07, sizeof m_border);
    // The character sets in the video RAM
    m_ramBase[0] = (uint16_t)((m_dor & 0x07) << 11);
    m_ramBase[1] = m_ramBase[0];
    m_ramBase[2] = (uint16_t)((m_dor & 0x30) << 8);
    m_ramBase[3] = (uint16_t)(m_ramBase[2] + 0x0800);
    // The page displayed
    m_block = (uint16_t)(0x0800 * ((((m_ror & 0xf0) >> 4) | ((m_ror & 0x40) >> 5) | ((m_ror & 0x20) >> 3)) & 0x0c));
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------
void Ef9345::exec(uint8_t cmd) {
    m_state = 0;
    if ((m_reg[5] & 0x3f) == 39) m_state |= 0x10;     // LXa
    if ((m_reg[7] & 0x3f) == 39) m_state |= 0x20;     // LXm
    uint16_t a = indexRam(7);
    auto wr = [this](uint16_t addr, uint8_t v) { setVram(addr, v); };
    auto rd = [this](uint16_t addr) { return vram(addr); };
    switch (cmd) {
        case 0x00: case 0x01:                         // KRF: R1-R3 to the RAM
            setBusy(4000);
            wr(a, m_reg[1]); wr((uint16_t)(a + 0x0800), m_reg[2]); wr((uint16_t)(a + 0x1000), m_reg[3]);
            if (cmd & 1) incX(7);
            break;
        case 0x02: case 0x03:                         // KRG: R1-R2 to the RAM
            setBusy(5500);
            wr(a, m_reg[1]); wr((uint16_t)(a + 0x0800), m_reg[2]);
            if (cmd & 1) incX(7);
            break;
        case 0x05: case 0x07:                         // CLF / CLG: clear the page
            // (MAME leaves the chip busy until the next command; mmsim gives
            // each code KRF's 4 us)
            setBusy(4000ull * 32 * 40);
            for (int i = 0; i < 32 * 40; ++i) {
                a = indexRam(7);
                wr(a, m_reg[1]); wr((uint16_t)(a + 0x0800), m_reg[2]);
                if (cmd == 0x05) wr((uint16_t)(a + 0x1000), m_reg[3]);
                incX(7);
                if ((m_reg[7] & 0x3f) == 0) incY(6);
            }
            break;
        case 0x08: case 0x09:                         // KRF: the RAM to R1-R3
            setBusy(7500);
            m_reg[1] = rd(a); m_reg[2] = rd((uint16_t)(a + 0x0800)); m_reg[3] = rd((uint16_t)(a + 0x1000));
            if (cmd & 1) incX(7);
            break;
        case 0x0a: case 0x0b:                         // KRG: the RAM to R1-R2
            setBusy(7500);
            m_reg[1] = rd(a); m_reg[2] = rd((uint16_t)(a + 0x0800));
            if (cmd & 1) incX(7);
            break;
        case 0x30: case 0x31:                         // OCT: R1 to the RAM, main pointer
            setBusy(4000);
            wr(indexRam(7), m_reg[1]);
            if (cmd & 1) { incX(7); if ((m_reg[7] & 0x3f) == 0) incY(6); }
            break;
        case 0x34: case 0x35:                         // OCT: R1 to the RAM, auxiliary pointer
            setBusy(4000);
            wr(indexRam(5), m_reg[1]);
            if (cmd & 1) incX(5);
            break;
        case 0x38: case 0x39:                         // OCT: the RAM to R1, main pointer
            setBusy(4500);
            m_reg[1] = rd(indexRam(7));
            if (cmd & 1) { incX(7); if ((m_reg[7] & 0x3f) == 0) incY(6); }
            break;
        case 0x3c: case 0x3d:                         // OCT: the RAM to R1, auxiliary pointer
            setBusy(4500);
            m_reg[1] = rd(indexRam(5));
            if (cmd & 1) incX(5);
            break;
        case 0x50: case 0x51:                         // KRL: 80 columns, 12 bits, write
            setBusy(12500);
            wr(a, m_reg[1]);
            if (((a / 0x0800) & 1) == 0) {
                const uint8_t t = rd((uint16_t)(a + 0x1000));
                wr((uint16_t)(a + 0x1000), (uint8_t)((t & 0x0f) | (m_reg[3] & 0xf0)));
            } else {
                const uint8_t t = rd((uint16_t)(a + 0x0800));
                wr((uint16_t)(a + 0x0800), (uint8_t)((t & 0xf0) | (m_reg[3] & 0x0f)));
            }
            if (cmd & 1) {
                if (!(m_reg[7] & 0x80)) { m_reg[7] |= 0x80; return; }
                m_reg[7] &= (uint8_t)~0x80;
                incX(7);
            }
            break;
        case 0x58: case 0x59:                         // KRL: 80 columns, 12 bits, read
            setBusy(11500);
            m_reg[1] = rd(a);
            m_reg[3] = ((a / 0x0800) & 1) == 0 ? rd((uint16_t)(a + 0x1000)) : rd((uint16_t)(a + 0x0800));
            if (cmd & 1) {
                if (!(m_reg[7] & 0x80)) { m_reg[7] |= 0x80; break; }
                m_reg[7] &= (uint8_t)~0x80;
                incX(7);
            }
            break;
        case 0x80:                                    // IND: R1 to the ROM (nothing)
            break;
        case 0x81: case 0x82: case 0x83: case 0x84: case 0x87:   // IND: R1 to TGS, MAT, PAT, DOR, ROR
            setBusy(2000);
            switch (cmd & 7) {
                case 1: m_tgs = m_reg[1]; break;
                case 2: m_mat = m_reg[1]; break;
                case 3: m_pat = m_reg[1]; break;
                case 4: m_dor = m_reg[1]; break;
                case 7: m_ror = m_reg[1]; break;
            }
            setVideoMode();
            m_state &= 0x8f;
            break;
        case 0x88: case 0x89: case 0x8a: case 0x8b: case 0x8c: case 0x8f:   // IND: the ROM, TGS... to R1
            setBusy(3500);
            switch (cmd & 7) {
                case 0: {
                    const uint8_t type = (uint8_t)(((m_reg[6] & 0x20) >> 3) | ((m_reg[7] & 0x40) >> 5) | ((m_reg[7] & 0x80) >> 7));
                    const uint16_t addr = (uint16_t)(((m_reg[6] & 0x1f) << 6) | (m_reg[7] & 0x3f));
                    m_reg[1] = readChar(type, addr);
                    break;
                }
                case 1: m_reg[1] = m_tgs; break;
                case 2: m_reg[1] = m_mat; break;
                case 3: m_reg[1] = m_pat; break;
                case 4: m_reg[1] = m_dor; break;
                case 7: m_reg[1] = m_ror; break;
            }
            m_state &= 0x8f;
            break;
        case 0x90: case 0x91: case 0x95: case 0x99:   // NOP, VRM, VSM
            setBusy(1000);
            break;
        case 0xb0:                                    // INY
            setBusy(2000);
            incY(6);
            m_state &= 0x8f;
            break;
        case 0xd5: case 0xd6: case 0xd9: case 0xda:   // MVB / MVD / MVT: block moves (stop at the line's end, or not)
        case 0xe5: case 0xe6: case 0xe9: case 0xea:
        case 0xf5: case 0xf6: case 0xf9: case 0xfa: {
            const int n = (cmd >> 4) - 0x0c;
            const int r1 = (cmd & 0x04) ? 7 : 5;
            const int r2 = (cmd & 0x04) ? 5 : 7;
            uint64_t ns = 2000;
            for (int i = 0; i < 32 * 40; ++i) {
                const uint16_t a1 = indexRam(r1), a2 = indexRam(r2);
                wr(a2, rd(a1));
                if (n > 1) wr((uint16_t)(a2 + 0x0800), rd((uint16_t)(a1 + 0x0800)));
                if (n > 2) wr((uint16_t)(a2 + 0x1000), rd((uint16_t)(a1 + 0x1000)));
                incX(r1);
                incX(r2);
                if ((m_reg[5] & 0x3f) == 0 && (cmd & 1)) break;
                if ((m_reg[7] & 0x3f) == 0) {
                    if (cmd & 1) break;
                    incY(6);
                }
                ns += 4000ull * (uint64_t)n;
            }
            m_state &= 0x8f;
            setBusy(ns);
            break;
        }
        default:                                      // KRC (0x40, 0x41, 0x48, 0x49) and the rest: not done
            break;
    }
}

// ---------------------------------------------------------------------------
// The display
// ---------------------------------------------------------------------------
uint8_t Ef9345::readChar(uint8_t index, uint16_t addr) const {
    if (index < 0x04) return m_charset[(0x0800 * index + addr) & (CHARSET_SIZE - 1)];
    if (index < 0x08) return m_accented[(0x0800 * (index & 3) + addr) & (CHARSET_SIZE - 1)];
    if (index < 0x0c) return vram((uint16_t)(m_ramBase[index - 8] + addr));
    return vram(addr);
}

// The quarter of a double-size character this cell shows
uint8_t Ef9345::dial(int x, uint8_t attrib) {
    uint8_t& d = m_lastDial[x];
    if (x > 0 && m_lastDial[x - 1] == 1) d = 2;          // top right
    else if (x > 0 && m_lastDial[x - 1] == 5) d = 10;    // right half
    else if (d == 1) d = 4;                              // bottom left
    else if (d == 2) d = 8;                              // bottom right
    else if (d == 3) d = 12;                             // lower half
    else if (attrib == 1) d = 5;                         // left half
    else if (attrib == 2) d = 3;                         // upper half
    else if (attrib == 3) d = 1;                         // top left
    else d = 0;
    return d;
}

void Ef9345::zoom(uint8_t* pix, uint16_t n) {
    if ((n & 0x0a) == 0)                                  // the left side
        for (int i = 0; i < 80; i += 8)
            for (int j = 7; j > 0; --j) pix[i + j] = pix[i + j / 2];
    if ((n & 0x05) == 0)                                  // the right side
        for (int i = 0; i < 80; i += 8)
            for (int j = 0; j < 7; ++j) pix[i + j] = pix[i + 4 + j / 2];
    if ((n & 0x0c) == 0)                                  // the top
        for (int i = 0; i < 8; ++i)
            for (int j = 9; j > 0; --j) pix[i + 8 * j] = pix[i + 8 * ((j - 1) / 2)];
    if ((n & 0x03) == 0)                                  // the bottom
        for (int i = 0; i < 8; ++i)
            for (int j = 0; j < 9; ++j) pix[i + 8 * j] = pix[i + 32 + 8 * ((j + 1) / 2)];
}

uint16_t Ef9345::indexRow(int y) const {
    // The service row comes from Y=0 or Y=1 (TGS b5)
    const int j = y == 0 ? bit(m_tgs, 5) : (m_ror & 0x1f) + y - 1;
    return (uint16_t)(j > 31 ? j - 24 : j);
}

uint16_t Ef9345::indexBlock(int x, int y) const {
    int i = x;
    const int j = indexRow(y);
    if ((m_tgs & 0x80) == 0 && x > 0) {                  // the right side of a double-width character
        if (m_lastDial[x - 1] == 1) --i;
        if (m_lastDial[x - 1] == 4) --i;
        if (m_lastDial[x - 1] == 5) --i;
    }
    return (uint16_t)(0x40 * j + i);
}

void Ef9345::makeColours(uint8_t& c0, uint8_t& c1, bool& underline, bool insert, bool flash, bool conceal,
                         bool negative, bool cursor) const {
    uint8_t compl_ = 0;
    underline = false;
    if (negative) std::swap(c0, c1);
    if (cursor) {
        switch (m_mat & 0x70) {
            case 0x40: compl_ = 7; break;                         // fixed, complemented
            case 0x50: underline = true; break;                   // fixed, underlined
            case 0x60: if (m_blink & 1) compl_ = 7; break;        // flashing, complemented
            case 0x70: if (m_blink & 1) underline = true; break;  // flashing, underlined
        }
    }
    switch (m_pat & 0x30) {                                       // the insert mode
        case 0x00:                                                // inlay
            if (insert) { c0 = 0; c1 = (uint8_t)((c1 ^ compl_) | 8); }
            else c0 = c1 = 0;
            break;
        case 0x10:                                                // boxing
            if (insert) { c0 = (uint8_t)((c0 ^ compl_) | 8); c1 = (uint8_t)((c1 ^ compl_) | 8); }
            else c0 = c1 = 0;
            break;
        case 0x20:                                                // character mark
            if (insert) { c0 = (uint8_t)((c0 ^ compl_) | 8); c1 = (uint8_t)((c1 ^ compl_) | 8); }
            else { c0 ^= compl_; c1 ^= compl_; }
            break;
        case 0x30:                                                // active area mark
            c0 = (uint8_t)((c0 ^ compl_) | 8); c1 = (uint8_t)((c1 ^ compl_) | 8);
            break;
    }
    // Flashing characters blink on the opposite phase when negative
    if ((flash && (m_pat & 0x40) && negative == bit(m_blink, 1)) || (conceal && (m_pat & 0x08))) c1 = c0;
}

void Ef9345::drawChar40(const uint8_t* c, int x, int y) {
    const int w = std::min(W40 - x * 8, 8), h = std::min(OUT_H - y * 10, 10);
    for (int i = 0; i < h; ++i)
        for (int j = 0; j < w; ++j) m_out[(size_t)(y * 10 + i) * W80 + (size_t)(x * 8 + j)] = c[8 * i + j] & 7;
}

void Ef9345::drawChar80(const uint8_t* c, int x, int y) {
    const int w = std::min(W80 - x * 6, 6), h = std::min(OUT_H - y * 10, 10);
    for (int i = 0; i < h; ++i)
        for (int j = 0; j < w; ++j) m_out[(size_t)(y * 10 + i) * W80 + (size_t)(x * 6 + j)] = c[6 * i + j] & 7;
}

void Ef9345::drawBorder(int row) {
    if (is80()) for (int i = 0; i < 82; ++i) drawChar80(m_border, i, row);
    else for (int i = 0; i < 42; ++i) drawChar40(m_border, i, row);
}

void Ef9345::bichrome40(uint8_t type, uint16_t address, uint8_t dl, uint16_t iblock, int x, int y, uint8_t c0,
                        uint8_t c1, bool insert, bool flash, bool conceal, bool negative, bool underline) {
    uint8_t pix[80];
    int i = m_reg[6] & 0x1f;                              // the cursor on this character?
    if (i < 8) i &= 1;
    if (dl > 0 && (dl & 0x05) == 0) ++iblock;             // the right side
    const bool cursor = iblock == 0x40 * i + (m_reg[7] & 0x3f);
    bool cursorUnderline;
    makeColours(c0, c1, cursorUnderline, insert, flash, conceal, negative, cursor);
    if ((type & 7) != 2 && (type & 7) != 3) underline |= cursorUnderline;   // none under semigraphics
    for (int r = 0; r < 40; r += 4) {
        const uint8_t ch = readChar(type, (uint16_t)(address + r));
        for (int b = 0; b < 8; ++b) pix[r * 2 + b] = (ch & (1 << b)) ? c1 : c0;
    }
    if (underline) std::memset(&pix[72], c1, 8);
    if (dl > 0) zoom(pix, dl);
    if (m_mat & 0x80) zoom(pix, (y & 1) ? 0x0c : 0x03);   // double height everywhere
    drawChar40(pix, x + 1, y + 1);
}

void Ef9345::quadrichrome40(uint8_t c, uint8_t b, uint8_t a, int x, int y) {
    // C0-6 the character, B1 low resolution, B2 the subset (low resolution),
    // B3-5 the set, A0-7 the four colours (insert, B0, not done)
    uint8_t col[8], pix[80];
    const int lowRes = bit(b, 1);
    m_lastDial[x] = 0;                                     // no double size
    for (int n = 0, i = 0; i < 8; ++i) {
        col[i] = 7;
        if (bit(a, i)) col[n++] = (uint8_t)i;
    }
    const int block = (bit(b, 5) << 2) | (bit(b, 3) << 1) | bit(b, 4);
    uint16_t index = (uint16_t)(0x0800 * block + 0x40 * ((c >> 2) & 0x1f) + (c & 3));
    if (lowRes) index = (uint16_t)(index + 5 * (b & 0x04));
    for (int i = 0, j = 0; i < 10; ++i) {
        const uint8_t ch = readChar(0x0c, (uint16_t)(index + 4 * (i >> lowRes)));
        for (int s = 0; s < 8; s += 2) { pix[j] = pix[j + 1] = col[(ch >> s) & 3]; j += 2; }
    }
    drawChar40(pix, x + 1, y + 1);
}

void Ef9345::bichrome80(uint8_t c, uint8_t a, int x, int y, bool cursor) {
    uint8_t pix[60];
    const bool insert = bit(m_dor, bit(a, 0) ? 7 : 3);
    uint8_t c1 = (uint8_t)((m_dor >> (bit(a, 0) ? 4 : 0)) & 7);   // the foreground: DOR
    uint8_t c0 = m_mat & 7;                                         // the background: MAT
    if (!(c & 0x80)) {
        // A1 underline, A2 flash, A3 negative; C0-6 the character
        bool underline = bit(a, 1);
        bool cursorUnderline;
        makeColours(c0, c1, cursorUnderline, insert, bit(a, 2), false, bit(a, 3), cursor);
        underline ^= cursorUnderline;
        const uint16_t d = (uint16_t)(((c >> 2) & 0x1f) * 0x40 + (c & 3));
        for (int i = 0, j = 0; i < 10; ++i) {
            const uint8_t ch = readChar(0, (uint16_t)(d + 4 * i));
            for (int b = 0; b < 6; ++b) pix[j++] = bit(ch, b) ? c1 : c0;
        }
        if (underline) std::fill_n(&pix[54], 6, c1);
    } else {
        // The mosaic set: C0-6 and A1-3, ten blocks of 3 x 2
        bool cursorUnderline;
        makeColours(c0, c1, cursorUnderline, insert, false, false, false, cursor);
        pix[0] = bit(c, 0) ? c1 : c0;  pix[3] = bit(c, 1) ? c1 : c0;
        pix[12] = bit(c, 2) ? c1 : c0; pix[15] = bit(c, 3) ? c1 : c0;
        pix[24] = bit(c, 4) ? c1 : c0; pix[27] = bit(c, 5) ? c1 : c0;
        pix[36] = bit(c, 6) ? c1 : c0; pix[39] = bit(a, 1) ? c1 : c0;
        pix[48] = bit(a, 2) ? c1 : c0; pix[51] = bit(a, 3) ? c1 : c0;
        for (int i = 0; i < 60; i += 12) { pix[i + 6] = pix[i]; pix[i + 9] = pix[i + 3]; }
        for (int i = 0; i < 60; i += 3) pix[i + 2] = pix[i + 1] = pix[i];
        if (cursorUnderline) std::fill_n(&pix[54], 6, c1);
    }
    drawChar80(pix, x + 1, y + 1);
}

void Ef9345::makeChar16x40(int x, int y) {
    const uint16_t iblock = (m_mat & 0x80 && y > 1) ? indexBlock(x, y / 2) : indexBlock(x, y);
    const uint8_t a = vram((uint16_t)(m_block + iblock));
    const uint8_t b = vram((uint16_t)(m_block + iblock + 0x0800));
    const uint8_t dl = dial(x, bit(a, 7) ? 0 : (uint8_t)((bit(a, 4) << 1) | bit(a, 5)));
    uint8_t type = (uint8_t)(((b & 0x80) >> 4) | ((a & 0x80) >> 6));
    uint16_t address = (uint16_t)(((b >> 2) & 0x1f) * 0x40 + (b & 3));
    if (x == 0) m_latchM = m_latchI = m_latchU = m_latchC0 = 0;
    if ((b & 0xe0) == 0x80) {                             // a delimiter: shows G0's 127, sets the serial attributes
        type = 0;
        address = (127 >> 2) * 0x40 + (127 & 3);
        m_latchM = b & 1;
        m_latchI = (b >> 1) & 1;
        m_latchU = (b >> 2) & 1;
    }
    if (a & 0x80) m_latchC0 = (a & 0x70) >> 4;
    bichrome40(type, address, dl, iblock, x, y, m_latchC0, a & 7, m_latchI, bit(a, 3), m_latchM,
               bit(a, 7) ? false : bit(a, 6), m_latchU);
}

void Ef9345::makeChar24x40(int x, int y) {
    const uint16_t iblock = (m_mat & 0x80 && y > 1) ? indexBlock(x, y / 2) : indexBlock(x, y);
    const uint8_t c = vram((uint16_t)(m_block + iblock));
    const uint8_t b = vram((uint16_t)(m_block + iblock + 0x0800));
    const uint8_t a = vram((uint16_t)(m_block + iblock + 0x1000));
    if ((b & 0xc0) == 0xc0) { quadrichrome40(c, b, a, x, y); return; }
    const uint8_t dl = dial(x, (uint8_t)((bit(b, 1) << 1) | bit(b, 3)));
    const uint16_t address = (uint16_t)(((c >> 2) & 0x1f) * 0x40 + (c & 3));
    const uint8_t type = (b & 0xf0) >> 4;
    const bool u = (((type & 0x6) == 0) || ((type & 0xc) == 0x4)) ? bit(b, 4) : false;
    bichrome40(type, address, dl, iblock, x, y, a & 7, (a >> 4) & 7, bit(b, 0), bit(a, 3), bit(b, 2), bit(a, 7), u);
}

void Ef9345::makeChar12x80(int x, int y) {
    const uint16_t iblock = indexBlock(x, y);
    const bool odd = bit(m_reg[7], 7);
    int i = m_reg[6] & 0x1f;
    if (i < 8) i &= 1;
    const bool cursor = iblock == 0x40 * i + (m_reg[7] & 0x3f);
    const uint8_t attr = vram((uint16_t)(m_block + iblock + 0x1000));
    bichrome80(vram((uint16_t)(m_block + iblock)), (attr >> 4) & 0x0f, 2 * x, y, cursor && !odd);
    bichrome80(vram((uint16_t)(m_block + iblock + 0x0800)), attr & 0x0f, 2 * x + 1, y, cursor && odd);
}

void Ef9345::makeChar(int x, int y) {
    switch (m_mode) {
        case Mode::Long40: makeChar24x40(x, y); break;
        case Mode::Short40: makeChar16x40(x, y); break;
        case Mode::Bits12x80: makeChar12x80(x, y); break;
        default: break;                                   // variable 40, 8-bit 80: not done
    }
}

// Lines 0, 10, ... 260: the row of text that starts there, and its margins
void Ef9345::scanline(int line) {
    if (line == 250) m_state &= 0xfb;                     // past the screen: VSYNC's flag off
    if (m_busy) m_busyLeft += nsToTicks(ROW_DELAY_NS);    // the display has the memory
    const int row = line / 10;
    if (is80()) { drawChar80(m_border, 0, row + 1); drawChar80(m_border, 81, row + 1); }
    else { drawChar40(m_border, 0, row + 1); drawChar40(m_border, 41, row + 1); }
    if (line == 0) {
        m_state |= 0x04;                                  // VSYNC's flag
        drawBorder(0);                                    // the top margin
        if (m_pat & 1) for (int i = 0; i < 40; ++i) makeChar(i, row);   // the service row
        else for (int i = 0; i < 42; ++i) drawChar40(m_border, i, 1);
    } else if (line < 120) {
        if (m_pat & 2) for (int i = 0; i < 40; ++i) makeChar(i, row);   // the upper bulk
        else drawBorder(row);
    } else if (line < 250) {
        if (m_pat & 4) for (int i = 0; i < 40; ++i) makeChar(i, row);   // the lower bulk
        else drawBorder(row);
        if (line == 240) drawBorder(26);                  // the bottom margin
    }
}

void Ef9345::render(uint32_t* out) const {
    static const uint32_t PALETTE[8] = {
        0xFF000000, 0xFFFF0000, 0xFF00FF00, 0xFFFFFF00, 0xFF0000FF, 0xFFFF00FF, 0xFF00FFFF, 0xFFFFFFFF,
    };
    const int w = width();
    for (int y = 0; y < OUT_H; ++y)
        for (int x = 0; x < w; ++x) *out++ = PALETTE[m_out[(size_t)y * W80 + (size_t)x] & 7];
}

std::string Ef9345::rowText(int y) const {
    std::string s;
    const int j = indexRow(y);
    for (int x = 0; x < 40; ++x) {
        const uint16_t at = (uint16_t)(m_block + 0x40 * j + x);
        uint8_t c = 0, type = 0;
        if (m_mode == Mode::Short40) {
            const uint8_t a = vram(at), b = vram((uint16_t)(at + 0x0800));
            c = b & 0x7f;
            type = (uint8_t)(((b & 0x80) >> 4) | ((a & 0x80) >> 6));
        } else {
            const uint8_t b = vram((uint16_t)(at + 0x0800));
            c = vram(at) & 0x7f;
            type = (b & 0xc0) == 0xc0 ? 0xff : (b & 0xf0) >> 4;
        }
        s += (type == 0 && c >= 0x20 && c < 0x7f) ? (char)c : '.';
    }
    return s;
}

// ---------------------------------------------------------------------------
// Snapshots
// ---------------------------------------------------------------------------
std::vector<uint8_t> Ef9345::saveState() const {
    std::vector<uint8_t> v(m_vram);
    for (uint8_t r : m_reg) put(v, r);
    put(v, m_state); put(v, m_tgs); put(v, m_mat); put(v, m_pat); put(v, m_dor); put(v, m_ror);
    put(v, m_blink); put(v, m_busy); put(v, m_busyLeft); put(v, m_lineLeft); put(v, m_line);
    put(v, m_blinkLeft); put(v, m_frames);
    return v;
}

void Ef9345::loadState(const std::vector<uint8_t>& s) {
    if (s.size() < VRAM_SIZE) return;
    std::copy_n(s.begin(), VRAM_SIZE, m_vram.begin());
    const uint8_t* p = s.data() + VRAM_SIZE;
    const uint8_t* end = s.data() + s.size();
    for (uint8_t& r : m_reg) get(p, end, r);
    get(p, end, m_state); get(p, end, m_tgs); get(p, end, m_mat); get(p, end, m_pat); get(p, end, m_dor);
    get(p, end, m_ror); get(p, end, m_blink); get(p, end, m_busy); get(p, end, m_busyLeft);
    get(p, end, m_lineLeft); get(p, end, m_line); get(p, end, m_blinkLeft); get(p, end, m_frames);
    setVideoMode();
}
