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

    // Sprites on top
    VIC2::renderSprites(buf);
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

    // Fall back to VIC-III rendering (80-col, bitplane, standard text)
    VIC3::renderFrame(buffer);
}
