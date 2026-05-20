#include "vic4.h"
#include "libmem/main/ibus.h"
#include <cstring>

VIC4::VIC4()
    : VIC3("VIC-IV", 0xD000)
{
    m_colorRamExt.resize(32768, 0); // 32 KB internal color RAM
    reset();
}

void VIC4::reset() {
    VIC3::reset();
    std::memset(m_extRegs, 0, sizeof(m_extRegs));

    // Default VIC-IV extended registers (indexed as $D0xx - $D040)
    // $D060-$D063: SCRNPTR — screen RAM base (28-bit), default $0000
    m_extRegs[0x20] = 0x00; // SCRNPTRLSB
    m_extRegs[0x21] = 0x00; // SCRNPTRMSB
    m_extRegs[0x22] = 0x00; // SCRNPTRBNK
    m_extRegs[0x23] = 0x00; // SCRNPTRMB (bits 27-24)

    // $D064-$D065: COLPTR — colour RAM base, default $0000
    m_extRegs[0x24] = 0x00; // COLPTRLSB
    m_extRegs[0x25] = 0x00; // COLPTRMSB

    // $D068-$D06A: CHARPTR — character set base (24-bit), default $0000
    m_extRegs[0x28] = 0x00; // CHARPTRLSB
    m_extRegs[0x29] = 0x00; // CHARPTRMSB
    m_extRegs[0x2A] = 0x00; // CHARPTRBNK
}

void VIC4::tick(uint64_t cycles) {
    VIC3::tick(cycles);
}

bool VIC4::ioRead(IBus* bus, uint32_t addr, uint8_t* val) {
    if ((addr & ~addrMask()) != baseAddr()) return false;

    uint16_t offset = addr & 0x03FF;

    // $D048-$D07F: VIC-IV specific extended registers
    if (offset >= 0x0048 && offset <= 0x007F) {
        if (isLocked()) { *val = 0xFF; return true; }
        *val = m_extRegs[offset - 0x0040];
        return true;
    }

    // Everything else ($D000-$D047, $D080-$D3FF) handled by VIC3
    return VIC3::ioRead(bus, addr, val);
}

bool VIC4::ioWrite(IBus* bus, uint32_t addr, uint8_t val) {
    if ((addr & ~addrMask()) != baseAddr()) return false;

    uint16_t offset = addr & 0x03FF;

    // $D048-$D07F: VIC-IV specific extended registers
    if (offset >= 0x0048 && offset <= 0x007F) {
        if (isLocked()) return true;
        m_extRegs[offset - 0x0040] = val;
        return true;
    }

    // Everything else handled by VIC3
    return VIC3::ioWrite(bus, addr, val);
}

uint32_t VIC4::getScreenBase() const {
    if (isLocked()) return VIC2::screenBase();
    // $D060-$D063: SCRNPTR (28-bit physical address)
    return ((uint32_t)(m_extRegs[0x23] & 0x0F) << 24) |
           ((uint32_t)m_extRegs[0x22] << 16) |
           ((uint32_t)m_extRegs[0x21] << 8) |
           (uint32_t)m_extRegs[0x20];
}

uint32_t VIC4::getCharBase() const {
    if (isLocked()) return VIC2::charBitmapBase();
    // $D068-$D06A: CHARPTR (24-bit physical address)
    return ((uint32_t)m_extRegs[0x2A] << 16) |
           ((uint32_t)m_extRegs[0x29] << 8) |
           (uint32_t)m_extRegs[0x28];
}

uint16_t VIC4::getColBase() const {
    // $D064-$D065: COLPTR (16-bit offset into 32KB colour RAM)
    return ((uint16_t)m_extRegs[0x25] << 8) | m_extRegs[0x24];
}

int VIC4::getChrCount() const {
    // $D05E (offset 0x1E): characters per row. 0 = default (40 or 80).
    uint8_t v = m_extRegs[0x1E];
    return (v == 0) ? -1 : v; // -1 signals "use default"
}

int VIC4::getDispRows() const {
    // $D07B (offset 0x3B): text rows to display. 0 = default (25).
    uint8_t v = m_extRegs[0x3B];
    return (v == 0) ? 25 : v;
}

