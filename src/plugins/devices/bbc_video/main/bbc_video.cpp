// BBC Micro video: 6845 CRTC, video ULA and SAA5050, after b-em video.c
// (GPL-2.0-or-later). The NuLA extensions and interlaced display are left out.

#include "bbc_video.h"
#include <algorithm>
#include <cstring>
#include <fstream>

namespace {

constexpr uint8_t CRTC_MASK[32] = {0xFF, 0xFF, 0xFF, 0xFF, 0x7F, 0x1F, 0x7F, 0x7F, 0xF3, 0x1F,
                                   0x7F, 0x1F, 0x3F, 0xFF, 0x3F, 0xFF, 0x3F, 0xFF};
// The eight colours: black, red, green, yellow, blue, magenta, cyan, white
constexpr uint32_t COLOURS[8] = {0xFF000000, 0xFFFF0000, 0xFF00FF00, 0xFFFFFF00,
                                 0xFF0000FF, 0xFFFF00FF, 0xFF00FFFF, 0xFFFFFFFF};
constexpr uint32_t BLACK = 0xFF000000;
constexpr uint16_t SCREEN_LEN[4] = {0x4000, 0x5000, 0x2000, 0x2800};
constexpr uint8_t CURSOR_LOOK[7] = {0, 0, 0, 0x80, 0x40, 0x20, 0x20};
constexpr int CDRAW_LOOK[4] = {3, 2, 1, 0};
constexpr int CMASK[4] = {0, 0, 8, 16};
constexpr int NUM_CHARS = 96;

uint32_t colour(int n) { return COLOURS[n & 7]; }

}  // namespace

BbcVideo::BbcVideo() {
    m_canvas.assign((size_t)WIDTH * HEIGHT, BLACK);
    m_frame = m_canvas;
    // b-em video_init: the pixel lookup for 1, 2 and 4 bits a pixel
    for (int temp = 0; temp < 256; temp++) {
        int temp2 = temp;
        for (int c = 0; c < 16; c++) {
            int left = 0;
            if (temp2 & 2) left |= 1;
            if (temp2 & 8) left |= 2;
            if (temp2 & 32) left |= 4;
            if (temp2 & 128) left |= 8;
            m_table4bpp[3][temp][c] = (uint8_t)left;
            temp2 = (temp2 << 1) | 1;
        }
        for (int c = 0; c < 16; c++) {
            m_table4bpp[2][temp][c] = m_table4bpp[3][temp][c >> 1];
            m_table4bpp[1][temp][c] = m_table4bpp[3][temp][c >> 2];
            m_table4bpp[0][temp][c] = m_table4bpp[3][temp][c >> 3];
        }
    }
    // mode7_gen_nula_lookup: foreground over background, by the pixel's weight
    for (int fg = 0; fg < 8; fg++)
        for (int bg = 0; bg < 8; bg++)
            for (int w = 0; w < 16; w++) {
                const uint32_t f = colour(fg), b = colour(bg);
                auto mix = [&](int sh) {
                    const int fc = (int)((f >> sh) & 0xFF), bc = (int)((b >> sh) & 0xFF);
                    return (uint32_t)(bc + ((fc - bc) * w) / 15) << sh;
                };
                m_m7Lookup[fg][bg][w] = 0xFF000000u | mix(16) | mix(8) | mix(0);
            }
    for (int c = 0; c < 16; c++) m_ulaPal[c] = colour(7);
    reset();
}

bool BbcVideo::loadTeletextFont(const std::string& path, std::string* err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        if (err) *err = "missing teletext font " + path;
        return false;
    }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return setTeletextFont(data, err);
}

