// NES PPU (2C02) after Mesen2 (Core/NES/NesPpu.cpp, BaseNesPpu.cpp,
// DefaultNesPpu.h, NesDefaultVideoFilter.cpp), GPL-3.0, Sour and the Mesen
// contributors; the nesdev wiki for the rendering and scrolling details.

#include "nes_ppu.h"
#include "nes_cart.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace nes {

namespace {

// Mesen NesDefaultVideoFilter: the 2C02 palette
const uint32_t PALETTE[64] = {
    0xFF666666, 0xFF002A88, 0xFF1412A7, 0xFF3B00A4, 0xFF5C007E, 0xFF6E0040, 0xFF6C0600, 0xFF561D00,
    0xFF333500, 0xFF0B4800, 0xFF005200, 0xFF004F08, 0xFF00404D, 0xFF000000, 0xFF000000, 0xFF000000,
    0xFFADADAD, 0xFF155FD9, 0xFF4240FF, 0xFF7527FE, 0xFFA01ACC, 0xFFB71E7B, 0xFFB53120, 0xFF994E00,
    0xFF6B6D00, 0xFF388700, 0xFF0C9300, 0xFF008F32, 0xFF007C8D, 0xFF000000, 0xFF000000, 0xFF000000,
    0xFFFFFEFF, 0xFF64B0FF, 0xFF9290FF, 0xFFC676FF, 0xFFF36AFF, 0xFFFE6ECC, 0xFFFE8170, 0xFFEA9E22,
    0xFFBCBE00, 0xFF88D800, 0xFF5CE430, 0xFF45E082, 0xFF48CDDE, 0xFF4F4F4F, 0xFF000000, 0xFF000000,
    0xFFFFFEFF, 0xFFC0DFFF, 0xFFD3D2FF, 0xFFE8C8FF, 0xFFFBC2FF, 0xFFFEC4EA, 0xFFFECCC5, 0xFFF7D8A5,
    0xFFE4E594, 0xFFCFEF96, 0xFFBDF4AB, 0xFFB3F3CC, 0xFFB5EBF2, 0xFFB8B8B8, 0xFF000000, 0xFF000000,
};

constexpr int NMI_LINE = 241;
constexpr int VBLANK_END = 260;

}  // namespace

uint32_t Ppu::color(uint8_t c, uint8_t emph) {
    const uint32_t argb = PALETTE[c & 0x3F];
    if (!(emph & 7) || (c & 0x0F) >= 0x0E) return argb;
    // Emphasis dims the colour channels that are not emphasised
    double r = (argb >> 16) & 0xFF, g = (argb >> 8) & 0xFF, b = argb & 0xFF;
    constexpr double DIM = 0.816328;
    if (!(emph & 1)) r *= DIM;      // red   ($2001 bit 5)
    if (!(emph & 2)) g *= DIM;      // green (bit 6)
    if (!(emph & 4)) b *= DIM;      // blue  (bit 7)
    if ((emph & 7) == 7) { r *= DIM; g *= DIM; b *= DIM; }
    return 0xFF000000u | ((uint32_t)std::lround(r) << 16) | ((uint32_t)std::lround(g) << 8) | (uint32_t)std::lround(b);
}

Ppu::Ppu() { reset(); }

void Ppu::reset() {
    m_v = m_t = 0;
    m_fineX = 0;
    m_w = false;
    m_openBus = 0;
    for (auto& t : m_openBusStamp) t = 0;
    m_readBuffer = 0;
    m_oamAddr = 0;
    m_vramInc32 = m_sprite16 = m_nmiEnable = false;
    m_spritePatterns = m_bgPatterns = 0;
    m_mask = 0;
    m_grayscale = m_bgLeft = m_spritesLeft = m_bgEnabled = m_spritesEnabled = false;
    m_vblank = m_sprite0Hit = m_overflow = false;
    m_preventVblank = false;
    m_renderingNow = false;
    m_renderingPrev = false;
    m_pendingIncrement = false;
    m_ignoreVramRead = 0;
    m_vDelay = 0;
    m_vPending = 0;
    m_scanline = -1;
    m_cycle = 0;
    m_frameCount = 0;
    m_tile = {};
    m_lowShift = m_highShift = 0;
    m_curPalette = m_prevPalette = 0;
    m_oam.fill(0);
    m_secondary.fill(0xFF);
    m_sprites = {};
    m_spriteCount = m_spriteIndex = 0;
    m_sprite0Next = m_sprite0Line = false;
    m_hasSprite.fill(false);
    // Power-on palette (Mesen BaseNesPpu: the values a 2C02 commonly shows)
    static const uint8_t pal[32] = {0x09, 0x01, 0x00, 0x01, 0x00, 0x02, 0x02, 0x0D, 0x08, 0x10, 0x08, 0x24, 0x00, 0x00, 0x04, 0x2C,
                                    0x09, 0x01, 0x34, 0x03, 0x00, 0x04, 0x00, 0x14, 0x08, 0x3A, 0x00, 0x02, 0x00, 0x20, 0x2C, 0x08};
    std::copy(pal, pal + 32, m_palette.begin());
    for (auto& b : m_buffers) b.fill(0x0F);
    m_back = 0;
    m_front = 1;
    m_dots = 0;
}

