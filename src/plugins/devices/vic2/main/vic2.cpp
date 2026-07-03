#include "vic2.h"
#include "libmem/main/ibus.h"
#include "mmemu_plugin_api.h"
#include <cstring>
#include <algorithm>
#include <cstdio>

VIC2::VIC2(const std::string& name, uint32_t baseAddr)
    : m_name(name), m_baseAddr(baseAddr)
{
    VIC2::reset();
}

void VIC2::reset() {
    std::memset(m_regs, 0, sizeof(m_regs));
    // Power-on state: display enabled, 25 rows, 40 cols, standard text
    m_regs[SCR1] = SCR1_DEN | SCR1_RSEL | 0x03; // scroll Y = 3
    m_regs[SCR2] = SCR2_CSEL | 0x08;             // scroll X = 0, CSEL=1
    m_regs[EXTCOL] = 0x0E; // light blue border (common C64 default)
    m_regs[BGCOL0] = 0x06; // blue background
    m_regs[VMEM]   = 0x14; // screen at $0400, char at $1000

    m_cycleAccum = 0;
    m_rasterLine = 0;
    m_sprSprColl = 0;
    m_sprBgColl  = 0;

    if (m_irqLine) m_irqLine->set(false);
}

// ---------------------------------------------------------------------------
// Standard C64 palette (RGBA8888)
// ---------------------------------------------------------------------------

static const uint32_t s_palette[16] = {
    0xFF000000, // 0  Black
    0xFFFFFFFF, // 1  White
    0xFF68372B, // 2  Red
    0xFF70A4B2, // 3  Cyan
    0xFF6F3D86, // 4  Purple
    0xFF588D43, // 5  Green
    0xFF352879, // 6  Blue
    0xFFB8C76F, // 7  Yellow
    0xFF6F4F25, // 8  Orange
    0xFF433900, // 9  Brown
    0xFF9A6759, // 10 Light Red
    0xFF444444, // 11 Dark Gray
    0xFF6C6C6C, // 12 Gray
    0xFF9AD284, // 13 Light Green
    0xFF6C5EB5, // 14 Light Blue
    0xFF959595, // 15 Light Gray
};

uint32_t VIC2::palette(uint8_t index) const {
    return s_palette[index & 0x0F];
}

uint8_t VIC2::dmaPeek(uint32_t offset) const {
    if (!m_dmaBus) return 0xFF;
    offset &= 0x3FFF; // 14-bit internal VIC-II address

    // Character ROM shadow: bank offsets $1000-$1FFF.
    // On a real C64, this shadow is ONLY visible in banks 0 and 2
    // (where the VIC-II's VA13 pin is effectively 0).
    bool isBank0or2 = (m_bankBase == 0x0000 || m_bankBase == 0x8000);
    if (m_charRom && isBank0or2 && (offset >= 0x1000 && offset <= 0x1FFF)) {
        return m_charRom[offset & 0x0FFF];
    }

    uint32_t addr = (m_bankBase + offset) & 0xFFFF;
    return m_dmaBus->peek8(addr);
}

uint8_t VIC2::charRomByte(uint32_t offset) const {
    if (!m_charRom || m_charRomSize == 0) return 0xFF;
    return m_charRom[offset & 0x0FFF];
}

// ---------------------------------------------------------------------------
// Memory pointer helpers
// ---------------------------------------------------------------------------

uint32_t VIC2::screenBase() const {
    // VMEM bits 7-4 select screen RAM in 1 KB steps within the 16 KB VIC bank.
    return ((m_regs[VMEM] >> 4) & 0x0F) * 0x0400;
}

uint32_t VIC2::charBitmapBase() const {
    // VMEM bits 3-1 select char/bitmap base in 2 KB steps within the 16 KB VIC bank.
    return ((m_regs[VMEM] >> 1) & 0x07) * 0x0800;
}

// ---------------------------------------------------------------------------
// IOHandler: ioRead
// ---------------------------------------------------------------------------