bool BbcVideo::setTeletextFont(const std::vector<uint8_t>& data, std::string* err) {
    // b-em mode7_load_file: "BEMTTX01", width, rows, name length, name, then
    // the characters, the contiguous and the separated graphics
    if (data.size() < 11 || std::memcmp(data.data(), "BEMTTX01", 8) != 0) {
        if (err) *err = "not a b-em teletext font";
        return false;
    }
    const int width = data[8], rows = data[9], nameLen = data[10];
    const size_t bank = (size_t)width * rows * NUM_CHARS;
    const size_t start = 11 + (size_t)nameLen;
    if (width != 16 || data.size() < start + 3 * bank) {
        if (err) *err = "teletext font: unexpected size";
        return false;
    }
    m_font.assign(data.begin() + (long)start, data.begin() + (long)(start + 3 * bank));
    m_m7Width = width;
    m_m7BytesPerChar = width * rows;
    m_m7Chars = m_font.data();
    m_m7Graph = m_m7Chars + bank;
    m_m7SepGraph = m_m7Graph + bank;
    m_m7P = m_m7HeldP = m_m7Chars;
    return true;
}

void BbcVideo::reset() {
    // crtc_reset, video_reset
    m_hc = m_vc = m_sc = m_vadj = 0;
    m_crtc[9] = 10;
    m_vsyncTime = m_hvblCount = m_frameOdd = 0;
    m_con = m_cdraw = m_cursorOn = m_charsLeft = 0;
}

void BbcVideo::setReg(int reg, uint8_t v) {
    v &= CRTC_MASK[reg];
    m_crtc[reg] = v;
    if (reg == 6 && m_vc == v) m_vdispen = 0;
    else if (reg == 12) m_ttxBank = (uint16_t)(0x3C00 | ((v & 0x8) << 11));
}

void BbcVideo::crtcWrite(uint16_t addr, uint8_t v) {
    if (!(addr & 1)) m_crtcIndex = v & 31;
    else setReg(m_crtcIndex, v);
}

uint8_t BbcVideo::crtcRead(uint16_t addr) const {
    // The address register reads back as b-em has it; data: the register
    return (addr & 1) ? m_crtc[m_crtcIndex] : (uint8_t)m_crtcIndex;
}

void BbcVideo::latchLightPen() {
    m_crtc[0x10] = (uint8_t)((m_ma >> 8) & 0x3F);
    m_crtc[0x11] = (uint8_t)(m_ma & 0xFF);
}

void BbcVideo::ulaWrite(uint16_t addr, uint8_t val) {
    switch (addr & 1) {      // &FE22/&FE23 are NuLA's: the original ULA decodes A0 only
        case 0:
            // Video control register: the flash bit swaps the flashing colours
            if ((m_ulaCtrl ^ val) & 1) {
                for (int c = 0; c < 16; c++) {
                    const bool flash = (val & 1) && (m_ulaPalBak[c] & 8);
                    m_ulaPal[c] = colour(flash ? m_ulaPalBak[c] : (m_ulaPalBak[c] ^ 7));
                }
            }
            m_ulaCtrl = val;
            m_ulaMode = (val >> 2) & 3;
            m_crtcMode = (val & 2) ? TELETEXT : (val & 0x10) ? HIFREQ : LOFREQ;
            break;
        case 1: {
            // Palette: logical colour (top nibble) to physical (bottom, inverted)
            const int code = val >> 4;
            m_ulaPalBak[code] = val & 15;
            m_ulaPal[code] = colour((val & 15) ^ 7);
            if ((val & 8) && (m_ulaCtrl & 1)) m_ulaPal[code] = colour(val & 15);
            break;
        }
    }
}

void BbcVideo::put(int x, int y, uint32_t c) {
    x -= X_ORIGIN;
    if (x < 0 || x >= WIDTH || y < 0 || y >= LINES) return;
    m_canvas[(size_t)(2 * y) * WIDTH + x] = c;
    m_canvas[(size_t)(2 * y + 1) * WIDTH + x] = c;
}

void BbcVideo::puts(int x, int y, int n, uint32_t c) {
    for (int i = 0; i < n; i++) put(x + i, y, c);
}

uint32_t BbcVideo::get(int x, int y) const {
    x -= X_ORIGIN;
    if (x < 0 || x >= WIDTH || y < 0 || y >= LINES) return BLACK;
    return m_canvas[(size_t)(2 * y) * WIDTH + x];
}

