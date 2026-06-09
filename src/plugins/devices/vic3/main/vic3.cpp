#include "vic3.h"
#include <cstring>
#include <algorithm>

// C64-compatible palette (RGB components)
static const uint8_t s_c64R[] = { 0x00,0xFF,0x68,0x70,0x6F,0x58,0x35,0xB8,0x6F,0x43,0x9A,0x44,0x6C,0x9A,0x6C,0x95 };
static const uint8_t s_c64G[] = { 0x00,0xFF,0x37,0xA4,0x3D,0x8D,0x28,0xC7,0x4F,0x39,0x67,0x44,0x6C,0xD2,0x5E,0x95 };
static const uint8_t s_c64B[] = { 0x00,0xFF,0x2B,0xB2,0x86,0x43,0x79,0x6F,0x25,0x00,0x59,0x44,0x6C,0x84,0xB5,0x95 };

VIC3::VIC3()
    : VIC2("VIC-III", 0xD000), m_vic3Name("VIC-III")
{
    reset();
}

VIC3::VIC3(const std::string& name, uint32_t baseAddr)
    : VIC2(name, baseAddr), m_vic3Name(name)
{
    reset();
}

void VIC3::initPalette() {
    std::memset(m_paletteR, 0, sizeof(m_paletteR));
    std::memset(m_paletteG, 0, sizeof(m_paletteG));
    std::memset(m_paletteB, 0, sizeof(m_paletteB));

    for (int i = 0; i < 16; ++i) {
        m_paletteR[i] = s_c64R[i];
        m_paletteG[i] = s_c64G[i];
        m_paletteB[i] = s_c64B[i];
    }
}

void VIC3::reset() {
    VIC2::reset();
    m_locked = true;
    initPalette();

    // Clear VIC-III extended registers ($D030-$D03F)
    // These share space with the VIC2 m_regs[64] array (indices 0x30-0x3F)
    for (int i = 0x30; i <= 0x3F; ++i)
        m_regs[i] = 0;

    // Default $D031: H640 + FAST + ATTR on C65 boot (per MEGA65 book)
    // But we start locked in C64 mode, so these only take effect when unlocked.
    // Reset to 0 — OS will set them after unlock.
}

uint32_t VIC3::getPaletteRGBA(uint8_t index) const {
    // RGBA8888: alpha=0xFF, B in bits 23-16, G in bits 15-8, R in bits 7-0
    return 0xFF000000u
         | ((uint32_t)m_paletteB[index] << 16)
         | ((uint32_t)m_paletteG[index] << 8)
         | (uint32_t)m_paletteR[index];
}

// ---------------------------------------------------------------------------
// IOHandler: ioRead
// ---------------------------------------------------------------------------