uint16_t VIC4::getLineStep() const {
    // $D058-$D059 (offsets 0x18-0x19): bytes between each text row (16-bit LE).
    // 0 = auto-calculate from columns × bytes-per-char.
    return ((uint16_t)m_extRegs[0x19] << 8) | m_extRegs[0x18];
}

// --- Border / text position ---

uint16_t VIC4::getTopBorder() const {
    // $D048-$D049 (offsets 0x08-0x09): top border position (12-bit)
    return ((uint16_t)(m_extRegs[0x09] & 0x0F) << 8) | m_extRegs[0x08];
}

uint16_t VIC4::getBottomBorder() const {
    // $D04A-$D04B (offsets 0x0A-0x0B): bottom border position (12-bit)
    return ((uint16_t)(m_extRegs[0x0B] & 0x0F) << 8) | m_extRegs[0x0A];
}

uint16_t VIC4::getTextXPos() const {
    // $D04C-$D04D (offsets 0x0C-0x0D): text X position (12-bit)
    return ((uint16_t)(m_extRegs[0x0D] & 0x0F) << 8) | m_extRegs[0x0C];
}

uint16_t VIC4::getTextYPos() const {
    // $D04E-$D04F (offsets 0x0E-0x0F): text Y position (12-bit)
    return ((uint16_t)(m_extRegs[0x0F] & 0x0F) << 8) | m_extRegs[0x0E];
}

uint16_t VIC4::getSideBorderWidth() const {
    // $D05C-$D05D (offsets 0x1C-0x1D): side border width (low 6 bits of MSB)
    return ((uint16_t)(m_extRegs[0x1D] & 0x3F) << 8) | m_extRegs[0x1C];
}

// --- Palette banks ($D070, offset 0x30) ---

uint8_t VIC4::getSprPalBank() const  { return m_extRegs[0x30] & 0x03; }
uint8_t VIC4::getBtPalBank() const   { return (m_extRegs[0x30] >> 2) & 0x03; }
uint8_t VIC4::getAbtPalBank() const  { return (m_extRegs[0x30] >> 4) & 0x03; }
uint8_t VIC4::getMapEdPal() const    { return (m_extRegs[0x30] >> 6) & 0x03; }

// --- System flags ---

bool VIC4::isVfast() const   { return (d054() & D054_VFAST) != 0; }
bool VIC4::isPalNtsc() const { return (m_extRegs[0x2F] & 0x80) != 0; } // $D06F bit 7

// ---------------------------------------------------------------------------
// Full-Colour Mode (FCM) and Nibble-Colour Mode (NCM) rendering
//
// FCM: Each character cell uses 64 bytes (8 rows × 8 pixels × 1 byte).
//      Each byte is a palette index. $FF = foreground from colour RAM.
//
// NCM: Each byte describes 2 pixels (high nibble = left, low nibble = right).
//      Characters are 16 pixels wide. Lower 4 bits from char data nibble,
//      upper 4 bits from colour RAM → full 8-bit palette index.
//      Nibble $F = use full colour RAM colour.
//      Selected per-character when colour RAM byte has bit 3 set (ncm_flag)
//      in Super-Extended Attribute Mode (CHR16).
//
// Enabled by FCLRLO ($D054 bit 1) for char numbers ≤ $FF
//      and/or FCLRHI ($D054 bit 2) for char numbers > $FF.
// CHR16 ($D054 bit 0) enables 16-bit char numbers and per-char NCM flag.
// ---------------------------------------------------------------------------