// b-em mode7_render: one character cell of a scan line; both fields (the
// SAA5050 rounds characters to 20 lines: the font's low nibble is the even
// field's line, the high nibble the odd's)
void BbcVideo::mode7Render(uint8_t dat) {
    if (!m_m7Chars) return;
    if (m_scrx >= 1280 - 32) return;
    int mcolx = m_m7Col;
    int holdoff = 0, holdclear = 0;
    const int flashx = m_m7Flash, dblx = m_m7Dbl;

    // The SAA5050 shows each character two cells after the CRTC fetches it
    const uint8_t t = m_m7Buf[0];
    m_m7Buf[0] = m_m7Buf[1];
    m_m7Buf[1] = dat;
    dat = t;
    const uint8_t* px = m_m7P;
    if (dat == 255) {
        puts(m_scrx + 16, m_scry, m_m7Width, BLACK);
        return;
    }
    if (dat < 0x20) {
        switch (dat) {
            case 1: case 2: case 3: case 4: case 5: case 6: case 7:     // alphanumeric colour
                m_m7Gfx = 0;
                m_m7Col = dat;
                m_m7P = m_m7Chars;
                holdclear = 1;
                break;
            case 8: m_m7Flash = 1; break;
            case 9: m_m7Flash = 0; break;
            case 12:
                if (m_m7Dbl) { m_m7Dbl = 0; m_m7HeldChar = 0; }
                break;
            case 13:
                if (!m_m7Dbl) { m_m7Dbl = 1; m_m7WasDbl = 1; m_m7HeldChar = 0; }
                break;
            case 17: case 18: case 19: case 20: case 21: case 22: case 23:  // graphics colour
                m_m7Gfx = 1;
                m_m7Col = dat & 7;
                m_m7P = m_m7Sep ? m_m7SepGraph : m_m7Graph;
                break;
            case 24: m_m7Col = mcolx = m_m7Bg; break;                     // conceal
            case 25: if (m_m7Gfx) m_m7P = m_m7Graph; m_m7Sep = 0; break;   // contiguous
            case 26: if (m_m7Gfx) m_m7P = m_m7SepGraph; m_m7Sep = 1; break; // separated
            case 28: m_m7Bg = 0; break;                                   // black background
            case 29: m_m7Bg = m_m7Col; break;                             // new background
            case 30: m_m7HoldChar = 1; break;                             // hold graphics
            case 31: holdoff = 1; break;                                  // release graphics
        }
        if (m_m7HoldChar) {
            dat = m_m7HeldChar;
            if (!(dat & 0x20)) dat = 0x20;
            px = m_m7HeldP;
        } else {
            dat = 0x20;
        }
        if (dblx != m_m7Dbl) dat = 0x20;        // double height doesn't keep held characters
        if (!m_m7HoldChar || !m_m7Gfx) m_m7HeldChar = 0x20;
    } else if (m_m7Gfx && (dat & 0x20)) {
        m_m7HeldChar = dat;
        m_m7HeldP = px;
    }

    const int x0 = m_scrx + 16 - X_ORIGIN;
    const int row = 2 * m_scry;
    auto line = [&](int r, int x, uint32_t c) {
        if (x >= 0 && x < WIDTH && r >= 0 && r < HEIGHT) m_canvas[(size_t)r * WIDTH + x] = c;
    };
    if (flashx && !m_m7FlashOn) {
        const uint32_t off = m_m7Lookup[0][m_m7Bg & 7][0];
        for (int c = 0; c < m_m7Width; c++) { line(row, x0 + c, off); line(row + 1, x0 + c, off); }
    } else {
        const uint8_t* ptr = px + (dat - 0x20) * m_m7BytesPerChar;
        if (dblx) ptr += (m_m7NextDbl ? ((m_sc >> 1) + 5) : (m_sc >> 1)) * m_m7Width;
        else ptr += m_sc * m_m7Width;
        const uint32_t* on = (!m_m7Dbl && m_m7NextDbl) ? m_m7Lookup[m_m7Bg & 7][m_m7Bg & 7]
                                                        : m_m7Lookup[mcolx & 7][m_m7Bg & 7];
        for (int c = 0; c < m_m7Width; c++) {
            const uint8_t w = ptr[c];
            if (dblx) {
                // Double height: each source line once, as the field b-em shows for it
                const uint32_t col = on[(m_sc & 1) ? (w >> 4) : (w & 0x0F)];
                line(row, x0 + c, col);
                line(row + 1, x0 + c, col);
            } else {
                line(row, x0 + c, on[w & 0x0F]);
                line(row + 1, x0 + c, on[w >> 4]);
            }
        }
    }
    m_scrx -= (16 - m_m7Width);
    if (holdoff) { m_m7HoldChar = 0; m_m7HeldChar = 32; }
    if (holdclear) m_m7HeldChar = 32;
}

