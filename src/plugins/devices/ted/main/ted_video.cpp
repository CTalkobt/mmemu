// TED drawing and palette.
//
// Ported from VICE (src/plus4/ted-draw.c, ted-color.c, src/video/
// video-color.c); ted-draw.c by Andreas Boose, Ettore Perazzoli and Tibor
// Biczo, the palette by John Selck, Philip Timmermann (Pepto) and Groepaz.
// GPL-2.0-or-later.
//
// Each line is drawn when it ends (VICE's draw alarm at cycle 114), from
// the character pointers (vbuf), attributes (cbuf) and the character or
// bitmap data fetched through FetchFn, with the state of the registers at
// that moment.

#include "ted.h"
#include <cmath>
#include <cstring>

namespace {
bool isIllegalMode(int m) { return m >= 5 && m <= 7; }

// ted-color.c ("colodore" angles), video-color.c conversion
constexpr float TED_SATURATION = 56.0f;
constexpr float TED_PHASE = -9.5f;
constexpr float ANGLE_RED = 112.5f;
constexpr float ANGLE_GRN = -112.5f;
constexpr float ANGLE_BLU = 0.0f;
constexpr float ANGLE_ORN = -45.0f;
constexpr float ANGLE_BRN = 157.5f;
constexpr float ANGLE_YLG = -157.5f;
constexpr float ANGLE_PNK = 90.0f;
constexpr float ANGLE_BLG = -90.0f;
constexpr float ANGLE_LBL = -22.5f;
constexpr float ANGLE_DBL = 22.5f;
constexpr float ANGLE_LGN = -135.0f;

struct CbmColor { float angle; int direction; };

const CbmColor COLORS_PAL[16] = {
    {ANGLE_ORN, 0}, {ANGLE_BRN, 0}, {ANGLE_RED, 1}, {ANGLE_RED, -1},     // black, white, red, cyan
    {ANGLE_GRN, -1}, {ANGLE_GRN, 1}, {ANGLE_BLU, 1}, {ANGLE_BLU, -1},    // purple, green, blue, yellow
    {ANGLE_ORN, -1}, {ANGLE_BRN, 1}, {ANGLE_YLG, 1}, {ANGLE_PNK, 1},     // orange, brown, yellow-green, pink
    {ANGLE_BLG, 1}, {ANGLE_LBL, 1}, {ANGLE_DBL, 1}, {ANGLE_LGN, 1}       // blue-green, light blue, dark blue, light green
};

const CbmColor COLORS_NTSC[16] = {
    {ANGLE_ORN, 0}, {ANGLE_BRN, 0}, {ANGLE_RED, 1}, {ANGLE_RED, -1},
    {ANGLE_GRN + 30.0f, -1}, {ANGLE_GRN + 30.0f, 1}, {ANGLE_BLU + 20.0f, 1}, {ANGLE_BLU + 20.0f, -1},
    {ANGLE_ORN, -1}, {ANGLE_BRN + 10.0f, 1}, {ANGLE_GRN, 1}, {ANGLE_PNK + 20.0f, 1},
    {ANGLE_BLG + 10.0f, 1}, {ANGLE_LBL, 1}, {ANGLE_PNK - 20.0f, 1}, {ANGLE_GRN + 10.0f, 1}
};

const float LUMINANCES[8] = {32.0f, 48.0f, 64.0f, 80.0f, 120.0f, 144.0f, 192.0f, 256.0f};

int clamp255(float f) {
    int v = (int)(f + 0.5f);
    if (v > 255) v = 255;
    if (v < 0) v = 0;
    return v;
}
}  // namespace

