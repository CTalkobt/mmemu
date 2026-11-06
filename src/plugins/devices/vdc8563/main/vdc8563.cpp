// MOS 8563 VDC register and memory interface, after VICE (src/vdc/vdc-mem.c,
// vdc.c), GPL-2.0-or-later, VICE team.

#include "vdc8563.h"
#include <cstring>

namespace {
// Unused register bits read 1 (vdc-mem.c regmask)
const uint8_t REGMASK[38] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0xE0, 0x00, 0x00,
    0xFC, 0xE0, 0x80, 0xE0, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xE0,
    0x00, 0x00, 0x00, 0x00, 0x0F, 0xE0, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xF0, 0x3F
};
constexpr uint8_t REVISION = 2;          // VICE's default (8563 R9)
}  // namespace

Vdc8563::Vdc8563(bool ram64k) : m_ram(ram64k ? 0x10000 : 0x4000, 0) {
    Vdc8563::reset();
}

void Vdc8563::reset() {
    std::memset(m_regs, 0, sizeof m_regs);
    m_index = 0;
    m_frameCycle = 0;
}

// R28 bit 4 selects 64 KB addressing; translate when it does not match the
// fitted chips (vdc_64k_to_16k_map / vdc_16k_to_64k_map)
uint32_t Vdc8563::map(uint16_t addr) const {
    const bool addr64 = m_regs[28] & 0x10;
    if (ram64k()) {
        if (addr64) return addr;
        return (addr & 0x80FF) | ((addr & 0x3F00) << 1) | (addr & 0x0100);
    }
    if (addr64) return (addr & 0x00FF) | ((addr & 0x7E00) >> 1);
    return addr & 0x3FFF;
}

void Vdc8563::writeData() {
    uint16_t ptr = (uint16_t)(m_regs[18] << 8 | m_regs[19]);
    m_ram[map(ptr)] = m_regs[31];
    ++ptr;
    m_regs[18] = ptr >> 8;
    m_regs[19] = ptr & 0xFF;
}

void Vdc8563::fillCopy() {
    const int len = m_regs[30] ? m_regs[30] : 256;
    uint16_t ptr = (uint16_t)(m_regs[18] << 8 | m_regs[19]);
    if (m_regs[24] & 0x80) {                               // copy
        uint16_t src = (uint16_t)(m_regs[32] << 8 | m_regs[33]);
        for (int i = 0; i < len; ++i) m_ram[map((uint16_t)(ptr + i))] = m_ram[map((uint16_t)(src + i))];
        src = (uint16_t)(src + len);
        m_regs[31] = m_ram[map((uint16_t)(src - 1))];
        m_regs[32] = src >> 8;
        m_regs[33] = src & 0xFF;
    } else {                                               // fill
        for (int i = 0; i < len; ++i) m_ram[map((uint16_t)(ptr + i))] = m_regs[31];
    }
    ptr = (uint16_t)(ptr + len);
    m_regs[18] = ptr >> 8;
    m_regs[19] = ptr & 0xFF;
}

// A line is (R0 + 1) characters of 8 dots at 16 MHz (vdc_calculate_xsync);
// a frame (R4 + 1) rows of (R9 + 1) lines plus R5
uint64_t Vdc8563::lineCycles() const {
    uint64_t c = (uint64_t)(m_regs[0] + 1) * 8 * m_clockHz / 16000000;
    return c ? c : 1;
}
uint64_t Vdc8563::frameCycles() const {
    uint64_t lines = (uint64_t)(m_regs[4] + 1) * ((m_regs[9] & 0x1F) + 1) + (m_regs[5] & 0x1F);
    if (lines < 2) lines = m_pal ? 312 : 262;              // not set up yet
    return lines * lineCycles();
}

// Vertical blank (VICE: !display_enable): past the displayed rows
uint8_t Vdc8563::status() const {
    const uint64_t line = m_frameCycle / lineCycles();
    const uint64_t shown = (uint64_t)m_regs[6] * ((m_regs[9] & 0x1F) + 1);
    return 0x80 | (line >= shown ? 0x20 : 0) | REVISION;
}