void Ppu::softReset() {
    m_w = false;
    m_readBuffer = 0;
    m_t = 0;
    m_fineX = 0;
    write(0, 0x00);
    write(1, 0x00);
    m_renderingNow = m_renderingPrev = false;
    m_vDelay = 0;
    m_pendingIncrement = false;
    m_ignoreVramRead = 0;
    m_scanline = -1;
    m_cycle = 0;
    m_frameCount = 0;
    m_spriteCount = m_lineSprites = 0;
}

void Ppu::setOpenBus(uint8_t mask, uint8_t value) {
    for (int i = 0; i < 8; ++i) {
        const uint8_t bit = (uint8_t)(1u << i);
        if (mask & bit) {
            m_openBus = (uint8_t)((m_openBus & ~bit) | (value & bit));
            m_openBusStamp[i] = m_frameCount;
        } else if (m_frameCount - m_openBusStamp[i] > 3) {
            m_openBus &= (uint8_t)~bit;          // decays to 0 after 3 frames unrefreshed
        }
    }
}

// ---------------------------------------------------------------------------
// The PPU bus
// ---------------------------------------------------------------------------

void Ppu::setBusAddress(uint16_t addr) {
    if (m_cart) m_cart->ppuAddress(addr, m_dots);
}

uint8_t Ppu::readVram(uint16_t addr) {
    addr &= 0x3FFF;
    setBusAddress(addr);
    return m_cart ? m_cart->ppuRead(addr) : 0;
}

void Ppu::writeVram(uint16_t addr, uint8_t value) {
    addr &= 0x3FFF;
    setBusAddress(addr);
    if (m_cart) m_cart->ppuWrite(addr, value);
}

uint8_t Ppu::peekVram(uint16_t addr) const {
    addr &= 0x3FFF;
    if (addr >= 0x3F00) return readPalette((uint8_t)addr);
    return m_cart ? m_cart->ppuRead(addr) : 0;
}

void Ppu::pokeVram(uint16_t addr, uint8_t value) {
    addr &= 0x3FFF;
    if (addr >= 0x3F00) writePalette((uint8_t)addr, value);
    else if (m_cart) m_cart->ppuWrite(addr, value);
}

uint8_t Ppu::readPalette(uint8_t addr) const {
    addr &= 0x1F;
    if ((addr & 0x13) == 0x10) addr &= ~0x10;      // $3F10/14/18/1C mirror $3F00/04/08/0C
    return m_palette[addr];
}

void Ppu::writePalette(uint8_t addr, uint8_t value) {
    addr &= 0x1F;
    value &= 0x3F;
    if ((addr & 0x03) == 0) { m_palette[addr & 0x0F] = value; m_palette[(addr & 0x0F) | 0x10] = value; }
    else m_palette[addr] = value;
}

// ---------------------------------------------------------------------------
// Registers
// ---------------------------------------------------------------------------