void VIC4::renderFCM(uint32_t* buf) {
    uint32_t borderCol = getPaletteRGBA(m_regs[EXTCOL] & 0xFF);
    uint32_t bgCol     = getPaletteRGBA(m_regs[BGCOL0] & 0xFF);

    // Fill border
    for (int i = 0; i < FRAME_W * FRAME_H; ++i) buf[i] = borderCol;

    uint32_t scrBase  = getScreenBase();
    uint32_t charBase = getCharBase();
    uint16_t colBase  = getColBase();

    uint8_t ctrl54 = d054();
    bool chr16  = (ctrl54 & D054_CHR16)  != 0;
    bool fclrLo = (ctrl54 & D054_FCLRLO) != 0;
    bool fclrHi = (ctrl54 & D054_FCLRHI) != 0;

    bool h640 = (m_regs[REG_D031] & D031_H640) != 0;
    int defaultCols = h640 ? 80 : 40;
    int chrCount = getChrCount();
    int cols = (chrCount < 0) ? defaultCols : chrCount;
    int rows = getDispRows();

    int bytesPerChar = chr16 ? 2 : 1;
    uint16_t lineStep = getLineStep();
    if (lineStep == 0) lineStep = cols * bytesPerChar;

    for (int row = 0; row < rows; ++row) {
        uint32_t rowBase = scrBase + (uint32_t)row * lineStep;
        for (int col = 0; col < cols; ++col) {
            int cellIdx = row * cols + col;
            uint32_t scrAddr = rowBase + (uint32_t)col * bytesPerChar;

            // Read character number
            uint16_t charNum;
            if (chr16) {
                uint8_t lo = m_dmaBus ? m_dmaBus->peek8(scrAddr) : 0;
                uint8_t hi = m_dmaBus ? m_dmaBus->peek8(scrAddr + 1) : 0;
                charNum = lo | ((uint16_t)hi << 8);
            } else {
                charNum = m_dmaBus ? m_dmaBus->peek8(scrAddr) : 0;
            }

            // Check if FCM/NCM applies to this character
            bool isFCM = (charNum <= 0xFF) ? fclrLo : fclrHi;
            if (!isFCM) continue;

            // Read colour RAM byte for this cell
            uint8_t fgColor = 0;
            uint16_t colAddr = colBase + cellIdx;
            if (m_colorRam && colBase == 0 && cellIdx < 2048) {
                fgColor = m_colorRam[cellIdx & 0x7FF];
            } else if (colAddr < m_colorRamExt.size()) {
                fgColor = m_colorRamExt[colAddr];
            }

            // NCM flag: colour RAM bit 3 when CHR16 is enabled
            bool isNCM = chr16 && (fgColor & 0x08);

            // Character data: 64 bytes at charBase + charNum * 64
            uint32_t dataAddr = charBase + (uint32_t)charNum * 64;

            int py = DISPLAY_Y + row * 8;

            if (isNCM) {
                // --- Nibble-Colour Mode ---
                // 16 pixels wide: 8 bytes per row, 2 pixels per byte
                // Upper 4 bits of colour = colour RAM (masked, bit 3 excluded)
                uint8_t colHigh = (fgColor & 0xF0); // upper nibble from colour RAM
                int charW = h640 ? 8 : 16; // NCM chars are 16px in 40-col, 8px in 80-col
                int px = DISPLAY_X + col * (h640 ? 4 : 8); // screen position for this cell
                // In NCM, each char occupies double width in the character grid
                // but we position based on the cell index

                for (int r = 0; r < 8; ++r) {
                    int fy = py + r;
                    if (fy < DISPLAY_Y || fy >= DISPLAY_Y + DISPLAY_H) continue;

                    for (int c = 0; c < 8; ++c) {
                        uint8_t byteVal = m_dmaBus ? m_dmaBus->peek8(dataAddr + r * 8 + c) : 0;
                        uint8_t hiNib = (byteVal >> 4) & 0x0F;
                        uint8_t loNib = byteVal & 0x0F;

                        // Left pixel (high nibble)
                        uint32_t colorL;
                        if (hiNib == 0x0F) {
                            colorL = getPaletteRGBA(fgColor);
                        } else if (hiNib == 0x00) {
                            colorL = bgCol;
                        } else {
                            colorL = getPaletteRGBA(colHigh | hiNib);
                        }

                        // Right pixel (low nibble)
                        uint32_t colorR;
                        if (loNib == 0x0F) {
                            colorR = getPaletteRGBA(fgColor);
                        } else if (loNib == 0x00) {
                            colorR = bgCol;
                        } else {
                            colorR = getPaletteRGBA(colHigh | loNib);
                        }

                        int fx = px + c * 2;
                        if (fx >= DISPLAY_X && fx < DISPLAY_X + DISPLAY_W)
                            buf[fy * FRAME_W + fx] = colorL;
                        if (fx + 1 >= DISPLAY_X && fx + 1 < DISPLAY_X + DISPLAY_W)
                            buf[fy * FRAME_W + fx + 1] = colorR;
                    }
                }
            } else {
                // --- Full-Colour Mode ---
                int charW = h640 ? 4 : 8;
                int px = DISPLAY_X + col * charW;

                for (int r = 0; r < 8; ++r) {
                    int fy = py + r;
                    if (fy < DISPLAY_Y || fy >= DISPLAY_Y + DISPLAY_H) continue;

                    for (int c = 0; c < 8; ++c) {
                        uint8_t pixVal = m_dmaBus ? m_dmaBus->peek8(dataAddr + r * 8 + c) : 0;

                        uint32_t color;
                        if (pixVal == 0xFF) {
                            color = getPaletteRGBA(fgColor);
                        } else if (pixVal == 0x00) {
                            color = bgCol;
                        } else {
                            color = getPaletteRGBA(pixVal);
                        }

                        if (h640) {
                            if (c & 1) continue;
                            int fx = px + (c / 2);
                            if (fx >= DISPLAY_X && fx < DISPLAY_X + DISPLAY_W)
                                buf[fy * FRAME_W + fx] = color;
                        } else {
                            int fx = px + c;
                            if (fx >= DISPLAY_X && fx < DISPLAY_X + DISPLAY_W)
                                buf[fy * FRAME_W + fx] = color;
                        }
                    }
                }
            }
        }
    }

    // Sprites on top (use VIC-IV extended sprites when unlocked)
    renderSpritesV4(buf);
}