void Ted::buildPalette() {
    // ted_color_update_palette() -> video_cbm_palette_to_ycbcr() ->
    // video_calc_palette() with the TED defaults: saturation 1250 (1.25),
    // contrast 1000, brightness 1000, gamma 1000 (1.0), tint 1000 (0).
    const bool pal = m_std == Standard::PAL;
    const CbmColor* tc = pal ? COLORS_PAL : COLORS_NTSC;
    const double PI = 3.14159265358979323846;
    const float sat = 1.25f;
    const float basesat = TED_SATURATION / 1.75f;   // INT_SAT_ADJ
    for (int lum = 0; lum < 8; ++lum) {
        for (int col = 0; col < 16; ++col) {
            float y = col ? LUMINANCES[lum] : 0.0f;
            float cb, cr;
            const float angle = tc[col].angle;
            if (pal) {
                float u = (float)(basesat * std::cos((angle + TED_PHASE) * (PI / 180.0)));
                float v = (float)(basesat * std::sin((angle + TED_PHASE) * (PI / 180.0)));
                cb = u / 0.493111f;
                cr = v / 0.877283f;
            } else {
                cb = (float)(basesat * std::sin((angle + TED_PHASE - (100.0f / 3.0f)) * (PI / 180.0)));
                cr = (float)(basesat * std::cos((angle + TED_PHASE - (100.0f / 3.0f)) * (PI / 180.0)));
            }
            if (tc[col].direction == 0) { cb = 0.0f; cr = 0.0f; }
            if (tc[col].direction < 0)  { cb = -cb; cr = -cr; }
            cb *= sat;
            cr *= sat;
            int r, g, b;
            if (pal) {
                r = clamp255(y + 1.402f * cr);
                g = clamp255(y - 0.344136f * cb - 0.714136f * cr);
                b = clamp255(y + 1.772f * cb);
            } else {
                r = clamp255(y + 1.630f * cb + 0.317f * cr);
                g = clamp255(y - 0.378f * cb - 0.466f * cr);
                b = clamp255(y - 1.089f * cb + 1.677f * cr);
            }
            // gamma 1.0, brightness 0, contrast 1.0: video_gamma() is the identity
            m_palette[lum * 16 + col] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
    }
}

uint8_t Ted::fetchVideo(uint16_t addr, bool fromRom) const {
    return m_fetch ? m_fetch(addr, fromRom) : 0;
}

uint8_t Ted::fetchChar(uint16_t addr) const {
    // character and bitmap data: ROM when $FF12 bit 2 is set; ROM below
    // $8000 is VICE's "open space" (an all-zero buffer)
    if (m_regs[0x12] & 0x04) {
        if (addr < 0x8000) return 0;
        return fetchVideo(addr, true);
    }
    return fetchVideo(addr, false);
}

void Ted::drawGraphics(uint8_t* p) {
    const int mode = videoMode();
    const uint8_t bg = m_regs[0x15] & 0x7F;
    const uint8_t ext0 = m_regs[0x16] & 0x7F, ext1 = m_regs[0x17] & 0x7F, ext2 = m_regs[0x18] & 0x7F;

    auto hires = [&](uint8_t* q, uint8_t d, uint8_t fg, uint8_t b) {
        for (int k = 0; k < 8; ++k) q[k] = (d & (0x80 >> k)) ? fg : b;
    };
    auto multi = [&](uint8_t* q, uint8_t d, const uint8_t c[4]) {
        for (int k = 0; k < 4; ++k) {
            uint8_t col = c[(d >> (6 - 2 * k)) & 3];
            q[2 * k] = q[2 * k + 1] = col;
        }
    };

    if (m_drawIdleState) {
        // _draw_idle(): background with black pixels from the idle byte
        if (isIllegalMode(mode)) { std::memset(p, 0, 320); return; }
        uint8_t d = m_blankEnabled ? 0 : m_idleData;
        for (int i = 0; i < 40; ++i) hires(p + i * 8, d, 0, m_idleBgColor);
        return;
    }

    const uint16_t cbase = charBase();
    const uint16_t bbase = bitmapBase();
    const int y = m_ycounter;

    switch (mode) {
    case 0: {   // standard text
        int cursorPos = -1;
        if (m_cursorVisible) {
            int c = m_crsrpos - m_memptr;
            if (c >= 0 && c < 40) cursorPos = c;
        }
        for (int i = 0; i < 40; ++i) {
            uint8_t c = m_vbuf[i], a = m_cbuf[i];
            int d;
            bool flashOff = (a & 0x80) && !m_cursorVisible;
            if (m_reverseMode) {
                // 256 characters, no hardware reverse
                d = flashOff ? 0 : fetchChar((uint16_t)(cbase + c * 8 + y));
            } else {
                uint8_t inv = (c & 0x80) ? 0xFF : 0x00;
                d = flashOff ? inv : (fetchChar((uint16_t)(cbase + (c & 0x7F) * 8 + y)) ^ inv);
            }
            if (i == cursorPos) d ^= 0xFF;
            hires(p + i * 8, (uint8_t)d, a & 0x7F, bg);
        }
        break;
    }
    case 1: {   // multicolour text
        for (int i = 0; i < 40; ++i) {
            uint8_t v = m_vbuf[i] & (m_reverseMode ? 0xFF : 0x7F);
            uint8_t d = fetchChar((uint16_t)(cbase + v * 8 + y));
            uint8_t fg = m_cbuf[i] & 0x77;
            if (m_cbuf[i] & 0x08) {
                const uint8_t c[4] = {bg, ext0, ext1, fg};
                multi(p + i * 8, d, c);
            } else {
                hires(p + i * 8, d, fg, bg);
            }
        }
        break;
    }
    case 2: {   // hires bitmap
        int j = ((m_memptr << 3) + y) & 0x1FFF;
        for (int i = 0; i < 40; ++i, j = (j + 8) & 0x1FFF) {
            uint8_t v = m_vbuf[i], a = m_cbuf[i];
            uint8_t fg = (uint8_t)(((a & 0x07) << 4) | (v >> 4));
            uint8_t b  = (uint8_t)((a & 0x70) | (v & 0x0F));
            hires(p + i * 8, fetchChar((uint16_t)(bbase + j)), fg, b);
        }
        // the overscan colour in hires is the last character's
        m_idleBgColor = m_vbuf[39] & 0x7F;
        break;
    }
    case 3: {   // multicolour bitmap
        int j = ((m_memptr << 3) + y) & 0x1FFF;
        for (int i = 0; i < 40; ++i, j = (j + 8) & 0x1FFF) {
            uint8_t v = m_vbuf[i], a = m_cbuf[i];
            const uint8_t c[4] = {bg, (uint8_t)((v >> 4) + ((a & 0x07) << 4)),
                                  (uint8_t)((v & 0x0F) + (a & 0x70)), ext0};
            multi(p + i * 8, fetchChar((uint16_t)(bbase + j)), c);
        }
        break;
    }
    case 4: {   // extended colour text
        const uint8_t bgs[4] = {bg, ext0, ext1, ext2};
        for (int i = 0; i < 40; ++i) {
            uint8_t v = m_vbuf[i];
            uint8_t d = fetchChar((uint16_t)(cbase + (v & 0x3F) * 8 + y));
            hires(p + i * 8, d, m_cbuf[i] & 0x7F, bgs[v >> 6]);
        }
        break;
    }
    default:    // illegal modes: black
        std::memset(p, 0, 320);
        break;
    }
}

void Ted::drawLine(int row) {
    if (row < 0 || row >= frameHeight()) return;
    uint32_t* out = &m_back[(size_t)row * FRAME_WIDTH];
    const uint8_t border = m_regs[0x19] & 0x7F;

    if (m_blankEnabled || m_blankThisLine) {
        const uint32_t c = m_palette[border];
        for (int x = 0; x < FRAME_WIDTH; ++x) out[x] = c;
        return;
    }

    uint8_t line[FRAME_WIDTH];
    std::memset(line, border, sizeof line);
    const int xs = m_displayXStart, xe = m_displayXStop;
    for (int x = xs; x < xe; ++x) line[x] = m_idleBgColor;   // behind the X scroll

    uint8_t gfx[320];
    drawGraphics(gfx);
    const int gx = LEFT_BORDER + m_xsmooth;
    for (int i = 0; i < 320; ++i) {
        int x = gx + i;
        if (x >= xs && x < xe) line[x] = gfx[i];
    }
    for (int x = 0; x < FRAME_WIDTH; ++x) out[x] = m_palette[line[x] & 0x7F];
}

void Ted::renderFrame(uint32_t* buffer) {
    std::memcpy(buffer, m_front.data(), (size_t)FRAME_WIDTH * frameHeight() * sizeof(uint32_t));
}