bool VIC2::ioRead(IBus* /*bus*/, uint32_t addr, uint8_t* val) {
    if ((addr & ~addrMask()) != m_baseAddr) return false;

    uint8_t reg = addr & 0x3F;

    if (reg > 0x2E) {
        // $D02F–$D03F: unused, read as $FF
        *val = 0xFF;
        return true;
    }

    switch (reg) {
        case RASTER:
            *val = (uint8_t)(m_rasterLine & 0xFF);
            break;
        case SCR1:
            *val = (m_regs[SCR1] & ~SCR1_RASTER8) |
                   ((m_rasterLine >> 8) ? SCR1_RASTER8 : 0);
            break;
        case IRQ:
            // Bit 7 is set if any enabled IRQ source is active.
            *val = m_regs[IRQ] | ((m_regs[IRQ] & m_regs[IRQEN] & 0x0F) ? IRQ_ANY : 0);
            break;
        case SSCOL:
            *val = m_sprSprColl;
            m_sprSprColl = 0; // cleared on read
            break;
        case SBCOL:
            *val = m_sprBgColl;
            m_sprBgColl = 0;
            break;
        // Color registers: only low 4 bits exist; high bits read as 1
        case EXTCOL: case BGCOL0: case BGCOL1: case BGCOL2: case BGCOL3:
        case SPMC0:  case SPMC1:
        case SP0COL: case SP1COL: case SP2COL: case SP3COL:
        case SP4COL: case SP5COL: case SP6COL: case SP7COL:
            *val = m_regs[reg] | 0xF0;
            break;
        default:
            *val = m_regs[reg];
            break;
    }
    return true;
}

// ---------------------------------------------------------------------------
// IOHandler: ioWrite
// ---------------------------------------------------------------------------

bool VIC2::ioWrite(IBus* /*bus*/, uint32_t addr, uint8_t val) {
    if ((addr & ~addrMask()) != m_baseAddr) return false;

    uint8_t reg = addr & 0x3F;
    if (reg > 0x2E) return true; // writes to unused regs are ignored

    switch (reg) {
        case IRQ:
            // Writing 1 to a bit clears that IRQ flag.
            m_regs[IRQ] &= ~(val & 0x0F);
            updateIrq();
            break;
        case IRQEN:
            m_regs[IRQEN] = val & 0x0F;
            updateIrq();
            break;
        case SSCOL:
        case SBCOL:
            // Read-only collision registers; writes are ignored.
            break;
        default:
            m_regs[reg] = val;
            break;
    }
    return true;
}

// ---------------------------------------------------------------------------
// IOHandler: tick — drives the raster counter and fires raster IRQ
// ---------------------------------------------------------------------------

void VIC2::tick(uint64_t cycles) {
    m_cycleAccum += cycles;

    // Advance raster lines based on accumulated cycles.
    while (m_cycleAccum >= (uint64_t)m_cyclesPerLine) {
        m_cycleAccum -= m_cyclesPerLine;
        m_rasterLine++;

        if (m_rasterLine >= (uint16_t)m_linesPerFrame) {
            m_rasterLine = 0;
        }

        // Badline detection (#20): a badline occurs when the raster is in the
        // display area, DEN is set, and the lower 3 bits of the raster line
        // match the Y-scroll value.  On a badline the VIC steals ~40 bus
        // cycles from the CPU to fetch character pointers.
        if (m_stallBacklog && m_badlineEnable && (m_regs[SCR1] & SCR1_DEN)) {
            uint16_t firstDisplayLine = DISPLAY_Y;
            uint16_t lastDisplayLine  = DISPLAY_Y + DISPLAY_H - 1;
            if (m_rasterLine >= firstDisplayLine && m_rasterLine <= lastDisplayLine) {
                uint8_t yscroll = m_regs[SCR1] & SCR1_YSCROLL;
                if ((m_rasterLine & 7) == yscroll) {
                    *m_stallBacklog += 40 + m_badlineExtra;
                }
            }
        }

        // Check for raster IRQ
        uint16_t rasterCmp = ((m_regs[SCR1] & SCR1_RASTER8) << 1) | m_regs[RASTER];
        if (m_rasterLine == rasterCmp) {
            if (!(m_regs[IRQ] & IRQ_RST)) {
                m_regs[IRQ] |= IRQ_RST;
                updateIrq();
            }
        }
    }
}

void VIC2::log(int level, const char* msg) const {
    if (m_logger && m_logNamed) {
        m_logNamed(m_logger, level, msg);
    }
}

// ---------------------------------------------------------------------------
// IVideoOutput: renderFrame
// ---------------------------------------------------------------------------