void BbcVideo::endFrame() {
    m_frame = m_canvas;
    ++m_frames;
}

void BbcVideo::renderFrame(uint32_t* buffer) {
    std::memcpy(buffer, m_frame.data(), m_frame.size() * sizeof(uint32_t));
}

// b-em video_poll, a 2 MHz cycle at a time
void BbcVideo::clock(uint64_t clocks) {
    while (clocks--) {
        m_scrx += 8;
        m_oddClock = !m_oddClock;
        if (!(m_ulaCtrl & 0x10) && !m_oddClock) continue;      // slow clock: every other cycle

        if (m_hc == m_crtc[1]) {                                 // horizontal displayed
            m_charsLeft = (m_dispen && (m_ulaCtrl & 2)) ? 3 : 0;
            m_dispen = 0;
        }
        if (m_hc == m_crtc[2]) {                                 // horizontal sync
            m_scrx = 128 - ((m_crtc[3] & 15) * ((m_ulaCtrl & 0x10) ? 4 : 8));
            if (m_scry < 384) ++m_scry;
        }

        if (m_dispen) {
            if (!((m_ma ^ (m_crtc[15] | (m_crtc[14] << 8))) & 0x3FFF) && m_con)
                m_cdraw = CDRAW_LOOK[m_crtc[8] >> 6];
            uint8_t dat = 0;
            if (m_ram) {
                if (m_ma & 0x2000) {
                    dat = m_ram[m_ttxBank | (m_ma & 0x3FF)];
                } else {
                    uint16_t addr = ((m_crtc[8] & 3) == 3) ? (uint16_t)((m_ma << 3) | ((m_sc & 3) << 1))
                                                           : (uint16_t)((m_ma << 3) | (m_sc & 7));
                    if (addr & 0x8000) addr = (uint16_t)(addr - SCREEN_LEN[m_scrSize]);
                    dat = m_ram[addr & 0x7FFF];
                }
            }
            if (m_scrx < 1280 - 16) {
                if ((m_crtc[8] & 0x30) == 0x30 || ((m_sc & 8) && !(m_ulaCtrl & 2))) {
                    puts(m_scrx, m_scry, (m_ulaCtrl & 0x10) ? 8 : 16, BLACK);    // Modes 3 and 6 gaps
                } else {
                    switch (m_crtcMode) {
                        case TELETEXT:
                            mode7Render(dat & 0x7F);
                            break;
                        case HIFREQ:
                            for (int c = 0; c < 8; c++)
                                put(m_scrx + c, m_scry, m_ulaPal[m_table4bpp[m_ulaMode][dat][c]]);
                            break;
                        case LOFREQ:
                            for (int c = 0; c < 16; c++)
                                put(m_scrx + c, m_scry, m_ulaPal[m_table4bpp[m_ulaMode][dat][c]]);
                            break;
                    }
                }
                if (m_cdraw) {
                    if (m_cursorOn && (m_ulaCtrl & CURSOR_LOOK[m_cdraw]))
                        for (int c = (m_ulaCtrl & 0x10) ? 8 : 16; c >= 0; c--)
                            put(m_scrx + c, m_scry, get(m_scrx + c, m_scry) ^ 0x00FFFFFF);
                    if (++m_cdraw == 7) m_cdraw = 0;
                }
            }
            m_ma++;
        } else {
            if (m_charsLeft) {
                if (m_charsLeft != 1) mode7Render(255);
                m_charsLeft--;
            } else if (m_scrx < 1280 - 32) {
                puts(m_scrx, m_scry, (m_ulaCtrl & 0x10) ? 8 : 16, BLACK);
                if (m_crtcMode == TELETEXT) puts(m_scrx + 16, m_scry, 16, BLACK);
            }
            if (m_cdraw && m_scrx < 1280 - 16) {
                if (m_cursorOn && (m_ulaCtrl & CURSOR_LOOK[m_cdraw]))
                    for (int c = (m_ulaCtrl & 0x10) ? 8 : 16; c >= 0; c--)
                        put(m_scrx + c, m_scry, get(m_scrx + c, m_scry) ^ 0x00FFFFFF);
                if (++m_cdraw == 7) m_cdraw = 0;
            }
        }

        if (m_hvblCount) {
            if (--m_hvblCount == 0 && onVsync) onVsync(false);
        }

        if (m_hc == m_crtc[0]) {                                 // end of the line
            m_m7Col = 7;
            m_m7Bg = 0;
            m_m7HoldChar = 0;
            m_m7HeldChar = 0x20;
            m_m7P = m_m7Chars;
            m_m7Flash = 0;
            m_m7Sep = 0;
            m_m7Gfx = 0;
            m_m7HeldP = m_m7P;
            m_hc = 0;
            if (m_sc == (m_crtc[11] & 31) || ((m_crtc[8] & 3) == 3 && m_sc == ((m_crtc[11] & 31) >> 1))) {
                m_con = 0;
                m_coff = 1;
            }
            if (m_vadj) {
                m_sc = (m_sc + 1) & 31;
                m_ma = m_maBack;
                if (--m_vadj == 0) {
                    m_vdispen = m_crtc[6];
                    m_ma = m_maBack = (uint16_t)((m_crtc[13] | (m_crtc[12] << 8)) & 0x3FFF);
                    m_sc = 0;
                }
            } else if (m_sc == m_crtc[9] || ((m_crtc[8] & 3) == 3 && m_sc == (m_crtc[9] >> 1))) {
                // The bottom of a character row
                m_maBack = m_ma;
                m_sc = 0;
                m_con = 0;
                m_coff = 0;
                m_m7NextDbl = m_m7NextDbl ? 0 : m_m7WasDbl;
                const int oldvc = m_vc;
                m_vc = (m_vc + 1) & 127;
                if (m_vc == m_crtc[6]) m_vdispen = 0;            // vertical displayed
                if (oldvc == m_crtc[4]) {                        // vertical total
                    m_vc = 0;
                    m_vadj = m_crtc[5];
                    if (!m_vadj) {
                        m_vdispen = m_crtc[6];
                        m_ma = m_maBack = (uint16_t)((m_crtc[13] | (m_crtc[12] << 8)) & 0x3FFF);
                    }
                    m_frCount++;
                    m_cursorOn = !(m_crtc[10] & 0x60) ? 1 : (m_frCount & CMASK[(m_crtc[10] & 0x60) >> 5]);
                }
                if (m_vc == m_crtc[7]) {                         // vertical sync
                    m_frameOdd ^= 1;
                    endFrame();
                    m_scry = 0;
                    if (onVsync) onVsync(true);
                    m_vsyncTime = (m_crtc[3] >> 4) + 1;
                    if (!(m_crtc[3] >> 4)) m_vsyncTime = 17;
                    m_m7FlashTime++;
                    if ((m_m7FlashOn && m_m7FlashTime == 32) || (!m_m7FlashOn && m_m7FlashTime == 16)) {
                        m_m7FlashOn = !m_m7FlashOn;
                        m_m7FlashTime = 0;
                    }
                }
            } else {
                m_sc = (m_sc + 1) & 31;
                m_ma = m_maBack;
            }
            m_m7Dbl = m_m7WasDbl = 0;
            if ((m_sc == (m_crtc[10] & 31) || ((m_crtc[8] & 3) == 3 && m_sc == ((m_crtc[10] & 31) >> 1))) && !m_coff)
                m_con = 1;
            if (m_vsyncTime && --m_vsyncTime == 0) m_hvblCount = 1;
            m_dispen = m_vdispen;
        } else {
            m_hc = (m_hc + 1) & 255;
        }
    }
}