// ---------------------------------------------------------------------------
// VIC-IV Extended Sprite Renderer
//
// Extensions over VIC-II:
//   $D055 SPRHGTEN:  per-sprite extended height enable
//   $D056 SPRHGHT:   shared extended height (0-255 pixels)
//   $D057 SPRX64EN:  per-sprite 64-pixel-wide mode (8 bytes/row)
//   $D06B SPR16EN:   per-sprite 16-colour mode (4 bits/pixel)
//   $D06C-$D06E SPRPTRADR: 24-bit sprite pointer base address
//   $D06E.7 SPRPTR16: 16-bit sprite pointers (2 bytes each)
//   $D077 SPRYMSBS:  per-sprite Y position bit 8
// ---------------------------------------------------------------------------

void VIC4::renderSpritesV4(uint32_t* buf) {
    if (!m_dmaBus) return;

    uint8_t sprHgtEn  = m_extRegs[0x15]; // $D055: per-sprite extended height enable
    uint8_t sprHgt    = m_extRegs[0x16]; // $D056: shared extended height
    uint8_t sprX64En  = m_extRegs[0x17]; // $D057: per-sprite 64px wide
    uint8_t spr16En   = m_extRegs[0x2B]; // $D06B: per-sprite 16-colour
    uint8_t sprYMsbs  = m_extRegs[0x37]; // $D077: per-sprite Y bit 8

    // Sprite pointer base: $D06C-$D06E (24-bit, bottom 7 bits of $D06E)
    uint32_t sprPtrBase = (uint32_t)m_extRegs[0x2C]
                        | ((uint32_t)m_extRegs[0x2D] << 8)
                        | ((uint32_t)(m_extRegs[0x2E] & 0x7F) << 16);
    bool sprPtr16 = (m_extRegs[0x2E] & 0x80) != 0;

    // If sprPtrBase is 0, use VIC-II default: screen base + $3F8
    bool useDefaultPtrs = (sprPtrBase == 0 && !sprPtr16);

    for (int sp = 7; sp >= 0; --sp) {
        if (!(m_regs[SPENA] & (1 << sp))) continue;

        // Position
        uint16_t spX = (uint16_t)m_regs[SP0X + sp * 2];
        if (m_regs[MSIGX] & (1 << sp)) spX |= 0x100;
        uint16_t spY = (uint16_t)m_regs[SP0Y + sp * 2];
        if (sprYMsbs & (1 << sp)) spY |= 0x100;

        bool xExp = (m_regs[XXPAND] & (1 << sp)) != 0;
        bool yExp = (m_regs[YXPAND] & (1 << sp)) != 0;

        // Height: 21 (VIC-II default) or extended
        int height = (sprHgtEn & (1 << sp)) ? (int)sprHgt : 21;
        if (height == 0) continue;

        // Width mode
        bool is64wide = (sprX64En & (1 << sp)) != 0;
        bool is16col  = (spr16En  & (1 << sp)) != 0;
        int pixelWidth = is64wide ? 64 : (is16col ? 16 : 24);
        int bytesPerRow = is64wide ? 8 : 3;

        // Sprite data address
        uint32_t dataAddr;
        if (useDefaultPtrs) {
            uint32_t ptrAddr = screenBase() + 0x03F8 + sp;
            uint8_t ptrVal = dmaPeek(ptrAddr);
            dataAddr = (uint32_t)ptrVal * 64;
        } else if (sprPtr16) {
            uint32_t ptrAddr = sprPtrBase + sp * 2;
            uint8_t lo = m_dmaBus->peek8(ptrAddr);
            uint8_t hi = m_dmaBus->peek8(ptrAddr + 1);
            dataAddr = ((uint32_t)lo | ((uint32_t)hi << 8)) * 64;
        } else {
            uint32_t ptrAddr = sprPtrBase + sp;
            uint8_t ptrVal = m_dmaBus->peek8(ptrAddr);
            dataAddr = (uint32_t)ptrVal * 64;
        }

        // Colour
        uint8_t colorIdx = m_regs[SP0COL + sp] & 0x0F;

        for (int r = 0; r < height; ++r) {
            int py = (int)spY + (yExp ? r * 2 : r);
            if (py < 0 || py >= FRAME_H) continue;

            if (is16col) {
                // 16-colour mode: 4 bits per pixel, normal resolution
                // Color = sprite# × 16 + nibble. Nibble 0 = transparent.
                for (int byteIdx = 0; byteIdx < bytesPerRow; ++byteIdx) {
                    uint8_t byteVal = m_dmaBus->peek8(dataAddr + r * bytesPerRow + byteIdx);
                    uint8_t hiNib = (byteVal >> 4) & 0x0F;
                    uint8_t loNib = byteVal & 0x0F;

                    for (int half = 0; half < 2; ++half) {
                        uint8_t nib = (half == 0) ? hiNib : loNib;
                        if (nib == 0) continue; // transparent

                        uint8_t palIdx = (uint8_t)(sp * 16 + nib);
                        uint32_t color = getPaletteRGBA(palIdx);

                        int bx = byteIdx * 2 + half;
                        int pxStart = (int)spX + (xExp ? bx * 2 : bx);
                        int pxEnd = xExp ? pxStart + 2 : pxStart + 1;
                        for (int px = pxStart; px < pxEnd; ++px) {
                            if (px >= 0 && px < FRAME_W)
                                buf[py * FRAME_W + px] = color;
                        }
                    }
                }
            } else {
                // Standard mono sprite (24 or 64 pixels wide)
                uint32_t spColor = getPaletteRGBA(colorIdx);

                for (int byteIdx = 0; byteIdx < bytesPerRow; ++byteIdx) {
                    uint8_t byteVal = m_dmaBus->peek8(dataAddr + r * bytesPerRow + byteIdx);
                    for (int bit = 7; bit >= 0; --bit) {
                        if (!((byteVal >> bit) & 1)) continue;

                        int bx = byteIdx * 8 + (7 - bit);
                        int pxStart = (int)spX + (xExp ? bx * 2 : bx);
                        int pxEnd = xExp ? pxStart + 2 : pxStart + 1;
                        for (int px = pxStart; px < pxEnd; ++px) {
                            if (px >= 0 && px < FRAME_W)
                                buf[py * FRAME_W + px] = spColor;
                        }
                    }
                }
            }

            // Y expansion: duplicate sprite pixels on next row
            if (yExp && (py + 1 < FRAME_H)) {
                int totalPx = xExp ? pixelWidth * 2 : pixelWidth;
                for (int dx = 0; dx < totalPx; ++dx) {
                    int px = (int)spX + dx;
                    if (px >= 0 && px < FRAME_W) {
                        buf[(py + 1) * FRAME_W + px] = buf[py * FRAME_W + px];
                    }
                }
            }
        }
    }
}