uint8_t Ppu::read(uint8_t reg) {
    uint8_t openMask = 0xFF, value = 0;
    switch (reg & 7) {
    case 2:
        m_w = false;
        value = (uint8_t)((m_overflow << 5) | (m_sprite0Hit << 6) | (m_vblank << 7));
        m_vblank = false;
        // "Reading one PPU clock before reads it as clear and never sets the flag or generates NMI for that frame"
        if (m_scanline == NMI_LINE && m_cycle == 0) m_preventVblank = true;
        openMask = 0x1F;
        break;
    case 4:
        if (m_scanline <= 239 && m_renderingNow) {
            // During rendering: the byte the sprite logic is moving
            if (m_cycle >= 257 && m_cycle <= 320) {
                const int step = std::min(3, (m_cycle - 257) % 8);
                value = m_secondary[((m_cycle - 257) / 8 * 4 + step) & 0x1F];
            } else if (m_cycle >= 1 && m_cycle <= 64) {
                value = 0xFF;
            } else {
                value = m_oam[m_oamAddr];
            }
        } else {
            value = m_oam[m_oamAddr];
        }
        openMask = 0x00;
        break;
    case 7:
        if (m_ignoreVramRead) {
            // A second read within a couple of CPU cycles (a DMA's halt read,
            // then the CPU's) is ignored: open bus, nothing advances
            openMask = 0xFF;
            break;
        }
        m_ignoreVramRead = 6;
        value = m_readBuffer;
        m_readBuffer = readVram(m_v & 0x3FFF);
        if ((m_v & 0x3FFF) >= 0x3F00) {
            // Palette reads are immediate (the buffer gets the nametable byte underneath)
            value = (uint8_t)((readPalette((uint8_t)m_v) & (m_grayscale ? 0x30 : 0x3F)) | (m_openBus & 0xC0));
            openMask = 0xC0;
        } else {
            openMask = 0x00;
        }
        m_pendingIncrement = true;
        break;
    default:
        break;
    }
    // Mesen ApplyOpenBus: the driven bits refresh the latch, the rest may decay
    setOpenBus((uint8_t)~openMask, value);
    return (uint8_t)(value | (m_openBus & openMask));
}

uint8_t Ppu::peek(uint8_t reg) const {
    switch (reg & 7) {
    case 2: return (uint8_t)((m_overflow << 5) | (m_sprite0Hit << 6) | (m_vblank << 7) | (m_openBus & 0x1F));
    case 4: return m_oam[m_oamAddr];
    case 7:
        if ((m_v & 0x3FFF) >= 0x3F00) return (uint8_t)(readPalette((uint8_t)m_v) | (m_openBus & 0xC0));
        return m_readBuffer;
    default: return m_openBus;
    }
}

void Ppu::write(uint8_t reg, uint8_t value) {
    setOpenBus(0xFF, value);
    switch (reg & 7) {
    case 0:
        m_t = (uint16_t)((m_t & ~0x0C00) | ((value & 0x03) << 10));
        m_vramInc32 = value & 0x04;
        m_spritePatterns = (value & 0x08) ? 0x1000 : 0x0000;
        m_bgPatterns = (value & 0x10) ? 0x1000 : 0x0000;
        m_sprite16 = value & 0x20;
        m_nmiEnable = value & 0x80;
        break;
    case 1:
        m_mask = value;
        m_grayscale = value & 0x01;
        m_bgLeft = value & 0x02;
        m_spritesLeft = value & 0x04;
        m_bgEnabled = value & 0x08;
        m_spritesEnabled = value & 0x10;
        break;
    case 3:
        m_oamAddr = value;
        break;
    case 4:
        if (m_scanline >= 240 || !m_renderingNow) {
            // Byte 2's unused bits 2-4 do not exist
            if ((m_oamAddr & 0x03) == 0x02) value &= 0xE3;
            m_oam[m_oamAddr++] = value;
        } else {
            // During rendering: no write, a glitchy increment of the high 6 bits
            m_oamAddr = (uint8_t)(m_oamAddr + 4);
        }
        break;
    case 5:
        if (m_w) m_t = (uint16_t)((m_t & ~0x73E0) | ((value & 0xF8) << 2) | ((value & 0x07) << 12));
        else {
            m_fineX = value & 0x07;
            m_t = (uint16_t)((m_t & ~0x001F) | (value >> 3));
        }
        m_w = !m_w;
        break;
    case 6:
        if (m_w) {
            m_t = (uint16_t)((m_t & ~0x00FF) | value);
            // v follows 3 dots later (Mesen, after Visual NES)
            m_vDelay = 3;
            m_vPending = m_t;
        } else {
            m_t = (uint16_t)((m_t & ~0xFF00) | ((value & 0x3F) << 8));
        }
        m_w = !m_w;
        break;
    case 7:
        if ((m_v & 0x3FFF) >= 0x3F00) writePalette((uint8_t)m_v, value);
        else if (m_scanline >= 240 || !m_renderingNow) writeVram(m_v & 0x3FFF, value);
        else writeVram(m_v & 0x3FFF, (uint8_t)(m_v & 0xFF));   // during rendering: the address' low byte
        m_pendingIncrement = true;
        break;
    }
}