bool VIC3::ioRead(IBus* bus, uint32_t addr, uint8_t* val) {
    if ((addr & ~addrMask()) != baseAddr()) return false;

    uint16_t offset = addr & 0x03FF;

    // $D000-$D02E: Standard VIC-II registers
    if (offset <= 0x002E) {
        // In unlocked mode, color registers return full 8-bit palette index
        if (!m_locked && (m_regs[REG_D031] & D031_ATTR)) {
            // Color regs: $D020-$D02E return full 8 bits (not masked to 4)
            if (offset >= 0x0020 && offset <= 0x002E) {
                *val = m_regs[offset & 0x3F];
                return true;
            }
        }
        return VIC2::ioRead(bus, addr, val);
    }

    // $D02F: KEY register — handled by separate KeyRegister device
    if (offset == 0x002F) {
        *val = 0xFF;
        return false;
    }

    // $D030-$D03F: VIC-III registers
    if (offset >= 0x0030 && offset <= 0x003F) {
        if (m_locked) { *val = 0xFF; return true; }
        *val = m_regs[offset & 0x3F];
        return true;
    }

    // $D040-$D047: DAT bitplane ports (VIC-III)
    if (offset >= 0x0040 && offset <= 0x0047) {
        if (m_locked) { *val = 0xFF; return true; }
        *val = m_regs[offset & 0x3F]; // Reuse m_regs space
        return true;
    }

    // $D048-$D07F: reserved / VIC-IV range — return $FF here, VIC4 overrides
    if (offset >= 0x0048 && offset <= 0x007F) {
        *val = 0xFF;
        return true;
    }

    // $D0A0-$D0FF: mirrors or unused ($D080-$D09F reserved for F011 FDC)
    if (offset >= 0x00A0 && offset <= 0x00FF) {
        *val = 0xFF;
        return true;
    }

    // $D100-$D1FF: Palette Red
    if (offset >= 0x0100 && offset <= 0x01FF) {
        if (m_locked) { *val = 0xFF; return true; }
        *val = m_paletteR[offset & 0xFF];
        return true;
    }

    // $D200-$D2FF: Palette Green
    if (offset >= 0x0200 && offset <= 0x02FF) {
        if (m_locked) { *val = 0xFF; return true; }
        *val = m_paletteG[offset & 0xFF];
        return true;
    }

    // $D300-$D3FF: Palette Blue
    if (offset >= 0x0300 && offset <= 0x03FF) {
        if (m_locked) { *val = 0xFF; return true; }
        *val = m_paletteB[offset & 0xFF];
        return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
// IOHandler: ioWrite
// ---------------------------------------------------------------------------

bool VIC3::ioWrite(IBus* bus, uint32_t addr, uint8_t val) {
    if ((addr & ~addrMask()) != baseAddr()) return false;

    uint16_t offset = addr & 0x03FF;

    // $D000-$D02E: Standard VIC-II registers
    if (offset <= 0x002E) {
        // In unlocked ATTR mode, color registers accept full 8-bit
        if (!m_locked && (m_regs[REG_D031] & D031_ATTR)) {
            if (offset >= 0x0020 && offset <= 0x002E) {
                m_regs[offset & 0x3F] = val;
                return true;
            }
        }
        return VIC2::ioWrite(bus, addr, val);
    }

    // $D02F: KEY register
    if (offset == 0x002F) {
        return false; // Handled by KeyRegister
    }

    // $D030-$D03F: VIC-III registers
    if (offset >= 0x0030 && offset <= 0x003F) {
        if (m_locked) return true; // Silently ignore when locked
        m_regs[offset & 0x3F] = val;
        return true;
    }

    // $D040-$D047: DAT bitplane ports
    if (offset >= 0x0040 && offset <= 0x0047) {
        if (m_locked) return true;
        m_regs[offset & 0x3F] = val;
        return true;
    }

    // $D048-$D07F: reserved / VIC-IV range
    if (offset >= 0x0048 && offset <= 0x007F) {
        return true; // Ignore
    }

    // $D0A0-$D0FF: mirrors or unused ($D080-$D09F reserved for F011 FDC)
    if (offset >= 0x00A0 && offset <= 0x00FF) {
        return true; // Ignore
    }

    // $D100-$D1FF: Palette Red
    if (offset >= 0x0100 && offset <= 0x01FF) {
        if (m_locked) return true;
        m_paletteR[offset & 0xFF] = val;
        return true;
    }

    // $D200-$D2FF: Palette Green
    if (offset >= 0x0200 && offset <= 0x02FF) {
        if (m_locked) return true;
        m_paletteG[offset & 0xFF] = val;
        return true;
    }

    // $D300-$D3FF: Palette Blue
    if (offset >= 0x0300 && offset <= 0x03FF) {
        if (m_locked) return true;
        m_paletteB[offset & 0xFF] = val;
        return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
// IVideoOutput: renderFrame
// ---------------------------------------------------------------------------

void VIC3::renderFrame(uint32_t* buffer) {
    if (m_locked) {
        VIC2::renderFrame(buffer);
        return;
    }

    uint8_t ctrl = m_regs[REG_D031];

    if (ctrl & D031_BPM) {
        // Bitplane mode
        renderBitplanes(buffer);
        return;
    }

    if (ctrl & D031_H640) {
        // 80-column text mode
        renderBackground80col(buffer);
        VIC2::renderSprites(buffer);
        return;
    }

    // Standard 40-column mode with palette support
    // Use VIC2 rendering but substitute palette colors if PAL bit set
    VIC2::renderFrame(buffer);
}

// ---------------------------------------------------------------------------
// 80-column text rendering
// ---------------------------------------------------------------------------

void VIC3::renderBackground80col(uint32_t* buf) {
    uint32_t borderCol = getPaletteRGBA(m_regs[EXTCOL] & 0xFF);
    uint32_t bgCol     = getPaletteRGBA(m_regs[BGCOL0] & 0xFF);

    // Fill border
    for (int i = 0; i < FRAME_W * FRAME_H; ++i) buf[i] = borderCol;

    bool attrMode = (m_regs[REG_D031] & D031_ATTR) != 0;
    uint32_t scrBase = screenBase();
    uint32_t cbBase  = charBitmapBase();

    // 80 columns × 25 rows. Each character cell is 4 pixels wide (half of 8).
    // Display area: 640 logical pixels → 320 screen pixels at half-width,
    // or we render at FRAME_W=384 with 4px per char in the 320px display area.
    // For simplicity, render each 80-col char as 4 pixels wide within DISPLAY_W.
    int charW = 4; // pixels per character in 80-col mode
    int cols = 80;

    bool isBank0or2 = (m_bankBase == 0x0000 || m_bankBase == 0x8000);
    bool useCharRom = isBank0or2 && (cbBase == 0x1000);

    for (int row = 0; row < 25; ++row) {
        for (int col = 0; col < cols; ++col) {
            int cellIdx = row * cols + col;
            uint8_t code = dmaPeek(scrBase + cellIdx);

            uint8_t colorNibble = m_colorRam ? m_colorRam[cellIdx & 0x7FF] : 0x0F;
            uint32_t fgCol;
            if (attrMode)
                fgCol = getPaletteRGBA(colorNibble);
            else
                fgCol = getPaletteRGBA(colorNibble & 0x0F);

            uint32_t glyphOffset = cbBase + code * 8;

            int px = DISPLAY_X + col * charW;
            int py = DISPLAY_Y + row * 8;

            for (int r = 0; r < 8; ++r) {
                uint8_t bits;
                if (useCharRom)
                    bits = charRomByte(code * 8 + r);
                else
                    bits = dmaPeek(glyphOffset + r);

                int fy = py + r;
                if (fy < DISPLAY_Y || fy >= DISPLAY_Y + DISPLAY_H) continue;

                // 80-col: render 8 glyph bits into 4 screen pixels (2 bits per pixel)
                // Each pair of glyph bits ORed together → 1 pixel
                for (int p = 0; p < 4; ++p) {
                    int b1 = (bits >> (7 - p * 2)) & 1;
                    int b2 = (bits >> (6 - p * 2)) & 1;
                    int fx = px + p;
                    if (fx < DISPLAY_X || fx >= DISPLAY_X + DISPLAY_W) continue;
                    buf[fy * FRAME_W + fx] = (b1 || b2) ? fgCol : bgCol;
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Bitplane rendering
// ---------------------------------------------------------------------------

void VIC3::renderBitplanes(uint32_t* buf) {
    uint32_t borderCol = getPaletteRGBA(m_regs[EXTCOL] & 0xFF);

    // Fill border
    for (int i = 0; i < FRAME_W * FRAME_H; ++i) buf[i] = borderCol;

    uint8_t bpEnable = m_regs[REG_BPEN];
    uint8_t bpComp   = m_regs[0x3B]; // complement flags
    int bpxOff = m_regs[0x3C];       // X offset
    int bpyOff = m_regs[0x3D];       // Y offset

    bool h640 = (m_regs[REG_D031] & D031_H640) != 0;
    int pixelsPerLine = h640 ? 640 : 320;
    int pixelScale = h640 ? 1 : 1; // In 320 mode each pixel is 1 screen px
    // Limit to display area
    if (pixelsPerLine > DISPLAY_W) pixelsPerLine = DISPLAY_W;

    // Bitplane addresses: $D033-$D03A
    // Each register: high nybble = odd line address (bits 19-16), low nybble = even line address
    // Address = nybble << 13 (8KB blocks)
    uint32_t bpAddrEven[8], bpAddrOdd[8];
    for (int i = 0; i < 8; ++i) {
        uint8_t reg = m_regs[0x33 + i];
        bpAddrEven[i] = (uint32_t)(reg & 0x0F) << 13;
        bpAddrOdd[i]  = (uint32_t)((reg >> 4) & 0x0F) << 13;
    }

    int bytesPerRow = pixelsPerLine / 8;

    for (int y = 0; y < DISPLAY_H; ++y) {
        int srcY = (y + bpyOff) % DISPLAY_H;
        int fy = DISPLAY_Y + y;

        for (int x = 0; x < pixelsPerLine; ++x) {
            int srcX = (x + bpxOff) % pixelsPerLine;
            int byteIdx = srcY * bytesPerRow + (srcX / 8);
            int bitIdx = 7 - (srcX & 7);

            uint8_t colorIdx = 0;
            for (int bp = 0; bp < 8; ++bp) {
                if (!(bpEnable & (1 << bp))) {
                    // Disabled plane: complement bit contributes 1 if set
                    if (bpComp & (1 << bp)) colorIdx |= (1 << bp);
                    continue;
                }
                uint32_t addr = (srcY & 1) ? bpAddrOdd[bp] : bpAddrEven[bp];
                uint8_t byte = dmaPeek(addr + byteIdx);
                int bit = (byte >> bitIdx) & 1;
                if (bpComp & (1 << bp)) bit ^= 1;
                if (bit) colorIdx |= (1 << bp);
            }

            int fx = DISPLAY_X + x;
            if (fx >= 0 && fx < FRAME_W)
                buf[fy * FRAME_W + fx] = getPaletteRGBA(colorIdx);
        }
    }
}