void Vdc8563::tick(uint64_t cycles) {
    const uint64_t frame = frameCycles();
    m_frameCycle += cycles;
    while (m_frameCycle >= frame) { m_frameCycle -= frame; ++m_frameCounter; }
}

// ---------------------------------------------------------------------------
// Display (vdc-draw.c)
// ---------------------------------------------------------------------------

uint32_t Vdc8563::palette(int i) {
    static const uint32_t RGBI[16] = {
        0x000000, 0x555555, 0x0000AA, 0x5555FF, 0x00AA00, 0x55FF55, 0x00AAAA, 0x55FFFF,
        0xAA0000, 0xFF5555, 0xAA00AA, 0xFF55FF, 0xAA5500, 0xFFFF55, 0xAAAAAA, 0xFFFFFF};
    return 0xFF000000u | RGBI[i & 15];
}

namespace {
// calculate_draw_masks: the dots of a character shown (R22), semigraphics
const uint8_t SEMIGFXTEST[16] = {0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01, 0, 0, 0, 0, 0, 0, 0, 0};
const uint8_t SEMIGFXMASK[16] = {0x7F, 0x3F, 0x1F, 0x0F, 0x07, 0x03, 0x01, 0x00, 0, 0, 0, 0, 0, 0, 0, 0};
const uint8_t DISPLAYEDPIXMASK[17] = {0x00, 0x80, 0xC0, 0xE0, 0xF0, 0xF8, 0xFC, 0xFE,
                                      0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
const int8_t DISPLAYEDWIDTH[256] = {
     0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
     1, 0, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
     1, 2, 0, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
     1, 2, 3, 0, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
     1, 2, 3, 4, 0, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
     1, 2, 3, 4, 5, 0, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
     1, 2, 3, 4, 5, 6, 0, 7, 7, 7, 7, 7, 7, 7, 7, 7,
     1, 2, 3, 4, 5, 6, 7, 0, 8, 8, 8, 8, 8, 8, 8, 8,
     2, 3, 4, 5, 6, 7, 8, 0, 1, 8, 8, 8, 8, 8, 8, 8,
     3, 4, 5, 6, 7, 8, 8, 0, 1, 2, 8, 8, 8, 8, 8, 8,
     4, 5, 6, 7, 8, 8, 8, 0, 1, 2, 3, 8, 8, 8, 8, 8,
     5, 6, 7, 8, 8, 8, 8, 0, 1, 2, 3, 4, 8, 8, 8, 8,
     6, 7, 8, 8, 8, 8, 8, 0, 1, 2, 3, 4, 5, 8, 8, 8,
     7, 8, 8, 8, 8, 8, 8, 0, 1, 2, 3, 4, 5, 6, 8, 8,
     8, 8, 8, 8, 8, 8, 8, 0, 1, 2, 3, 4, 5, 6, 7, 8,
     8, 8, 8, 8, 8, 8, 8, 0, 1, 2, 3, 4, 5, 6, 7, 8};
uint8_t semigfxType(int i) {                                // VICE's semigfxtype[256]
    const int hi = i >> 4, lo = i & 15;
    if (hi < 7) return 0;
    if (hi == 7) return lo < 8 ? 0xFF : 0;
    if (lo < 8) return lo < 15 - hi ? 0xFF : 0;             // rows 8-15, left half
    return lo <= hi ? 0xFF : 0;
}
// solid, none, 1/16, 1/32 (crsrblink)
const uint8_t CRSRBLINK[4] = {0x01, 0x00, 0x08, 0x10};

struct Masks { unsigned dmask, d2mask, semiTest, semiMask, semiType; };

Masks drawMasks(const uint8_t* regs) {
    Masks m{};
    int displayed;
    if (regs[25] & 0x10) {                                  // double pixel (40 columns)
        int i = (!(regs[22] & 0x08) && regs[22] >= 0x10) ? regs[22] - 0x10 : regs[22];
        displayed = DISPLAYEDWIDTH[i];
        const bool same = ((regs[22] >> 4) & 0x0F) == (regs[22] & 0x0F);
        if ((regs[25] & 0x20) || same) {
            if (same) { if ((regs[22] & 0x0F) <= 8) displayed = 1; }
            else if (displayed == 0) displayed = 8;
            m.semiTest = SEMIGFXTEST[displayed - 1];
            m.semiType = semigfxType(i);
        }
        m.dmask = DISPLAYEDPIXMASK[displayed];
        m.d2mask = (regs[22] >> 4) > 8 ? DISPLAYEDPIXMASK[(regs[22] >> 4) - 6] : 0;
    } else {
        displayed = DISPLAYEDWIDTH[regs[22]];
        if (regs[25] & 0x20) {
            if (displayed == 0) displayed = 8;
            m.semiTest = SEMIGFXTEST[displayed - 1];
            m.semiType = semigfxType(regs[22]);
        }
        m.dmask = DISPLAYEDPIXMASK[displayed];
        m.d2mask = (regs[22] >> 4) >= 8 ? DISPLAYEDPIXMASK[(regs[22] >> 4) - 6] : 0;
    }
    m.semiMask = displayed ? SEMIGFXMASK[displayed - 1] : 0;
    return m;
}
}  // namespace

void Vdc8563::renderFrame(uint32_t* buffer) {
    const uint8_t* r = m_regs;
    const int LINES = FRAME_H / 2;
    const uint32_t bgCol = palette(r[26] & 0x0F);
    for (int i = 0; i < FRAME_W * FRAME_H; ++i) buffer[i] = bgCol;

    const bool dbl = r[25] & 0x10;
    const int scale = dbl ? 2 : 1;
    const int cw = ((r[22] >> 4) + 1) * scale;             // dots a column
    const int cols = r[1], rows = r[6];
    const int ch = (r[9] & 0x1F) + 1;                       // lines a row
    if (!cols || !rows) return;
    const int dispW = cols * cw, dispH = rows * ch;
    const int x0 = (FRAME_W - dispW) / 2, y0 = (LINES - dispH) / 2;
    const int xshift = ((r[25] & 0x0F) - (r[22] >> 4)) * scale;   // smooth scroll
    const int yshift = r[24] & 0x1F;
    const int inc = cols + r[27];
    const uint16_t screen = (uint16_t)(r[12] << 8 | r[13]);
    const uint16_t attrs = (uint16_t)(r[20] << 8 | r[21]);
    const uint16_t cursor = (uint16_t)(r[14] << 8 | r[15]);
    const uint16_t charBase = (uint16_t)((r[28] & 0xE0) << 8);
    const int bpc = (r[9] & 0x1F) > 15 ? 32 : 16;
    const bool bitmap = r[25] & 0x80, attrMode = r[25] & 0x40;
    const bool blinkOn = (r[24] & 0x20) ? (m_frameCounter & 16) : (m_frameCounter & 8);
    const bool cursorOn = ((m_frameCounter | 1) & CRSRBLINK[(r[10] >> 5) & 3]) != 0;
    const int cs = r[10] & 0x1F, ce = r[11] & 0x1F;
    const Masks mk = drawMasks(r);

    auto plot = [&](int x, int line, uint32_t c) {
        if (x < x0 || x >= x0 + dispW || line < y0 || line >= y0 + dispH) return;
        buffer[(line * 2) * FRAME_W + x] = c;
        buffer[(line * 2 + 1) * FRAME_W + x] = c;
    };

    // One extra row for the vertical smooth scroll
    for (int row = 0; row <= rows; ++row) {
        for (int yc = 0; yc < ch; ++yc) {
            const int ycRow = row * ch + yc - yshift;       // line in the picture
            if (ycRow < 0 || ycRow >= dispH) continue;
            const int line = y0 + ycRow;
            const uint16_t rowScreen = (uint16_t)(screen + row * inc);
            const uint16_t rowAttr = (uint16_t)(attrs + row * inc);
            const uint16_t bitmapLine = (uint16_t)(screen + (row * ch + yc) * inc);
            for (int i = 0; i < cols; ++i) {
                const uint8_t a = attrMode ? m_ram[map((uint16_t)(rowAttr + i))] : 0;
                unsigned d = 0, d2 = 0;
                int fg, bg;
                if (bitmap) {
                    if (yc <= (r[23] & 0x1F)) d = m_ram[map((uint16_t)(bitmapLine + i))];
                    d &= mk.dmask;
                    if (d & mk.semiTest) { d |= mk.semiMask; d2 = mk.semiType; }
                    if (r[24] & 0x40) { d ^= 0xFF; d2 ^= 0xFF; }
                    if (attrMode) { fg = a & 0x0F; bg = a >> 4; }
                    else { fg = r[26] >> 4; bg = r[26] & 0x0F; }
                } else {
                    const uint8_t code = m_ram[map((uint16_t)(rowScreen + i))];
                    if (yc <= (r[23] & 0x1F)) {
                        uint16_t at = (uint16_t)(charBase + code * bpc + yc);
                        if (attrMode && (a & 0x80)) at = (uint16_t)(at + 0x100 * bpc);   // alternate set
                        d = m_ram[map(at)];
                    }
                    d &= mk.dmask;
                    if (attrMode) {
                        if (yc == (r[29] & 0x1F) && (a & 0x20)) { d = 0xFF; d2 = 0xFF; }   // underline
                        if (blinkOn && (a & 0x10)) { d = 0; d2 = 0; }
                    }
                    if (d & mk.semiTest) { d |= mk.semiMask; d2 = mk.semiType; }
                    if (attrMode && (a & 0x40)) { d ^= 0xFF; d2 ^= 0xFF; }
                    if ((uint16_t)(rowScreen + i) == cursor && cursorOn &&
                        ((yc >= cs && yc < ce) || (yc == cs && yc == ce) || (cs > ce && (yc >= cs || yc < ce)))) {
                        d ^= 0xFF; d2 ^= 0xFF;
                    }
                    if (r[24] & 0x40) { d ^= 0xFF; d2 ^= 0xFF; }
                    if (attrMode) { fg = a & 0x0F; bg = r[26] & 0x0F; }
                    else { fg = r[26] >> 4; bg = r[26] & 0x0F; }
                }
                d2 &= mk.d2mask;
                const uint32_t f = palette(fg), b = palette(bg);
                const int x = x0 + xshift + i * cw;
                const int dots = cw / scale;
                for (int px = 0; px < dots; ++px) {
                    bool on = px < 8 ? (d >> (7 - px)) & 1 : (d2 >> (7 - (px - 8))) & 1;
                    for (int k = 0; k < scale; ++k) plot(x + px * scale + k, line, on ? f : b);
                }
            }
        }
    }
}

bool Vdc8563::ioPeek(IBus*, uint32_t addr, uint8_t* val) {
    if (!(addr & 1)) { *val = status(); return true; }
    if (m_index == 31) { *val = m_ram[map((uint16_t)(m_regs[18] << 8 | m_regs[19]))]; return true; }
    *val = m_index < 38 ? (uint8_t)(m_regs[m_index] | REGMASK[m_index]) : 0xFF;
    return true;
}

bool Vdc8563::ioRead(IBus* bus, uint32_t addr, uint8_t* val) {
    if ((addr & 1) && m_index == 31) {                     // RAM, then advance
        uint16_t ptr = (uint16_t)(m_regs[18] << 8 | m_regs[19]);
        *val = m_ram[map(ptr)];
        ++ptr;
        m_regs[18] = ptr >> 8;
        m_regs[19] = ptr & 0xFF;
        return true;
    }
    return ioPeek(bus, addr, val);
}

bool Vdc8563::ioWrite(IBus*, uint32_t addr, uint8_t val) {
    if (!(addr & 1)) { m_index = val & 0x3F; return true; }
    if (m_index >= 38) return true;
    m_regs[m_index] = val;
    if (m_index == 30) fillCopy();
    else if (m_index == 31) writeData();
    return true;
}