void VIC4::renderFrame(uint32_t* buffer) {
    if (isLocked()) {
        VIC2::renderFrame(buffer);
        return;
    }

    // Check for FCM mode
    uint8_t ctrl54 = d054();
    if (ctrl54 & (D054_FCLRLO | D054_FCLRHI)) {
        renderFCM(buffer);
        return;
    }

    // Check for bitplane mode with 16-colour extensions
    uint8_t bp16ens = m_extRegs[0x31]; // $D071: 16-colour bitplane enables
    if ((m_regs[REG_D031] & D031_BPM) && bp16ens) {
        renderBitplanes16(buffer);
        renderSpritesV4(buffer);
        return;
    }

    // Fall back to VIC-III rendering (80-col, bitplane, standard text)
    VIC3::renderFrame(buffer);
}

// ---------------------------------------------------------------------------
// 16-colour bitplane rendering ($D071 BP16ENS)
//
// Each pair of enabled bitplanes (0+1, 2+3, 4+5, 6+7) forms a 4-bit layer.
// The lower plane in the pair provides the low 2 bits, the upper provides
// the high 2 bits, yielding a 4-bit colour index per layer.
// Layers are composited: higher-numbered layers on top, colour 0 transparent.
// ---------------------------------------------------------------------------

void VIC4::renderBitplanes16(uint32_t* buf) {
    uint32_t borderCol = getPaletteRGBA(m_regs[EXTCOL] & 0xFF);
    for (int i = 0; i < FRAME_W * FRAME_H; ++i) buf[i] = borderCol;

    uint8_t bpEnable = m_regs[REG_BPEN];
    uint8_t bpComp   = m_regs[0x3B];
    int bpxOff = m_regs[0x3C];
    int bpyOff = m_regs[0x3D];
    uint8_t bp16ens = m_extRegs[0x31]; // $D071

    bool h640 = (m_regs[REG_D031] & D031_H640) != 0;
    int pixelsPerLine = h640 ? 640 : 320;
    if (pixelsPerLine > DISPLAY_W) pixelsPerLine = DISPLAY_W;

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

            // Start with background (index 0)
            uint8_t finalColor = 0;

            // Process 4 pairs of bitplanes (0+1, 2+3, 4+5, 6+7)
            for (int pair = 0; pair < 4; ++pair) {
                int bpLo = pair * 2;
                int bpHi = pair * 2 + 1;

                // Check if this pair is in 16-colour mode
                if (!(bp16ens & (1 << pair))) {
                    // Standard mode for this pair — contribute individual bits
                    for (int sub = 0; sub < 2; ++sub) {
                        int bp = bpLo + sub;
                        if (!(bpEnable & (1 << bp))) {
                            if (bpComp & (1 << bp)) finalColor |= (1 << bp);
                            continue;
                        }
                        uint32_t addr = (srcY & 1) ? bpAddrOdd[bp] : bpAddrEven[bp];
                        uint8_t byte = dmaPeek(addr + byteIdx);
                        int bit = (byte >> bitIdx) & 1;
                        if (bpComp & (1 << bp)) bit ^= 1;
                        if (bit) finalColor |= (1 << bp);
                    }
                } else {
                    // 16-colour: pair forms a 4-bit nibble (layered, 0 = transparent)
                    uint8_t nibble = 0;
                    for (int sub = 0; sub < 2; ++sub) {
                        int bp = bpLo + sub;
                        if (!(bpEnable & (1 << bp))) continue;
                        uint32_t addr = (srcY & 1) ? bpAddrOdd[bp] : bpAddrEven[bp];
                        uint8_t byte = dmaPeek(addr + byteIdx);
                        int bit = (byte >> bitIdx) & 1;
                        if (bpComp & (1 << bp)) bit ^= 1;
                        if (bit) nibble |= (1 << sub);
                    }
                    // Nibble non-zero → overwrite (layer compositing)
                    if (nibble != 0) {
                        // Place nibble into the pair's bit position
                        finalColor = (finalColor & ~(0x03 << (pair * 2))) | (nibble << (pair * 2));
                    }
                }
            }

            int fx = DISPLAY_X + x;
            if (fx >= 0 && fx < FRAME_W)
                buf[fy * FRAME_W + fx] = getPaletteRGBA(finalColor);
        }
    }
}
