// MOS 8563 VDC register and memory interface, after VICE (src/vdc/vdc-mem.c,
// vdc.c), GPL-2.0-or-later, VICE team.

#include "vdc8563.h"
#include <algorithm>
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

Vdc8563::Vdc8563(bool ram64k)
    : m_ram(ram64k ? 0x10000 : 0x4000, 0),
      m_back((size_t)FRAME_W * FRAME_H, 0xFF000000u), m_front((size_t)FRAME_W * FRAME_H, 0xFF000000u) {
    Vdc8563::reset();
}

// vdc_reset (with the registers cleared, as at power-up)
void Vdc8563::reset() {
    std::memset(m_regs, 0, sizeof m_regs);
    m_index = 0;
    m_frameCounter = 0;
    m_acc = 0;
    m_screenTextCols = 100;
    m_xsmooth = 7;
    m_regs[0] = 126;
    m_regs[1] = 102;
    m_xcharsTotal = 127;
    m_regs[4] = 39;
    m_regs[6] = 25;
    m_interlaced = 0;
    m_regs[9] = 7;
    m_regs[22] = 0x78;
    m_charwidth = 8;
    m_attributeOffset = 0;
    m_rowCounter = m_rowCounterY = 0;
    m_rowCounterLatch = m_vertFineAdj = m_displayEnable = m_vsync = false;
    m_vsyncCounter = 0;
    m_primeDraw = m_drawActive = m_drawFinished = m_drawCounterLatch = false;
    m_drawCounter = m_drawCounterY = 0;
    m_memCounter = m_memCounterInc = m_bitmapCounter = 0;
    m_screenAdr = m_attributeAdr = 0;
    m_bufDraw = 0;
    m_attributeBlink = false;
    m_canvasHeight = m_pal ? 288 : 240;
    m_vsyncHeight = m_pal ? 25 : 21;
    m_canvasHeightOld = m_stableSizeCount = 0;
    m_currentLine = 0;
    std::fill(m_back.begin(), m_back.end(), 0xFF000000u);
    std::fill(m_front.begin(), m_front.end(), 0xFF000000u);
    updateGeometry();
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

// Vertical blank (VICE: !display_enable)
uint8_t Vdc8563::status() const {
    return 0x80 | (m_displayEnable ? 0 : 0x20) | REVISION;
}

// vdc_calculate_xsync: a line is xchars_total * 8 dots at 16 MHz, counted
// in CPU cycles (16.16)
void Vdc8563::tick(uint64_t cycles) {
    const uint64_t inc = (uint64_t)m_xcharsTotal * 8 * 65536 * m_clockHz / 16000000;
    if (!inc) return;
    m_acc += cycles << 16;
    while (m_acc >= inc) { m_acc -= inc; rasterLine(); }
}

void Vdc8563::calculateCharwidth() {
    m_charwidth = (m_regs[25] & 0x10) ? 2 * (m_regs[22] >> 4) : 1 + (m_regs[22] >> 4);
}

// vdc_update_geometry: the displayed columns and the left border (R1, R2)
void Vdc8563::updateGeometry() {
    const int r0 = m_regs[0], r1 = m_regs[1];
    if (r1 >= 6 && r1 <= 100 && r1 < r0) m_screenTextCols = r1;
    else if (r1 < 6) m_screenTextCols = 6;
    else if (r1 >= r0 && r0 > 0) m_screenTextCols = std::min(r0 - 1, 100);
    else m_screenTextCols = 100;
    int hsync = (m_regs[25] & 0x10) ? 62 * 16 - m_regs[2] * m_charwidth : 116 * 8 - m_regs[2] * m_charwidth;
    if (hsync < 0) hsync = 0;
    if (m_screenTextCols * m_charwidth > CANVAS_W) hsync = 0;
    else if (hsync + m_screenTextCols * m_charwidth > CANVAS_W) hsync = CANVAS_W - m_screenTextCols * m_charwidth;
    m_borderWidth = hsync;
    m_updateGeometry = false;
}

// Register side effects (vdc-mem.c vdc_store)
void Vdc8563::registerWritten(int reg, uint8_t oldval) {
    switch (reg) {
        case 0:
            if (m_regs[0] != oldval) {
                if (m_regs[0] >= 120 && m_regs[0] <= 127) m_xcharsTotal = m_regs[0] + 1;
                m_updateGeometry = true;
            }
            break;
        case 1: case 2: case 6:
            if (m_regs[reg] != oldval) m_updateGeometry = true;
            break;
        case 8:                                            // interlace sync and video
            m_interlaced = (m_regs[8] & 3) == 3 ? 1 : 0;
            break;
        case 9:                                            // the RFO VDC FLI 8x1 colour cell trick
            if ((oldval & 0x1F) == 0 && m_rowCounter == (unsigned)(m_regs[6] - 1)) m_attributeOffset = 3;
            break;
        case 22:
            calculateCharwidth();
            break;
        case 24:
            m_attributeBlink = (m_regs[24] & 0x20) ? (m_frameCounter & 16) : (m_frameCounter & 8);
            break;
        case 25:
            if ((m_regs[25] & 0x0F) != (oldval & 0x0F)) m_xsmooth = m_regs[25] & 0x0F;   // revisions 1-2
            if ((m_regs[25] & 0x10) != (oldval & 0x10)) m_updateGeometry = true;
            calculateCharwidth();
            break;
        case 30: fillCopy(); break;
        case 31: writeData(); break;
        default: break;
    }
}

// Take the screen and attribute addresses for the next frame and fetch the
// first row (vdc.c, after the last displayed row)
void Vdc8563::latchAddresses() {
    m_screenAdr = (uint32_t)(m_regs[12] << 8 | m_regs[13]);
    m_attributeAdr = ((uint32_t)(m_regs[20] << 8 | m_regs[21]) + m_attributeOffset) & 0xFFFF;
    m_attributeOffset = 0;
    if (m_regs[4] == 255) m_attributeAdr = (m_attributeAdr - m_regs[1]) & 0xFFFF;   // vdcmodemania FLI
    if (m_interlaced && (m_regs[25] & 0x80) && !(m_frameCounter & 1)) {
        // interlaced bitmap, even frame: the counters run on
    } else {
        m_memCounter = 0;
        m_bitmapCounter = 0;
        m_drawActive = false;
    }
    for (unsigned i = 0, j = 0; i <= m_memCounterInc; i++, j++) {
        if (j == 82) j = 41;
        m_attrbuf[m_bufDraw | j] = ramRead(m_attributeAdr + m_memCounter + i);
        m_scrnbuf[m_bufDraw | j] = ramRead(m_screenAdr + m_memCounter + i);
    }
}

// One raster line: vdc_raster_draw_alarm_handler
void Vdc8563::rasterLine() {
    const unsigned r9 = m_regs[9] & 0x1F, il = m_interlaced;
    // --- the video signal ---
    if (m_rowCounterLatch) {
        m_rowCounterLatch = false;
        m_rowCounterY = ~(m_rowCounterY ^ m_regs[9]) & il;
        m_rowCounter++;
        if (m_rowCounter == 1) m_displayEnable = true;
        if (m_rowCounter > m_regs[6]) m_displayEnable = false;
        if (m_rowCounter > m_regs[4]) {
            if (m_vertFineAdj || (m_regs[5] & 0x1F) == 0 || (m_regs[5] & 0x1F) == r9) {
                m_vertFineAdj = false;
                m_rowCounter = 0;
                m_rowCounterY = il & m_frameCounter;
                m_primeDraw = true;
                if (!m_drawFinished) latchAddresses();
                m_drawFinished = true;
            } else {
                m_vertFineAdj = true;
            }
        }
        if (m_rowCounter == m_regs[7]) {                   // vertical sync starts
            m_vsync = true;
            m_vsyncCounter = 0;
            if (m_currentLine != m_canvasHeightOld) {
                m_canvasHeightOld = m_currentLine;
                m_stableSizeCount = 0;
            } else if (m_stableSizeCount == 10) {
                m_canvasHeight = m_currentLine < 264 ? 240 : 288;
                m_vsyncHeight = m_currentLine < 264 ? 21 : 25;
                m_updateGeometry = true;
                m_stableSizeCount++;
            } else {
                m_stableSizeCount++;
            }
        }
    } else {
        m_rowCounterY = (m_rowCounterY + 1 + il) & 0x1F;
    }
    const unsigned last = m_vertFineAdj ? (m_regs[5] & 0x1F) : r9;
    if (m_rowCounterY == last || m_rowCounterY + il == last) m_rowCounterLatch = true;

    if (m_vsync && ++m_vsyncCounter > m_vsyncHeight) {     // out of the pulse: a new frame
        m_vsync = false;
        m_currentLine = 0;
        m_front = m_back;
        m_frameCounter++;
        m_attributeBlink = (m_regs[24] & 0x20) ? (m_frameCounter & 16) : (m_frameCounter & 8);
        if (m_updateGeometry) updateGeometry();
    }

    if (m_rowCounter == (unsigned)m_regs[6] + 1 && (m_rowCounterY == 1 || (il && m_rowCounterY == 2))) {
        m_drawActive = false;
        m_drawFinished = true;
        latchAddresses();
    }

    // --- drawing ---
    const uint32_t step = m_memCounterInc + m_regs[27];
    if (m_primeDraw) {
        if (m_rowCounter == 0 && m_rowCounterY == (il & m_frameCounter)) {
            m_drawCounterY = m_regs[24] & 0x1F;
            m_drawCounter = 0;
            m_drawCounterLatch = false;
            if (il & m_frameCounter) {
                m_drawCounterY = (m_drawCounterY + 1) % (r9 + 1);
                m_bitmapCounter = (m_bitmapCounter + step) & 0xFFFF;
            }
        } else if (m_drawCounterY == r9 || m_drawCounterY + il == r9) {
            m_drawCounterY = ~(m_drawCounterY ^ m_regs[9]) & il;
            m_drawCounter++;
            m_primeDraw = false;
            m_drawActive = true;
            m_drawFinished = false;
            if (il && (m_regs[25] & 0x80) && !(m_frameCounter & 1)) {
                m_memCounterInc = (uint32_t)m_screenTextCols;
                m_memCounter = (m_memCounter + m_memCounterInc + m_regs[27]) & 0xFFFF;
            }
        } else {
            m_drawCounterY = (m_drawCounterY + 1 + il) & 0x1F;
            if (il) m_bitmapCounter = (m_bitmapCounter + step) & 0xFFFF;
        }
    } else if (m_drawActive) {
        if (m_drawCounterLatch) {                          // a new character row
            m_drawCounterY = ~(m_drawCounterY ^ m_regs[9]) & il;
            m_drawCounter++;
            m_drawCounterLatch = false;
            m_bufDraw ^= 0x100;
            m_memCounterInc = (uint32_t)m_screenTextCols;
            m_memCounter = (m_memCounter + m_memCounterInc + m_regs[27]) & 0xFFFF;
            m_bitmapCounter = (m_bitmapCounter + m_memCounterInc + m_regs[27]) & 0xFFFF;
        } else {                                           // a new line of the row
            m_drawCounterY = (m_drawCounterY + 1 + il) & 0x1F;
            m_bitmapCounter = (m_bitmapCounter + step) & 0xFFFF;
        }
        if (m_drawCounterY == r9 || m_drawCounterY + il == r9) m_drawCounterLatch = true;
    } else {
        m_drawCounterLatch = false;
    }

    // --- what this line shows ---
    enum { IDLE, TEXT, BITMAP };
    int mode = IDLE;
    const int r34 = m_regs[34], r35 = m_regs[35], r0 = m_regs[0];
    if (m_vsync) mode = IDLE;
    else if ((m_regs[25] & 0x0F) > ((m_regs[22] >> 4) & 0x0F) ||
             ((m_regs[25] & 0x10) && (m_regs[25] & 0x0F) == ((m_regs[22] >> 4) & 0x0F))) mode = IDLE;
    else if ((r34 > r0 && r35 <= r0) || (r34 == r35 && r35 <= r0)) mode = IDLE;
    else if (m_charwidth < 8) mode = IDLE;
    else if (m_drawActive && m_displayEnable) mode = m_drawCounterY > r9 ? IDLE : (m_regs[25] & 0x80) ? BITMAP : TEXT;

    // Fetch the next character row into the other buffer as this one starts
    if ((m_drawCounterY | il) == il) {
        for (unsigned i = 0, j = 0; i <= m_memCounterInc; i++, j++) {
            if (j == 82) j = 41;
            const uint32_t at = m_memCounter + m_memCounterInc + m_regs[27] + i;
            m_attrbuf[(m_bufDraw ^ 0x100) | j] = ramRead(m_attributeAdr + at);
            m_scrnbuf[(m_bufDraw ^ 0x100) | j] = ramRead(m_screenAdr + at);
        }
    }

    drawLine(mode);
    m_currentLine++;
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

// Draw the current raster line (vdc-draw.c draw_std_text, draw_std_bitmap,
// draw_idle) into the picture
void Vdc8563::drawLine(int mode) {
    const int top = (m_canvasHeight - FRAME_H / 2) / 2;   // PAL 8, NTSC -16
    const int oy = m_currentLine - top;
    if (oy < 0 || oy >= FRAME_H / 2) return;
    const uint8_t* r = m_regs;
    uint8_t line[CANVAS_W + 64];
    const uint8_t border = r[26] & 0x0F;
    std::memset(line, border, sizeof line);
    auto put = [&](int x, uint8_t c) { if (x >= 0 && x < (int)sizeof line) line[x] = c; };
    const bool dbl = r[25] & 0x10;
    const int scale = dbl ? 2 : 1;
    int icsi = -1;                                          // the gap between characters
    if (dbl) { if (m_charwidth > 16) icsi = m_charwidth / 2 - 8; }
    else if (m_charwidth > 8) icsi = m_charwidth - 8;
    // a byte and the gap byte, in fg/bg, at x
    auto byteAt = [&](int x, unsigned d, unsigned d2, uint8_t fg, uint8_t bg) {
        for (int b = 0; b < 8; ++b)
            for (int k = 0; k < scale; ++k) put(x + b * scale + k, (d >> (7 - b)) & 1 ? fg : bg);
        if (icsi >= 0)
            for (int b = 0; b < 8; ++b)
                for (int k = 0; k < scale; ++k) put(x + 8 * scale + b * scale + k, (d2 >> (7 - b)) & 1 ? fg : bg);
    };
    const int ycounter = (int)m_drawCounterY;
    if (mode != 0) {
        int p = m_borderWidth + (dbl ? 2 : 0) + m_xsmooth * scale - (r[22] >> 4) * scale;
        const uint8_t* attr = &m_attrbuf[m_bufDraw];
        const uint8_t* scrn = &m_scrnbuf[m_bufDraw];
        const Masks mk = drawMasks(r);
        if (mode == 1) {                                    // text
            const uint32_t cpos = ((uint32_t)(r[14] << 8 | r[15]) - m_screenAdr - m_memCounter) & 0xFFFF;
            const uint32_t charIndex = (uint32_t)((r[28] << 8) & 0xE000) + (uint32_t)ycounter;
            const int bpc = (r[9] & 0x1F) < 16 ? 16 : 32;
            const bool attrMode = r[25] & 0x40;
            const bool cursorOn = ((m_frameCounter | 1) & CRSRBLINK[(r[10] >> 5) & 3]) != 0;
            const int cs = r[10] & 0x1F, ce = r[11] & 0x1F;
            for (int i = 0; i < m_screenTextCols; i++, p += m_charwidth) {
                const uint8_t a = attrMode ? attr[i] : 0;
                unsigned d = 0, d2 = 0;
                if (ycounter <= (int)r[23])
                    d = ramRead(charIndex + ((attrMode && (a & 0x80)) ? 0x100u * bpc : 0) + scrn[i] * (uint32_t)bpc);
                d &= mk.dmask;
                if (attrMode) {
                    if (ycounter == r[29] && (a & 0x20)) { d = 0xFF; d2 = 0xFF; }
                    if (m_attributeBlink && (a & 0x10)) { d = 0; d2 = 0; }
                }
                if (d & mk.semiTest) { d |= mk.semiMask; d2 = mk.semiType; }
                if (attrMode && (a & 0x40)) { d ^= 0xFF; d2 ^= 0xFF; }
                if (cpos == (uint32_t)i && cursorOn &&
                    ((ycounter >= cs && ycounter < ce) || (ycounter == cs && ycounter == ce) ||
                     (cs > ce && (ycounter >= cs || ycounter < ce)))) {
                    d ^= 0xFF; d2 ^= 0xFF;
                }
                if (r[24] & 0x40) { d ^= 0xFF; d2 ^= 0xFF; }
                d2 &= mk.d2mask;
                const uint8_t fg = attrMode ? (a & 0x0F) : (r[26] >> 4);
                byteAt(p, d, d2, fg, r[26] & 0x0F);
            }
            for (int i = m_xsmooth; i < (r[22] >> 4); i++, p++) put(p, r[26] & 0x0F);
        } else {                                            // bitmap
            const uint32_t bitmapIndex = m_screenAdr + m_bitmapCounter;
            const bool attrMode = r[25] & 0x40;
            unsigned i = 0;
            for (; i < m_memCounterInc; i++, p += m_charwidth) {
                unsigned d = ycounter > (int)r[23] ? 0 : ramRead(bitmapIndex + i), d2 = 0;
                d &= mk.dmask;
                if (d & mk.semiTest) { d |= mk.semiMask; d2 = mk.semiType; }
                if (r[24] & 0x40) { d ^= 0xFF; d2 ^= 0xFF; }
                d2 &= mk.d2mask;
                const uint8_t fg = attrMode ? (attr[i] & 0x0F) : (r[26] >> 4);
                const uint8_t bg = attrMode ? (attr[i] >> 4) : (r[26] & 0x0F);
                byteAt(p, d, d2, fg, bg);
            }
            unsigned d = ramRead(bitmapIndex + i);
            if (r[24] & 0x40) d ^= 0xFF;
            const uint8_t fg = attrMode ? (attr[i] >> 4) : (r[26] >> 4);
            const uint8_t bg = attrMode ? (attr[i] & 0x0F) : (r[26] & 0x0F);
            unsigned j = 0x80;
            for (int k = m_xsmooth; k < (r[22] >> 4); k++, p++, j >>= 1) put(p, (d & j) ? fg : bg);
        }
        // The border outside the display window (raster display_xstart/xstop)
        const int xstop = std::min(m_borderWidth + m_charwidth * m_screenTextCols, CANVAS_W - 1);
        for (int x = 0; x < CANVAS_W; ++x)
            if (x < m_borderWidth || x >= xstop) line[x] = border;
    }
    // Into the picture: both lines, or this field's line when interlaced
    uint32_t px[FRAME_W];
    for (int x = 0; x < FRAME_W; ++x) px[x] = palette(line[x + X_OFFSET]);
    for (int f = 0; f < 2; ++f) {
        if (m_interlaced && f != (int)(m_frameCounter & 1)) continue;
        std::memcpy(&m_back[(size_t)(oy * 2 + f) * FRAME_W], px, sizeof px);
    }
}

void Vdc8563::renderFrame(uint32_t* buffer) {
    std::memcpy(buffer, m_front.data(), m_front.size() * sizeof(uint32_t));
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
    const uint8_t old = m_regs[m_index];
    if (m_index == 16 || m_index == 17) return true;        // light pen: read only
    m_regs[m_index] = val;
    registerWritten(m_index, old);
    return true;
}