// ---------------------------------------------------------------------------
// Scrolling (nesdev "PPU scrolling": wrapping around)
// ---------------------------------------------------------------------------

void Ppu::incX() {
    if ((m_v & 0x001F) == 31) m_v = (uint16_t)((m_v & ~0x001F) ^ 0x0400);
    else ++m_v;
}

void Ppu::incY() {
    if ((m_v & 0x7000) != 0x7000) { m_v += 0x1000; return; }
    m_v &= ~0x7000;
    int y = (m_v & 0x03E0) >> 5;
    if (y == 29) { y = 0; m_v ^= 0x0800; }
    else if (y == 31) y = 0;
    else ++y;
    m_v = (uint16_t)((m_v & ~0x03E0) | (y << 5));
}

void Ppu::vramIncrement() {
    if (m_scanline >= 240 || !m_renderingNow) {
        m_v = (uint16_t)((m_v + (m_vramInc32 ? 32 : 1)) & 0x7FFF);
        setBusAddress(m_v & 0x3FFF);           // MMC3 sees A12 rise on $2007 accesses
    } else {
        // During rendering: a coarse X and a Y increment at once
        incX();
        incY();
    }
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void Ppu::loadTile() {
    if (!m_renderingNow) return;
    switch (m_cycle & 7) {
    case 1: {
        m_prevPalette = m_curPalette;
        m_curPalette = m_tile.palette;
        m_lowShift |= m_tile.low;
        m_highShift |= m_tile.high;
        const uint8_t tile = readVram((uint16_t)(0x2000 | (m_v & 0x0FFF)));
        m_tile.addr = (uint16_t)((tile << 4) | (m_v >> 12) | m_bgPatterns);
        break;
    }
    case 3: {
        const uint16_t at = (uint16_t)(0x23C0 | (m_v & 0x0C00) | ((m_v >> 4) & 0x38) | ((m_v >> 2) & 0x07));
        const uint8_t shift = (uint8_t)(((m_v >> 4) & 0x04) | (m_v & 0x02));
        m_tile.palette = (uint8_t)(((readVram(at) >> shift) & 0x03) << 2);
        break;
    }
    case 5: m_tile.low = readVram(m_tile.addr); break;
    case 7: m_tile.high = readVram((uint16_t)(m_tile.addr + 8)); break;
    }
}

// The whole of sprite evaluation for the next line, done at dot 256 (the
// hardware spreads it over dots 65-256), with the overflow flag's diagonal
// scan bug (nesdev "PPU sprite evaluation").
void Ppu::evaluateSprites() {
    const int h = m_sprite16 ? 16 : 8;
    auto inRange = [&](uint8_t y) { return m_scanline >= y && m_scanline < y + h; };
    m_secondary.fill(0xFF);
    int count = 0, n = 0;
    m_sprite0Next = false;
    for (; n < 64 && count < 8; ++n) {
        const uint8_t y = m_oam[n * 4];
        m_secondary[count * 4] = y;
        if (inRange(y)) {
            for (int i = 1; i < 4; ++i) m_secondary[count * 4 + i] = m_oam[n * 4 + i];
            if (n == 0) m_sprite0Next = true;
            ++count;
        }
    }
    m_spriteCount = count;
}

int Ppu::overflowDot() const {
    // The evaluation's timing (Mesen ProcessSpriteEvaluation): from dot 65,
    // an OAM read on odd dots and a step on even ones; a sprite in range
    // takes 4 steps (Y and 3 bytes copied), one out of range 1. With 8
    // found, each later check is a step, and the overflow flag is set on the
    // step that finds a 9th in range.
    const int h = m_sprite16 ? 16 : 8;
    auto inRange = [&](uint8_t y) { return m_scanline >= y && m_scanline < y + h; };
    int step = 0, count = 0, n = 0;
    for (; n < 64 && count < 8; ++n) {
        if (inRange(m_oam[n * 4])) { step += 4; ++count; }
        else ++step;
    }
    if (count < 8) return -1;
    int m = 0;
    for (; n < 64; ++n, ++step) {
        if (inRange(m_oam[(n * 4 + m) & 0xFF])) return 66 + 2 * step;
        m = (m + 1) & 3;                // the bug: m moves on along with n
    }
    return -1;
}

void Ppu::fetchSprite(int i) {
    const uint8_t* s = &m_secondary[i * 4];
    uint8_t y = s[0], tile = s[1], attr = s[2], x = s[3];
    const bool real = i < m_spriteCount && y < 240;
    int line = real ? m_scanline - y : 0;
    if (!real) tile = 0xFF;
    if (real && (attr & 0x80)) line = (m_sprite16 ? 15 : 7) - line;
    uint16_t addr;
    if (m_sprite16) addr = (uint16_t)((((tile & 1) ? 0x1000 : 0) | ((tile & ~1) << 4)) + (line >= 8 ? line + 8 : line));
    else addr = (uint16_t)(((tile << 4) | m_spritePatterns) + line);
    const uint8_t lo = readVram(addr), hi = readVram((uint16_t)(addr + 8));
    if (!real) return;
    Sprite& sp = m_sprites[i];
    sp.low = lo;
    sp.high = hi;
    sp.x = x;
    sp.palette = (uint8_t)(((attr & 0x03) << 2) | 0x10);
    sp.behind = attr & 0x20;
    sp.hflip = attr & 0x40;
    if (m_scanline >= 0)
        for (int p = 0; p < 8 && x + p + 1 < 257; ++p) m_hasSprite[x + p + 1] = true;
}

uint8_t Ppu::pixel() {
    const int minBg = m_bgEnabled ? (m_bgLeft ? 0 : 8) : 300;
    const int minSprite = m_spritesEnabled ? (m_spritesLeft ? 0 : 8) : 300;
    uint8_t bg = 0;
    if (m_cycle > minBg)
        bg = (uint8_t)((((m_lowShift << m_fineX) & 0x8000) >> 15) | (((m_highShift << m_fineX) & 0x8000) >> 14));
    if (m_hasSprite[m_cycle] && m_cycle > minSprite) {
        for (int i = 0; i < m_lineSprites; ++i) {
            const Sprite& sp = m_sprites[i];
            const int shift = m_cycle - sp.x - 1;
            if (shift < 0 || shift >= 8) continue;
            uint8_t c;
            if (sp.hflip) c = (uint8_t)(((sp.low >> shift) & 1) | (((sp.high >> shift) & 1) << 1));
            else c = (uint8_t)((((sp.low << shift) & 0x80) >> 7) | (((sp.high << shift) & 0x80) >> 6));
            if (!c) continue;
            // Sprite 0 hit: an opaque sprite 0 pixel over an opaque background pixel, not at x = 255
            if (i == 0 && bg && m_sprite0Line && m_cycle != 256 && m_bgEnabled && !m_sprite0Hit) m_sprite0Hit = true;
            if (!bg || !sp.behind) return (uint8_t)(sp.palette + c);
            break;
        }
    }
    return (uint8_t)(((m_fineX + ((m_cycle - 1) & 7) < 8) ? m_prevPalette : m_curPalette) + bg);
}

void Ppu::renderDot() {
    // Dots 1-340 of lines -1..239
    if (m_cycle <= 256) {
        loadTile();
        if (m_renderingPrev && (m_cycle & 7) == 0) {
            incX();
            if (m_cycle == 256) incY();
        }
        if (m_scanline >= 0) {
            if (m_cycle == 65) m_overflowAt = m_renderingNow ? overflowDot() : -1;
            if (m_cycle == m_overflowAt && m_renderingNow) m_overflow = true;
            uint8_t c;
            if (m_renderingNow || (m_v & 0x3F00) != 0x3F00) {
                const uint8_t p = pixel();
                c = readPalette((p & 3) ? p : 0);
            } else {
                // Forced blank with v in the palette: that colour shows
                c = readPalette((uint8_t)(m_v & 0x1F));
            }
            if (m_grayscale) c &= 0x30;
            m_buffers[m_back][(size_t)(m_scanline << 8) + (size_t)(m_cycle - 1)] = (uint16_t)(c | ((m_mask >> 5) << 6));
            m_lowShift <<= 1;
            m_highShift <<= 1;
            if (m_cycle == 256) {
                if (m_renderingNow) evaluateSprites();
                else m_spriteCount = 0;
                // what the sprite fetches of this line will load
            }
        } else if (m_cycle == 1) {
            m_vblank = false;
        }
    } else if (m_cycle <= 320) {
        if (m_cycle == 257) {
            m_overflowAt = -1;
            m_spriteIndex = 0;
            m_hasSprite.fill(false);
            if (m_renderingPrev) m_v = (uint16_t)((m_v & ~0x041F) | (m_t & 0x041F));   // horizontal from t
        }
        if (m_renderingNow) {
            m_oamAddr = 0;
            switch ((m_cycle - 257) % 8) {
            case 0: readVram((uint16_t)(0x2000 | (m_v & 0x0FFF))); break;                         // garbage NT fetch
            case 2: readVram((uint16_t)(0x23C0 | (m_v & 0x0C00) | ((m_v >> 4) & 0x38) | ((m_v >> 2) & 0x07))); break;
            case 4: fetchSprite(m_spriteIndex++); break;
            }
            if (m_scanline == -1 && m_cycle >= 280 && m_cycle <= 304)
                m_v = (uint16_t)((m_v & ~0x7BE0) | (m_t & 0x7BE0));                             // vertical from t
        }
        if (m_cycle == 320) {
            m_lineSprites = m_scanline >= 0 ? m_spriteCount : 0;
            m_sprite0Line = m_sprite0Next && m_scanline >= 0;
        }
    } else if (m_cycle <= 336) {
        loadTile();
        if (m_renderingPrev && (m_cycle == 328 || m_cycle == 336)) {
            m_lowShift <<= 8;
            m_highShift <<= 8;
            incX();
        }
    } else if (m_cycle == 337 || m_cycle == 339) {
        if (m_renderingNow) {
            m_ntLatch = readVram((uint16_t)(0x2000 | (m_v & 0x0FFF)));
            // Odd frames with rendering on skip the last dot of the pre-render line
            if (m_scanline == -1 && m_cycle == 339 && (m_frameCount & 1)) m_cycle = 340;
        }
    }
}

void Ppu::firstDot() {
    m_cycle = 0;
    if (++m_scanline > VBLANK_END) {
        m_scanline = -1;
        m_spriteCount = 0;
        m_lineSprites = 0;
    }
    if (m_scanline == -1) {
        m_overflow = false;
        m_sprite0Hit = false;
    } else if (m_scanline < 240) {
        if (m_renderingPrev && (m_scanline > 0 || !(m_frameCount & 1)))
            setBusAddress((uint16_t)((m_ntLatch << 4) | (m_v >> 12) | m_bgPatterns));
    } else if (m_scanline == 240) {
        setBusAddress(m_v & 0x3FFF);
        std::swap(m_front, m_back);
        ++m_frameCount;
    }
}

void Ppu::exec() {
    if (m_cycle < 340) {
        ++m_cycle;
        if (m_scanline < 240) renderDot();
        else if (m_cycle == 1 && m_scanline == NMI_LINE) {
            if (!m_preventVblank) m_vblank = true;
            m_preventVblank = false;
        }
    } else {
        firstDot();
    }
    // Mesen UpdateState: $2001's rendering enable lands a dot late, and is
    // seen by the scroll increments a dot after that
    if (m_renderingPrev != m_renderingNow) m_renderingPrev = m_renderingNow;
    const bool en = m_bgEnabled || m_spritesEnabled;
    if (m_renderingNow != en) {
        m_renderingNow = en;
        if (!en && m_scanline < 240) setBusAddress(m_v & 0x3FFF);
    }
    if (m_vDelay && --m_vDelay == 0) {
        m_v = m_vPending;
        if (m_scanline >= 240 || !m_renderingNow) setBusAddress(m_v & 0x3FFF);
    }
    if (m_ignoreVramRead) --m_ignoreVramRead;
    if (m_pendingIncrement) {
        m_pendingIncrement = false;
        vramIncrement();
    }
    ++m_dots;
}

void Ppu::clock(uint32_t dots) {
    while (dots--) exec();
}

void Ppu::renderFrame(uint32_t* buffer) {
    const auto& src = m_buffers[m_front];
    for (size_t i = 0; i < src.size(); ++i) buffer[i] = color((uint8_t)(src[i] & 0x3F), (uint8_t)(src[i] >> 6));
}

}  // namespace nes
