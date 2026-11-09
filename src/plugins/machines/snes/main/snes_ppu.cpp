// The SNES PPU after Mesen2 (Core/SNES/SnesPpu.cpp), GPL-3.0, Sour and the
// Mesen contributors, and fullsnes / Anomie; drawn up to the dot.

#include "snes_ppu.h"

#include <algorithm>
#include <cstring>

namespace snes {

namespace {

// OBJ sizes (OBSEL bits 5-7): small, large (width, height)
const uint8_t OBJ_SIZES[8][2][2] = {
    {{8, 8}, {16, 16}}, {{8, 8}, {32, 32}}, {{8, 8}, {64, 64}}, {{16, 16}, {32, 32}},
    {{16, 16}, {64, 64}}, {{32, 32}, {64, 64}}, {{16, 32}, {32, 64}}, {{16, 32}, {32, 32}},
};

// Layer priorities by mode: z for [layer][priority] (higher is in front),
// OBJ priorities 0-3 as layer 4. 0 = the layer does not exist in the mode.
struct ZTable { uint8_t bg[4][2]; uint8_t obj[4]; };
const ZTable Z_MODE0 = {{{8, 11}, {7, 10}, {2, 5}, {1, 4}}, {3, 6, 9, 12}};
const ZTable Z_MODE1 = {{{6, 9}, {5, 8}, {1, 3}, {0, 0}}, {2, 4, 7, 10}};
const ZTable Z_MODE1_BG3 = {{{5, 8}, {4, 7}, {1, 10}, {0, 0}}, {2, 3, 6, 9}};
const ZTable Z_MODE2_5 = {{{3, 7}, {1, 5}, {0, 0}, {0, 0}}, {2, 4, 6, 8}};
const ZTable Z_MODE6 = {{{2, 5}, {0, 0}, {0, 0}, {0, 0}}, {1, 3, 4, 6}};
const ZTable Z_MODE7 = {{{3, 3}, {1, 5}, {0, 0}, {0, 0}}, {2, 4, 6, 7}};

int16_t sext13(int v) { return (int16_t)((v & 0x1000) ? (v | ~0x1FFF) : (v & 0x1FFF)); }

// Brightness as Mesen: the 5-bit component times b / 15, then to 8 bits
struct BrightLut {
    uint8_t v[16][32];
    BrightLut() {
        for (int b = 0; b < 16; ++b)
            for (int c = 0; c < 32; ++c) v[b][c] = (uint8_t)((c * b / 15) * 255 / 31);
    }
};
const BrightLut BRIGHT;

}  // namespace

uint32_t Ppu::argb(uint16_t c) {
    const auto& l = BRIGHT.v[15];
    return 0xFF000000u | (uint32_t)l[c & 31] << 16 | (uint32_t)l[(c >> 5) & 31] << 8 | l[(c >> 10) & 31];
}

Ppu::Ppu() { reset(); }

void Ppu::reset() {
    m_vram.fill(0);
    m_cgram.fill(0);
    m_oam.fill(0);
    m_forcedBlank = true;
    m_brightness = 0;
    m_objBase = 0; m_objGap = 0x1000; m_objSize = 0;
    m_oamAddr = m_oamReload = 0; m_oamPriority = false; m_oamLatch = 0;
    m_bgMode = 0; m_bg3Priority = false; m_tile16 = 0;
    m_mosaicSize = 1; m_mosaicMask = 0;
    for (int i = 0; i < 4; ++i) { m_mapBase[i] = m_charBase[i] = 0; m_mapSize[i] = 0; m_hofs[i] = m_vofs[i] = 0; }
    m_ofsLatch = m_hofsLatch = 0;
    m_m7hofs = m_m7vofs = 0;
    m_vmain = 0; m_vramAddr = 0; m_vramPrefetch = 0;
    m_m7sel = 0; m_m7latch = 0;
    for (auto& v : m_m7) v = 0;
    m_m7x = m_m7y = 0;
    m_cgAddr = 0; m_cgLatch = 0; m_cgHigh = false;
    std::memset(m_winSel, 0, sizeof m_winSel);
    std::memset(m_winPos, 0, sizeof m_winPos);
    m_winLog[0] = m_winLog[1] = 0;
    m_tm = m_ts = m_tmw = m_tsw = 0;
    m_cgwsel = m_cgadsub = 0; m_fixed = 0; m_setini = 0;
    m_hLatch = m_vLatch = 0; m_latched = false; m_hFlip = m_vFlip = false;
    m_rangeOver = m_timeOver = false;
    m_ppu1Bus = m_ppu2Bus = 0;
    m_mpy = 0; m_mpyResult = 0;
    for (auto& f : m_frame) f.assign((size_t)MAX_WIDTH * MAX_HEIGHT, 0xFF000000u);
    m_frameWide[0] = m_frameWide[1] = m_frameTall[0] = m_frameTall[1] = false;
    m_back = 0;
    m_frames = 0;
    m_interlaceField = false;
    m_overscanFrame = m_interlaceFrame = m_wideFrame = false;
    m_drawLine = -1; m_drawX = 0;
    for (auto& o : m_obj.opaque) o = false;
}

// ---------------------------------------------------------------------------
// Registers
// ---------------------------------------------------------------------------

uint16_t Ppu::vramAddrRemap() const {
    const uint16_t a = m_vramAddr;
    switch ((m_vmain >> 2) & 3) {
    case 1: return (uint16_t)((a & 0xFF00) | ((a & 0x001F) << 3) | ((a >> 5) & 7));
    case 2: return (uint16_t)((a & 0xFE00) | ((a & 0x003F) << 3) | ((a >> 6) & 7));
    case 3: return (uint16_t)((a & 0xFC00) | ((a & 0x007F) << 3) | ((a >> 7) & 7));
    default: return a;
    }
}

void Ppu::vramIncrement(bool high) {
    if (high != ((m_vmain & 0x80) != 0)) return;
    static const uint16_t STEP[4] = {1, 32, 128, 128};
    m_vramAddr = (uint16_t)(m_vramAddr + STEP[m_vmain & 3]);
}

void Ppu::write(uint8_t reg, uint8_t v) {
    switch (reg & 0x3F) {
    case 0x00:
        if (m_forcedBlank && !(v & 0x80)) { /* rendering resumes */ }
        m_forcedBlank = v & 0x80;
        m_brightness = v & 0x0F;
        break;
    case 0x01:
        m_objBase = (uint16_t)((v & 7) << 13);
        m_objGap = (uint16_t)((((v >> 3) & 3) + 1) << 12);
        m_objSize = v >> 5;
        break;
    case 0x02:
        m_oamReload = (uint16_t)((m_oamReload & 0x200) | (v << 1));
        m_oamAddr = m_oamReload;
        break;
    case 0x03:
        m_oamReload = (uint16_t)(((v & 1) << 9) | (m_oamReload & 0x1FE));
        m_oamPriority = v & 0x80;
        m_oamAddr = m_oamReload;
        break;
    case 0x04: {
        const uint16_t a = m_oamAddr & 0x3FF;
        if (a < 0x200) {
            if (!(a & 1)) m_oamLatch = v;
            else { m_oam[a - 1] = m_oamLatch; m_oam[a] = v; }
        } else {
            m_oam[0x200 | (a & 0x1F)] = v;
        }
        m_oamAddr = (uint16_t)((a + 1) & 0x3FF);
        break;
    }
    case 0x05:
        m_bgMode = v & 7;
        m_bg3Priority = v & 0x08;
        m_tile16 = v >> 4;
        break;
    case 0x06:
        m_mosaicSize = (uint8_t)((v >> 4) + 1);
        m_mosaicMask = v & 0x0F;
        break;
    case 0x07: case 0x08: case 0x09: case 0x0A:
        m_mapBase[reg - 7] = (uint16_t)((v & 0xFC) << 8);
        m_mapSize[reg - 7] = v & 3;
        break;
    case 0x0B: m_charBase[0] = (uint16_t)((v & 0x0F) << 12); m_charBase[1] = (uint16_t)((v >> 4) << 12); break;
    case 0x0C: m_charBase[2] = (uint16_t)((v & 0x0F) << 12); m_charBase[3] = (uint16_t)((v >> 4) << 12); break;
    case 0x0D: case 0x0F: case 0x11: case 0x13: {
        const int bg = (reg - 0x0D) >> 1;
        if (bg == 0) { m_m7hofs = sext13(v << 8 | m_m7latch); m_m7latch = v; }
        m_hofs[bg] = (uint16_t)(((v << 8) | (m_ofsLatch & ~7) | (m_hofsLatch & 7)) & 0x3FF);
        m_ofsLatch = v;
        m_hofsLatch = v;
        break;
    }
    case 0x0E: case 0x10: case 0x12: case 0x14: {
        const int bg = (reg - 0x0E) >> 1;
        if (bg == 0) { m_m7vofs = sext13(v << 8 | m_m7latch); m_m7latch = v; }
        m_vofs[bg] = (uint16_t)(((v << 8) | m_ofsLatch) & 0x3FF);
        m_ofsLatch = v;
        break;
    }
    case 0x15: m_vmain = v; break;
    case 0x16: m_vramAddr = (uint16_t)((m_vramAddr & 0xFF00) | v); m_vramPrefetch = m_vram[vramAddrRemap() & 0x7FFF]; break;
    case 0x17: m_vramAddr = (uint16_t)((m_vramAddr & 0x00FF) | v << 8); m_vramPrefetch = m_vram[vramAddrRemap() & 0x7FFF]; break;
    case 0x18: { auto& w = m_vram[vramAddrRemap() & 0x7FFF]; w = (uint16_t)((w & 0xFF00) | v); vramIncrement(false); break; }
    case 0x19: { auto& w = m_vram[vramAddrRemap() & 0x7FFF]; w = (uint16_t)((w & 0x00FF) | v << 8); vramIncrement(true); break; }
    case 0x1A: m_m7sel = v; break;
    case 0x1B: case 0x1C: case 0x1D: case 0x1E:
        m_m7[reg - 0x1B] = (int16_t)(v << 8 | m_m7latch);
        m_m7latch = v;
        if (reg <= 0x1C) m_mpyResult = (uint32_t)((int32_t)m_m7[0] * (int8_t)(m_m7[1] >> 8)) & 0xFFFFFF;
        break;
    case 0x1F: m_m7x = sext13(v << 8 | m_m7latch); m_m7latch = v; break;
    case 0x20: m_m7y = sext13(v << 8 | m_m7latch); m_m7latch = v; break;
    case 0x21: m_cgAddr = v; m_cgHigh = false; break;
    case 0x22:
        if (!m_cgHigh) m_cgLatch = v;
        else m_cgram[m_cgAddr++] = (uint16_t)(((v & 0x7F) << 8) | m_cgLatch);
        m_cgHigh = !m_cgHigh;
        break;
    case 0x23: m_winSel[0] = v; break;
    case 0x24: m_winSel[1] = v; break;
    case 0x25: m_winSel[2] = v; break;
    case 0x26: case 0x27: case 0x28: case 0x29: m_winPos[reg - 0x26] = v; break;
    case 0x2A: m_winLog[0] = v; break;
    case 0x2B: m_winLog[1] = v; break;
    case 0x2C: m_tm = v & 0x1F; break;
    case 0x2D: m_ts = v & 0x1F; break;
    case 0x2E: m_tmw = v & 0x1F; break;
    case 0x2F: m_tsw = v & 0x1F; break;
    case 0x30: m_cgwsel = v; break;
    case 0x31: m_cgadsub = v; break;
    case 0x32: {
        const uint16_t c = v & 0x1F;
        if (v & 0x20) m_fixed = (uint16_t)((m_fixed & ~0x001F) | c);
        if (v & 0x40) m_fixed = (uint16_t)((m_fixed & ~0x03E0) | c << 5);
        if (v & 0x80) m_fixed = (uint16_t)((m_fixed & ~0x7C00) | c << 10);
        break;
    }
    case 0x33: m_setini = v; break;
    default: break;
    }
}

uint8_t Ppu::peek(uint8_t reg) const {
    switch (reg & 0x3F) {
    case 0x34: return (uint8_t)m_mpyResult;
    case 0x35: return (uint8_t)(m_mpyResult >> 8);
    case 0x36: return (uint8_t)(m_mpyResult >> 16);
    case 0x38: { const uint16_t a = m_oamAddr & 0x3FF; return m_oam[a < 0x200 ? a : (0x200 | (a & 0x1F))]; }
    case 0x39: return (uint8_t)m_vramPrefetch;
    case 0x3A: return (uint8_t)(m_vramPrefetch >> 8);
    case 0x3B: return m_cgHigh ? (uint8_t)((m_cgram[m_cgAddr] >> 8) | (m_ppu2Bus & 0x80)) : (uint8_t)m_cgram[m_cgAddr];
    case 0x3E: return (uint8_t)((m_timeOver << 7) | (m_rangeOver << 6) | (m_ppu1Bus & 0x10) | 0x01);
    case 0x3F: return (uint8_t)((m_interlaceField << 7) | (m_latched << 6) | (m_ppu2Bus & 0x20) | 0x03);
    default: return m_ppu1Bus;
    }
}

uint8_t Ppu::read(uint8_t reg) {
    uint8_t v;
    switch (reg & 0x3F) {
    case 0x34: case 0x35: case 0x36:
        v = peek(reg);
        m_ppu1Bus = v;
        return v;
    case 0x38: {
        v = peek(reg);
        m_oamAddr = (uint16_t)((m_oamAddr + 1) & 0x3FF);
        m_ppu1Bus = v;
        return v;
    }
    case 0x39: case 0x3A: {
        v = peek(reg);
        if (((reg & 0x3F) == 0x3A) == ((m_vmain & 0x80) != 0)) {
            m_vramPrefetch = m_vram[vramAddrRemap() & 0x7FFF];
            vramIncrement((reg & 0x3F) == 0x3A);
        }
        m_ppu1Bus = v;
        return v;
    }
    case 0x3B:
        v = peek(reg);
        if (m_cgHigh) ++m_cgAddr;
        m_cgHigh = !m_cgHigh;
        m_ppu2Bus = v;
        return v;
    case 0x3C:
        v = m_hFlip ? (uint8_t)((m_hLatch >> 8 & 1) | (m_ppu2Bus & 0xFE)) : (uint8_t)m_hLatch;
        m_hFlip = !m_hFlip;
        m_ppu2Bus = v;
        return v;
    case 0x3D:
        v = m_vFlip ? (uint8_t)((m_vLatch >> 8 & 1) | (m_ppu2Bus & 0xFE)) : (uint8_t)m_vLatch;
        m_vFlip = !m_vFlip;
        m_ppu2Bus = v;
        return v;
    case 0x3E:
        v = peek(reg);
        m_ppu1Bus = v;
        return v;
    case 0x3F:
        v = peek(reg);
        m_latched = false;
        m_hFlip = m_vFlip = false;
        m_ppu2Bus = v;
        return v;
    default:
        return m_ppu1Bus;
    }
}

void Ppu::startFrame() {
    m_interlaceField = !m_interlaceField;
    m_overscanFrame = m_setini & 0x04;
    m_interlaceFrame = m_setini & 0x01;
    m_wideFrame = doubleWidth() || m_interlaceFrame;
    m_rangeOver = m_timeOver = false;
    // An interlaced frame draws one field: the other stays from the last frame
    if (m_interlaceFrame) m_frame[m_back] = m_frame[m_back ^ 1];
}

void Ppu::startVblank() {
    if (!m_forcedBlank) m_oamAddr = m_oamReload;
    m_frameWide[m_back] = m_wideFrame;
    m_frameTall[m_back] = m_overscanFrame;
    m_back ^= 1;
    ++m_frames;
}

void Ppu::sync(int line, int dot) {
    if (line != m_drawLine || m_drawX > 255) return;
    const int end = std::min(dot - 22, 255);
    if (end < m_drawX) return;
    drawPixels(line, m_drawX, end);
    m_drawX = end + 1;
}

void Ppu::endLine(int line) {
    if (line == m_drawLine && m_drawX <= 255) drawPixels(line, m_drawX, 255);
    m_drawLine = line + 1;
    m_drawX = 0;
    if (m_drawLine >= vblankLine()) { m_drawLine = -1; return; }
    if (m_forcedBlank || !((m_tm | m_ts) & 0x10)) { m_obj.opaque.fill(false); return; }
    renderObj(m_drawLine, m_obj, m_objPal);
}

IVideoOutput::VideoDimensions Ppu::getDimensions() const {
    const int f = m_back ^ 1;
    const int w = m_frameWide[f] ? MAX_WIDTH : WIDTH;
    const int h = (m_frameTall[f] ? 239 : HEIGHT) * (m_frameWide[f] ? 2 : 1);
    return {w, h, w, h};
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

uint16_t Ppu::paletteColor(int bpp, int bg, int pal, int idx) const {
    switch (bpp) {
    case 2: return m_cgram[(m_bgMode == 0 ? bg * 32 : 0) + pal * 4 + idx];
    case 4: return m_cgram[pal * 16 + idx];
    default:
        if (m_cgwsel & 1) {
            // Direct colour: R = D0-2 / P0, G = D3-5 / P1, B = D6-7 / P2
            const int r = (idx & 7) << 2 | (pal & 1) << 1;
            const int g = ((idx >> 3) & 7) << 2 | (pal & 2);
            const int b = ((idx >> 6) & 3) << 3 | (pal & 4);
            return (uint16_t)(r | g << 5 | b << 10);
        }
        return m_cgram[idx];
    }
}

void Ppu::renderBg(int bg, int bpp, int line, Layer& out, int x0, int x1, bool even) {
    const bool hires = m_bgMode == 5 || m_bgMode == 6;
    const bool t16 = (m_tile16 >> bg) & 1;
    const int tw = (t16 || hires) ? 16 : 8, th = t16 ? 16 : 8;
    const bool mosaic = (m_mosaicMask >> bg) & 1;
    const int msz = mosaic ? m_mosaicSize : 1;
    // The BG row is the line counter (the first visible line is 1) plus VOFS;
    // interlaced modes 5/6 draw the field's rows of a 448-row picture
    int y = line - ((line - 1) % msz);
    if (hires && (m_setini & 1)) y = y * 2 + (mosaic ? 0 : (int)m_interlaceField);
    const bool opt = m_bgMode == 2 || m_bgMode == 4 || m_bgMode == 6;
    const int wordsPerTile = bpp * 4;
    auto mapEntry = [&](int b, int tileX, int tileY) -> uint16_t {
        const int tx = tileX & 63, ty = tileY & 63;
        uint16_t a = (uint16_t)(m_mapBase[b] + ((ty & 31) << 5) + (tx & 31));
        if ((tx & 32) && (m_mapSize[b] & 1)) a = (uint16_t)(a + 0x400);
        if ((ty & 32) && (m_mapSize[b] & 2)) a = (uint16_t)(a + ((m_mapSize[b] & 1) ? 0x800 : 0x400));
        return m_vram[a & 0x7FFF];
    };
    for (int x = x0; x <= x1; ++x) {
        out.opaque[x] = false;
        const int xm = x - (x % msz);
        int hofs = m_hofs[bg], vofs = m_vofs[bg];
        if (opt && bg < 2) {
            // Offset-per-tile: BG3's map row gives each column's offsets
            const int col = (xm + (hofs & 7)) >> 3;
            if (col > 0) {
                const int optX = ((col - 1) << 3) + (m_hofs[2] & ~7);
                const int tx = optX >> 3, ty = m_vofs[2] >> 3;
                const uint16_t enable = (uint16_t)(0x2000 << bg);
                if (m_bgMode == 4) {
                    const uint16_t e = mapEntry(2, tx, ty);
                    if (e & enable) {
                        if (e & 0x8000) vofs = e & 0x3FF;
                        else hofs = (hofs & 7) | (e & 0x3F8);
                    }
                } else {
                    const uint16_t eh = mapEntry(2, tx, ty), ev = mapEntry(2, tx, ty + 1);
                    if (eh & enable) hofs = (hofs & 7) | (eh & 0x3F8);
                    if (ev & enable) vofs = ev & 0x3FF;
                }
            }
        }
        const int px = (hires ? (xm * 2 + (even ? 0 : 1)) + hofs * 2 : xm + hofs);
        const int py = y + vofs;
        const int tileX = px / tw, tileY = py / th;
        const uint16_t e = mapEntry(bg, tileX, tileY);
        int tile = e & 0x3FF;
        const int pal = (e >> 10) & 7;
        int fx = px % tw, fy = py % th;
        if (e & 0x4000) fx = tw - 1 - fx;
        if (e & 0x8000) fy = th - 1 - fy;
        if (fx >= 8) tile += 1;
        if (fy >= 8) tile += 16;
        fx &= 7; fy &= 7;
        const uint16_t addr = (uint16_t)(m_charBase[bg] + tile * wordsPerTile + fy);
        int idx = 0;
        for (int p = 0; p < bpp / 2; ++p) {
            const uint16_t w = m_vram[(addr + p * 8) & 0x7FFF];
            idx |= ((w >> (7 - fx)) & 1) << (2 * p) | ((w >> (15 - fx)) & 1) << (2 * p + 1);
        }
        if (!idx) continue;
        out.opaque[x] = true;
        out.prio[x] = (uint8_t)((e >> 13) & 1);
        out.color[x] = paletteColor(bpp, bg, pal, idx);
    }
}

void Ppu::renderMode7(int bg, int line, Layer& out, int x0, int x1) {
    const bool mosaic = (m_mosaicMask >> bg) & 1;
    const int msz = mosaic ? m_mosaicSize : 1;
    int y = line;
    y = y - ((y - 1) % msz);
    if (m_m7sel & 0x02) y = 255 - y;
    const int a = m_m7[0], b = m_m7[1], c = m_m7[2], d = m_m7[3];
    const int cx = m_m7x, cy = m_m7y;
    auto clip = [](int v) { return (v & 0x2000) ? (v | ~0x3FF) : (v & 0x3FF); };
    const int hoff = clip(m_m7hofs - cx), voff = clip(m_m7vofs - cy);
    const int ox = ((a * hoff) & ~63) + ((b * voff) & ~63) + ((b * y) & ~63) + (cx << 8);
    const int oy = ((c * hoff) & ~63) + ((d * voff) & ~63) + ((d * y) & ~63) + (cy << 8);
    const int over = m_m7sel >> 6;
    for (int x = x0; x <= x1; ++x) {
        out.opaque[x] = false;
        int sx = x - (x % msz);
        if (m_m7sel & 0x01) sx = 255 - sx;
        int tx = (ox + a * sx) >> 8, ty = (oy + c * sx) >> 8;
        bool outside = (tx | ty) & ~0x3FF;
        uint8_t tile;
        if (outside && over == 2) continue;                 // transparent
        if (outside && over == 3) tile = 0;
        else { tx &= 0x3FF; ty &= 0x3FF; tile = (uint8_t)m_vram[((ty >> 3) << 7) + (tx >> 3)]; }
        const uint8_t idx = (uint8_t)(m_vram[(tile << 6) + ((ty & 7) << 3) + (tx & 7)] >> 8);
        if (bg == 1) {                                       // EXTBG: 7 bits of colour, bit 7 the priority
            if (!(idx & 0x7F)) continue;
            out.opaque[x] = true;
            out.prio[x] = idx >> 7;
            out.color[x] = m_cgram[idx & 0x7F];
            continue;
        }
        if (!idx) continue;
        out.opaque[x] = true;
        out.prio[x] = 0;
        out.color[x] = paletteColor(8, 0, 0, idx);
    }
}

void Ppu::renderObj(int line, Layer& out, std::array<uint8_t, 256>& pal) {
    for (int x = 0; x < 256; ++x) out.opaque[x] = false;
    const int y = line - 1;
    const bool objInterlace = m_setini & 0x02;   // sprites at half height, the field's rows
    const int first = m_oamPriority ? ((m_oamAddr >> 2) & 0x7F) : 0;
    int list[32];
    int n = 0;
    for (int k = 0; k < 128; ++k) {
        const int i = (first + k) & 0x7F;
        const uint8_t hi = m_oam[0x200 + (i >> 2)] >> ((i & 3) * 2);
        const bool large = hi & 2;
        const int w = OBJ_SIZES[m_objSize][large][0], h = OBJ_SIZES[m_objSize][large][1];
        int x = m_oam[i * 4] | ((hi & 1) << 8);
        if (x >= 256) x -= 512;
        if (x <= -w) continue;
        const uint8_t sy = m_oam[i * 4 + 1];
        if (((y - sy) & 0xFF) >= (objInterlace ? h / 2 : h)) continue;
        if (n == 32) { m_rangeOver = true; break; }
        list[n++] = i;
    }
    int slivers = 0;
    for (int k = n - 1; k >= 0; --k) {
        const int i = list[k];
        const uint8_t hi = m_oam[0x200 + (i >> 2)] >> ((i & 3) * 2);
        const bool large = hi & 2;
        const int w = OBJ_SIZES[m_objSize][large][0], h = OBJ_SIZES[m_objSize][large][1];
        int x = m_oam[i * 4] | ((hi & 1) << 8);
        if (x >= 256) x -= 512;
        const uint8_t attr = m_oam[i * 4 + 3];
        const int tile = m_oam[i * 4 + 2] | ((attr & 1) << 8);
        int dy = (y - m_oam[i * 4 + 1]) & 0xFF;
        if (objInterlace) dy = dy * 2 + (int)m_interlaceField;
        if (attr & 0x80) dy = h - 1 - dy;
        const int p = (attr >> 1) & 7, prio = (attr >> 4) & 3;
        for (int col = 0; col < w / 8; ++col) {
            const int sx = x + col * 8;
            if (sx + 7 < 0 || sx > 255) continue;
            if (++slivers > 34) { m_timeOver = true; continue; }
            const int tcol = (attr & 0x40) ? (w / 8 - 1 - col) : col;
            const int base = tile & 0xFF;
            const int t8 = ((((base >> 4) + (dy >> 3)) & 0x0F) << 4) | ((base + tcol) & 0x0F);
            const uint16_t addr = (uint16_t)(m_objBase + ((tile & 0x100) ? m_objGap : 0) + t8 * 16 + (dy & 7));
            const uint16_t w0 = m_vram[addr & 0x7FFF], w1 = m_vram[(addr + 8) & 0x7FFF];
            for (int px = 0; px < 8; ++px) {
                const int sxp = sx + px;
                if (sxp < 0 || sxp > 255) continue;
                const int fx = (attr & 0x40) ? 7 - px : px;
                const int idx = ((w0 >> (7 - fx)) & 1) | ((w0 >> (15 - fx)) & 1) << 1 |
                                ((w1 >> (7 - fx)) & 1) << 2 | ((w1 >> (15 - fx)) & 1) << 3;
                if (!idx) continue;
                out.opaque[sxp] = true;
                out.prio[sxp] = (uint8_t)prio;
                out.color[sxp] = m_cgram[128 + p * 16 + idx];
                pal[sxp] = (uint8_t)p;
            }
        }
    }
}

bool Ppu::window(int layer, int x) const {
    uint8_t sel, logic;
    if (layer < 4) { sel = (uint8_t)(m_winSel[layer >> 1] >> ((layer & 1) * 4)); logic = (uint8_t)((m_winLog[0] >> (layer * 2)) & 3); }
    else if (layer == 4) { sel = m_winSel[2]; logic = m_winLog[1] & 3; }
    else { sel = (uint8_t)(m_winSel[2] >> 4); logic = (m_winLog[1] >> 2) & 3; }
    const bool e1 = sel & 2, e2 = sel & 8;
    if (!e1 && !e2) return false;
    const bool w1 = (x >= m_winPos[0] && x <= m_winPos[1]) != ((sel & 1) != 0);
    const bool w2 = (x >= m_winPos[2] && x <= m_winPos[3]) != ((sel & 4) != 0);
    if (e1 && !e2) return w1;
    if (e2 && !e1) return w2;
    switch (logic) {
    case 0: return w1 || w2;
    case 1: return w1 && w2;
    case 2: return w1 != w2;
    default: return w1 == w2;
    }
}

// One pixel's colour math, after Mesen's ApplyColorMathToPixel: `a` the
// pixel, `b` the sub screen's (or, for the hi-res even pixel, the main
// screen's previous one); x picks the flags.
void Ppu::mathPixel(uint16_t& a, uint16_t b, int x, bool cw) {
    int halfShift = (m_cgadsub & 0x40) ? 1 : 0;
    switch (m_cgwsel >> 6) {                        // clip to black
    case 1: if (!cw) { a = 0; halfShift = 0; } break;
    case 2: if (cw) { a = 0; halfShift = 0; } break;
    case 3: a = 0; break;
    default: break;
    }
    if (!(m_mainFlags[x] & 1)) return;
    switch ((m_cgwsel >> 4) & 3) {                  // prevent colour math
    case 1: if (!cw) return; break;
    case 2: if (cw) return; break;
    case 3: return;
    default: break;
    }
    uint16_t other = m_fixed;
    if (m_cgwsel & 0x02) {
        if (m_subHas[x]) other = b;
        else halfShift = 0;
    }
    int r = a & 31, g = (a >> 5) & 31, bl = (a >> 10) & 31;
    const int r2 = other & 31, g2 = (other >> 5) & 31, b2 = (other >> 10) & 31;
    if (m_cgadsub & 0x80) {
        r = std::max(r - r2, 0) >> halfShift; g = std::max(g - g2, 0) >> halfShift; bl = std::max(bl - b2, 0) >> halfShift;
    } else {
        r = std::min((r + r2) >> halfShift, 31); g = std::min((g + g2) >> halfShift, 31); bl = std::min((bl + b2) >> halfShift, 31);
    }
    a = (uint16_t)(r | g << 5 | bl << 10);
}

void Ppu::drawPixels(int line, int x0, int x1) {
    if (line < 1 || line >= vblankLine()) return;
    const bool wide = doubleWidth();
    m_wideFrame = m_wideFrame || wide || (m_setini & 1);
    const bool interlaced = m_setini & 1;
    const int row = (line - 1) * 2;
    uint32_t* rowA = &m_frame[m_back][(size_t)(row + (interlaced ? (int)m_interlaceField : 0)) * MAX_WIDTH];
    uint32_t* rowB = interlaced ? nullptr : rowA + MAX_WIDTH;
    if (m_forcedBlank) {
        for (int x = x0; x <= x1; ++x) {
            m_main[x] = m_sub[x] = 0; m_mainFlags[x] = 0;
            rowA[x * 2] = rowA[x * 2 + 1] = 0xFF000000u;
            if (rowB) rowB[x * 2] = rowB[x * 2 + 1] = 0xFF000000u;
        }
        return;
    }
    static Layer layers[4], evenLayers[4];
    bool have[5] = {}, haveEven[4] = {};
    const ZTable* z;
    switch (m_bgMode) {
    case 0: z = &Z_MODE0; break;
    case 1: z = m_bg3Priority ? &Z_MODE1_BG3 : &Z_MODE1; break;
    case 6: z = &Z_MODE6; break;
    case 7: z = &Z_MODE7; break;
    default: z = &Z_MODE2_5; break;
    }
    static const int BPP[8][4] = {{2, 2, 2, 2}, {4, 4, 2, 0}, {4, 4, 0, 0}, {8, 4, 0, 0}, {8, 2, 0, 0}, {4, 2, 0, 0}, {4, 0, 0, 0}, {8, 0, 0, 0}};
    const bool hiresBg = m_bgMode == 5 || m_bgMode == 6;
    const uint8_t used = m_tm | m_ts;
    for (int bg = 0; bg < 4; ++bg) {
        if (!(used & (1 << bg))) continue;
        if (m_bgMode == 7) {
            if (bg == 0 || (bg == 1 && (m_setini & 0x40))) { renderMode7(bg, line, layers[bg], x0, x1); have[bg] = true; }
            continue;
        }
        if (!BPP[m_bgMode][bg]) continue;
        renderBg(bg, BPP[m_bgMode][bg], line, layers[bg], x0, x1, false);
        have[bg] = true;
        if (hiresBg && (m_ts & (1 << bg))) { renderBg(bg, BPP[m_bgMode][bg], line, evenLayers[bg], x0, x1, true); haveEven[bg] = true; }
    }
    have[4] = (used & 0x10) != 0;

    auto layerAt = [&](int l, bool even) -> const Layer& {
        if (l == 4) return m_obj;
        return (even && hiresBg) ? evenLayers[l] : layers[l];
    };
    const uint16_t backdrop = m_cgram[0];
    // The sub screen matters for hi-res and for colour math with it; colour
    // math matters when a layer takes part or the clip-to-black is on
    const bool mathUsed = (m_cgadsub & 0x3F) || (m_cgwsel & 0xC0);
    const bool needSub = wide || (mathUsed && (m_cgwsel & 0x02));
    for (int x = x0; x <= x1; ++x) {
        auto pick = [&](uint8_t enabled, uint8_t winMask, bool even) {
            Pixel best;
            for (int l = 0; l < 5; ++l) {
                if (!(enabled & (1 << l))) continue;
                if (l < 4 ? !(even && hiresBg ? haveEven[l] : have[l]) : !have[4]) continue;
                const Layer& ly = layerAt(l, even);
                if (!ly.opaque[x]) continue;
                if ((winMask & (1 << l)) && window(l, x)) continue;
                const uint8_t zz = l < 4 ? z->bg[l][ly.prio[x]] : z->obj[ly.prio[x]];
                if (zz > best.z) {
                    best.z = zz;
                    best.color = ly.color[x];
                    best.layer = (uint8_t)l;
                    best.objMath = l != 4 || m_objPal[x] >= 4;
                }
            }
            return best;
        };
        const Pixel main = pick(m_tm, m_tmw, false);
        m_main[x] = main.z ? main.color : backdrop;
        m_mainFlags[x] = (uint8_t)(((m_cgadsub >> main.layer) & 1) && main.objMath);
        if (needSub) {
            const Pixel sub = pick(m_ts, m_tsw, true);
            m_sub[x] = sub.z ? sub.color : backdrop;
            m_subHas[x] = sub.z != 0;
        } else {
            m_sub[x] = backdrop;
            m_subHas[x] = false;
        }
    }
    // Colour math; in hi-res the even pixel is the sub screen's, mixed with the main screen's previous pixel
    if (mathUsed) {
        for (int x = x0; x <= x1; ++x) {
            const bool cw = window(5, x);
            if (wide) {
                const uint16_t subPixel = m_sub[x];
                const int px = x > 0 ? x - 1 : 0;
                mathPixel(m_sub[x], x > 0 ? m_main[x - 1] : 0, px, cw);
                mathPixel(m_main[x], subPixel, x, cw);
            } else {
                mathPixel(m_main[x], m_sub[x], x, cw);
            }
        }
    }
    const auto& bl = BRIGHT.v[m_brightness];
    auto toArgb = [&](uint16_t c) { return 0xFF000000u | (uint32_t)bl[c & 31] << 16 | (uint32_t)bl[(c >> 5) & 31] << 8 | bl[(c >> 10) & 31]; };
    for (int x = x0; x <= x1; ++x) {
        const uint32_t m = toArgb(m_main[x]);
        const uint32_t e = wide ? toArgb(m_sub[x]) : m;
        rowA[x * 2] = e; rowA[x * 2 + 1] = m;
        if (rowB) { rowB[x * 2] = e; rowB[x * 2 + 1] = m; }
    }
}

void Ppu::renderFrame(uint32_t* buffer) {
    const int f = m_back ^ 1;
    const auto& src = m_frame[f];
    const int lines = m_frameTall[f] ? 239 : HEIGHT;
    if (m_frameWide[f]) {
        std::copy(src.begin(), src.begin() + (size_t)MAX_WIDTH * lines * 2, buffer);
        return;
    }
    for (int y = 0; y < lines; ++y)
        for (int x = 0; x < WIDTH; ++x) buffer[y * WIDTH + x] = src[(size_t)y * 2 * MAX_WIDTH + x * 2 + 1];
}

}  // namespace snes