void VIC2::renderFrame(uint32_t* buf) {
    renderBackground(buf);
    renderSprites(buf);
}

void VIC2::renderBackground(uint32_t* buf) {
    uint32_t extCol = palette(m_regs[EXTCOL] & 0x0F);
    uint32_t bg0    = palette(m_regs[BGCOL0] & 0x0F);
    uint32_t bg1    = palette(m_regs[BGCOL1] & 0x0F);
    uint32_t bg2    = palette(m_regs[BGCOL2] & 0x0F);
    uint32_t bg3    = palette(m_regs[BGCOL3] & 0x0F);

    // Fill with border color
    for (int i = 0; i < FRAME_W * FRAME_H; ++i) buf[i] = extCol;

    bool bmm = (m_regs[SCR1] & SCR1_BMM) != 0;
    bool ecm = (m_regs[SCR1] & SCR1_ECM) != 0;
    bool mcm = (m_regs[SCR2] & SCR2_MCM) != 0;

    uint32_t scrBase  = screenBase();
    uint32_t cbBase   = charBitmapBase();

    // On C64, character generator ROM is always seen by the VIC at bank 
    // offsets $1000-$1FFF and $9000-$9FFF BUT only in Banks 0 and 2.
    bool isBank0or2 = (m_bankBase == 0x0000 || m_bankBase == 0x8000);
    bool useCharRom = !bmm && isBank0or2 && (cbBase == 0x1000);

    // Logging for debug
    static int frameCounter = 0;
    if (frameCounter++ % 60 == 0) {
        char msg[256];
        std::snprintf(msg, sizeof(msg), 
            "VIC2 render: SCRBASE=$%04X CBBASE=$%04X BANK=$%04X CHARROM=%d ROMPTR=%p",
            scrBase, cbBase, m_bankBase, (int)useCharRom, (void*)m_charRom);
        log(SIM_LOG_DEBUG, msg);
    }

    for (int row = 0; row < 25; ++row) {
        for (int col = 0; col < 40; ++col) {
            int cellIdx = row * 40 + col;
            uint8_t code = dmaPeek(scrBase + cellIdx);

            int px = DISPLAY_X + col * 8;
            int py = DISPLAY_Y + row * 8;

            if (bmm && !ecm) {
                // -------------------------------------------------------
                // Bitmap modes (Standard or Multicolor)
                // -------------------------------------------------------
                uint32_t bitmapOffset = cbBase + cellIdx * 8;
                uint8_t  colorByte    = code; // screen RAM holds colors in bitmap mode

                if (!mcm) {
                    // Standard bitmap: 2 colors per 8×8 cell
                    uint8_t fgIdx = (colorByte >> 4) & 0x0F;
                    uint8_t bgIdx = colorByte & 0x0F;
                    uint32_t fgCol = palette(fgIdx);
                    uint32_t bgCol = palette(bgIdx);

                    for (int r = 0; r < 8; ++r) {
                        uint8_t bits = dmaPeek(bitmapOffset + r);
                        int fy = py + r;
                        if (fy < DISPLAY_Y || fy >= DISPLAY_Y + DISPLAY_H) continue;
                        for (int b = 7; b >= 0; --b) {
                            int fx = px + (7 - b);
                            if (fx < DISPLAY_X || fx >= DISPLAY_X + DISPLAY_W) continue;
                            buf[fy * FRAME_W + fx] = (bits >> b) & 1 ? fgCol : bgCol;
                        }
                    }
                } else {
                    // Multicolor bitmap: 4 colors per 8×8 cell, 4×8 effective resolution
                    uint8_t c00 = m_regs[BGCOL0] & 0x0F;
                    uint8_t c01 = (colorByte >> 4) & 0x0F;
                    uint8_t c10 = colorByte & 0x0F;
                    // Color RAM: direct 4-bit connection.
                    uint8_t c11 = m_colorRam ? (m_colorRam[cellIdx & 0x3FF] & 0x0F) : 0;

                    uint32_t mc[4] = { palette(c00), palette(c01), palette(c10), palette(c11) };

                    for (int r = 0; r < 8; ++r) {
                        uint8_t bits = dmaPeek(bitmapOffset + r);
                        int fy = py + r;
                        if (fy < DISPLAY_Y || fy >= DISPLAY_Y + DISPLAY_H) continue;
                        for (int pair = 0; pair < 4; ++pair) {
                            uint8_t sel = (bits >> (6 - pair * 2)) & 0x03;
                            uint32_t c = mc[sel];
                            int fx = px + pair * 2;
                            if (fx >= DISPLAY_X && fx + 1 < DISPLAY_X + DISPLAY_W) {
                                buf[fy * FRAME_W + fx]     = c;
                                buf[fy * FRAME_W + fx + 1] = c;
                            }
                        }
                    }
                }

            } else {
                // -------------------------------------------------------
                // Text modes (Standard, Multicolor, Extended background)
                // -------------------------------------------------------
                uint8_t charCode;
                uint32_t bgPx;

                if (ecm) {
                    // Extended background: char code is low 6 bits; top 2 bits select BG
                    uint8_t bgSel = (code >> 6) & 0x03;
                    charCode = code & 0x3F;
                    uint32_t bgs[4] = { bg0, bg1, bg2, bg3 };
                    bgPx = bgs[bgSel];
                } else {
                    charCode = code;
                    bgPx = bg0;
                }

                // Color RAM: direct 4-bit connection, not via DMA bus.
                uint8_t colorNibble = m_colorRam ? (m_colorRam[cellIdx & 0x3FF] & 0x0F) : 0x0F;

                uint32_t glyphOffset = cbBase + charCode * 8;

                for (int r = 0; r < 8; ++r) {
                    uint8_t bits;
                    if (useCharRom) {
                        bits = charRomByte(charCode * 8 + r);
                    } else {
                        bits = dmaPeek(glyphOffset + r);
                    }

                    int fy = py + r;
                    if (fy < DISPLAY_Y || fy >= DISPLAY_Y + DISPLAY_H) continue;

                    if (mcm && (colorNibble & 0x08)) {
                        // Multicolor: 2 bits per pixel → 4 colors, 4×8 resolution
                        uint32_t mc[4] = {
                            bgPx,
                            palette(m_regs[BGCOL1] & 0x0F),
                            palette(m_regs[BGCOL2] & 0x0F),
                            palette(colorNibble & 0x07)
                        };
                        for (int pair = 0; pair < 4; ++pair) {
                            uint8_t sel = (bits >> (6 - pair * 2)) & 0x03;
                            int fx = px + pair * 2;
                            if (fx >= DISPLAY_X && fx + 1 < DISPLAY_X + DISPLAY_W) {
                                buf[fy * FRAME_W + fx]     = mc[sel];
                                buf[fy * FRAME_W + fx + 1] = mc[sel];
                            }
                        }
                    } else {
                        // Standard text: 1 bit per pixel, fg/bg
                        uint32_t fgCol = palette(colorNibble);
                        for (int b = 7; b >= 0; --b) {
                            int fx = px + (7 - b);
                            if (fx < DISPLAY_X || fx >= DISPLAY_X + DISPLAY_W) continue;
                            buf[fy * FRAME_W + fx] = (bits >> b) & 1 ? fgCol : bgPx;
                        }
                    }
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Sprite rendering
// ---------------------------------------------------------------------------

void VIC2::renderSprites(uint32_t* buf) {
    uint32_t scrBase = screenBase();

    // Render sprites in reverse priority order (sprite 7 first, 0 last → 0 on top)
    for (int sp = 7; sp >= 0; --sp) {
        if (!(m_regs[SPENA] & (1 << sp))) continue; // sprite disabled

        // Sprite position
        uint16_t spX = (uint16_t)m_regs[SP0X + sp * 2];
        if (m_regs[MSIGX] & (1 << sp)) spX |= 0x100;
        uint8_t  spY = m_regs[SP0Y + sp * 2];

        bool xExp = (m_regs[XXPAND] & (1 << sp)) != 0;
        bool yExp = (m_regs[YXPAND] & (1 << sp)) != 0;

        uint8_t  colorIdx = m_regs[SP0COL + sp] & 0x0F;
        uint32_t spColor  = palette(colorIdx);

        // Fetch sprite pointer from end of screen RAM
        uint32_t ptrAddr = scrBase + 0x03F8 + sp;
        uint8_t  ptrVal  = dmaPeek(ptrAddr);
        uint32_t dataAddr = ptrVal * 64;

        for (int r = 0; r < 21; ++r) {
            uint32_t rowBits = (dmaPeek(dataAddr + r * 3) << 16) |
                               (dmaPeek(dataAddr + r * 3 + 1) << 8) |
                               (dmaPeek(dataAddr + r * 3 + 2));

            int py = spY + (yExp ? r * 2 : r);
            if (py < 0 || py >= FRAME_H) continue;

            for (int b = 0; b < 24; ++b) {
                bool bit = (rowBits >> (23 - b)) & 1;
                if (!bit) continue;

                int pxStart = spX + (xExp ? b * 2 : b);
                int pxEnd   = xExp ? pxStart + 2 : pxStart + 1;

                for (int px = pxStart; px < pxEnd; ++px) {
                    if (px < 0 || px >= FRAME_W) continue;
                    buf[py * FRAME_W + px] = spColor;
                }
            }
            if (yExp && (py + 1 < FRAME_H)) {
                // duplicate row for Y expansion
                for (int b = 0; b < 24; ++b) {
                    bool bit = (rowBits >> (23 - b)) & 1;
                    if (!bit) continue;
                    int pxStart = spX + (xExp ? b * 2 : b);
                    int pxEnd   = xExp ? pxStart + 2 : pxStart + 1;
                    for (int px = pxStart; px < pxEnd; ++px) {
                        if (px < 0 || px >= FRAME_W) continue;
                        buf[(py+1) * FRAME_W + px] = spColor;
                    }
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// IRQ helper
// ---------------------------------------------------------------------------

void VIC2::updateIrq() {
    bool assert = (m_regs[IRQ] & m_regs[IRQEN] & 0x0F) != 0;
    if (m_irqLine) m_irqLine->set(assert);
}

void VIC2::getDeviceInfo(DeviceInfo& out) const {
    out.name = m_name;
    out.baseAddr = m_baseAddr;
    out.addrMask = addrMask();

    auto addReg = [&](const std::string& name, Reg reg, const std::string& desc = "",
                      std::vector<BitField> bits = {}) {
        out.registers.push_back({name, (uint32_t)reg, m_regs[reg], desc, std::move(bits)});
    };

    addReg("SP0X", SP0X); addReg("SP0Y", SP0Y);
    addReg("SP1X", SP1X); addReg("SP1Y", SP1Y);
    addReg("SP2X", SP2X); addReg("SP2Y", SP2Y);
    addReg("SP3X", SP3X); addReg("SP3Y", SP3Y);
    addReg("SP4X", SP4X); addReg("SP4Y", SP4Y);
    addReg("SP5X", SP5X); addReg("SP5Y", SP5Y);
    addReg("SP6X", SP6X); addReg("SP6Y", SP6Y);
    addReg("SP7X", SP7X); addReg("SP7Y", SP7Y);
    addReg("MSIGX", MSIGX, "Sprite X MSBs");
    addReg("SCR1", SCR1, "Control 1", {
        {"YSCROLL", 0, 3}, {"RSEL", 3, 1}, {"DEN", 4, 1},
        {"BMM", 5, 1}, {"ECM", 6, 1}, {"RST8", 7, 1}
    });
    addReg("RASTER", RASTER, "Raster counter");
    addReg("LPX", LPX); addReg("LPY", LPY);
    addReg("SPENA", SPENA, "Sprite enable");
    addReg("SCR2", SCR2, "Control 2", {
        {"XSCROLL", 0, 3}, {"CSEL", 3, 1}, {"MCM", 4, 1}, {"RES", 5, 1}
    });
    addReg("YXPAND", YXPAND, "Sprite Y expansion");
    addReg("VMEM", VMEM, "Memory pointers", {
        {"CB", 1, 3}, {"VM", 4, 4}
    });
    addReg("IRQ", IRQ, "IRQ status", {
        {"IRST", 0, 1}, {"IMBC", 1, 1}, {"IMMC", 2, 1}, {"ILP", 3, 1}, {"IRQ", 7, 1}
    });
    addReg("IRQEN", IRQEN, "IRQ enable", {
        {"ERST", 0, 1}, {"EMBC", 1, 1}, {"EMMC", 2, 1}, {"ELP", 3, 1}
    });
    addReg("SPBGPR", SPBGPR, "Sprite-BG priority");
    addReg("SPMC", SPMC, "Sprite multicolor");
    addReg("XXPAND", XXPAND, "Sprite X expansion");
    addReg("SSCOL", SSCOL, "Sprite-sprite collision");
    addReg("SBCOL", SBCOL, "Sprite-BG collision");
    addReg("EXTCOL", EXTCOL, "Border color");
    addReg("BGCOL0", BGCOL0); addReg("BGCOL1", BGCOL1);
    addReg("BGCOL2", BGCOL2); addReg("BGCOL3", BGCOL3);
    addReg("BGCOL3", BGCOL3);
    addReg("SPMC0", SPMC0); addReg("SPMC1", SPMC1);
    addReg("SP0COL", SP0COL); addReg("SP1COL", SP1COL);
    addReg("SP2COL", SP2COL); addReg("SP3COL", SP3COL);
    addReg("SP4COL", SP4COL); addReg("SP5COL", SP5COL);
    addReg("SP6COL", SP6COL); addReg("SP7COL", SP7COL);

    char buf[64];
    std::sprintf(buf, "%d", m_rasterLine);
    out.state.push_back({"Raster Line", buf});
    std::sprintf(buf, "%llu", (unsigned long long)m_cycleAccum);
    out.state.push_back({"Cycle Accum", buf});
    std::sprintf(buf, "$%04X", m_bankBase);
    out.state.push_back({"Bank Base", buf});
    out.state.push_back({"Raster Line", std::to_string(m_rasterLine)});
    out.state.push_back({"Raster Cycle", std::to_string(m_cycleAccum % m_cyclesPerLine)});
    out.state.push_back({"Lines/Frame", std::to_string(m_linesPerFrame)});
    out.state.push_back({"Cycles/Line", std::to_string(m_cyclesPerLine)});

    out.dependencies.push_back({"DMA Bus", m_dmaBus ? "connected" : "none"});
    out.dependencies.push_back({"Char ROM", m_charRom ? "connected" : "none"});
    out.dependencies.push_back({"Color RAM", m_colorRam ? "connected" : "none"});
    out.dependencies.push_back({"IRQ Line", m_irqLine ? "connected" : "none"});

    // Sprites
    uint32_t scrBase = screenBase();
    for (int i = 0; i < 8; ++i) {
        bool enabled = (m_regs[SPENA] & (1 << i)) != 0;
        uint16_t x = m_regs[SP0X + i * 2];
        if (m_regs[MSIGX] & (1 << i)) x |= 0x100;
        uint8_t y = m_regs[SP0Y + i * 2];
        bool xExp = (m_regs[XXPAND] & (1 << i)) != 0;
        bool yExp = (m_regs[YXPAND] & (1 << i)) != 0;
        uint8_t colorIdx = m_regs[SP0COL + i] & 0x0F;
        uint32_t color = palette(colorIdx);

        std::string prefix = "Sprite " + std::to_string(i);
        out.state.push_back({prefix + " Enabled", enabled ? "yes" : "no"});
        out.state.push_back({prefix + " Pos", std::to_string(x) + ", " + std::to_string(y)});
        out.state.push_back({prefix + " Expansion", std::string(xExp ? "X" : "") + (yExp ? "Y" : "")});
        out.state.push_back({prefix + " Color", std::to_string((int)colorIdx)});

        // Bitmap
        DeviceBitmap db;
        db.name = prefix;
        db.width = 24;
        db.height = 21;
        db.pixels.resize(24 * 21, 0); // Transparent black

        uint32_t ptrAddr = scrBase + 0x03F8 + i;
        uint8_t  ptrVal  = dmaPeek(ptrAddr);
        uint32_t dataAddr = ptrVal * 64;

        for (int r = 0; r < 21; ++r) {
            uint32_t rowBits = (dmaPeek(dataAddr + r * 3) << 16) |
                               (dmaPeek(dataAddr + r * 3 + 1) << 8) |
                               (dmaPeek(dataAddr + r * 3 + 2));
            for (int b = 0; b < 24; ++b) {
                if ((rowBits >> (23 - b)) & 1) {
                    db.pixels[r * 24 + b] = color;
                }
            }
        }
        out.bitmaps.push_back(std::move(db));
    }
}
