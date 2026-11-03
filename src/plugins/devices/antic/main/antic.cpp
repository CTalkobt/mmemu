#include "antic.h"
#include "plugins/devices/gtia/main/gtia.h"
#include "libmem/main/ibus.h"
#include <algorithm>
#include <cstring>

namespace {

// Per ANTIC mode (2-15): scan lines per mode line, bytes per normal-width line
const int MODE_LINES[16] = { 0, 0, 8, 10, 8, 16, 8, 16, 8, 4, 4, 2, 1, 2, 1, 1 };
const int MODE_BYTES[16] = { 0, 0, 40, 40, 40, 40, 20, 20, 10, 10, 20, 20, 20, 40, 40, 40 };

// Playfield windows in pixels (half colour clocks) from colour clock 32,
// by DMACTL width: none, narrow (cc 64-191), normal (48-207), wide (32-223)
const int WIN_START[4] = { 0, 64, 32, 0 };
const int WIN_END[4]   = { 0, 320, 352, 384 };

int bytesForWidth(int mode, int width) {
    int n = MODE_BYTES[mode];
    switch (width) {
        case 1: return n * 4 / 5;   // narrow
        case 3: return n * 6 / 5;   // wide
        default: return n;
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Registers
// ---------------------------------------------------------------------------

bool Antic::ioRead(IBus* /*bus*/, uint32_t addr, uint8_t* val) {
    switch (addr & 0x0F) {
        case VCOUNT: *val = (uint8_t)(m_line >> 1); break;
        case NMIST:  *val = (uint8_t)(m_nmist | 0x1F); break;
        case PENH:   *val = m_regs[PENH]; break;
        case PENV:   *val = m_regs[PENV]; break;
        default:     *val = 0xFF; break;   // the others are write-only
    }
    return true;
}

bool Antic::ioWrite(IBus* /*bus*/, uint32_t addr, uint8_t val) {
    uint32_t r = addr & 0x0F;
    if (r == VCOUNT || r == PENH || r == PENV) return true;   // read-only
    m_regs[r] = val;
    switch (r) {
        case DLISTL: m_dlPc = (uint16_t)((m_dlPc & 0xFF00) | val); break;
        case DLISTH: m_dlPc = (uint16_t)((m_dlPc & 0x00FF) | (val << 8)); break;
        case WSYNC:  m_wsync = true; break;
        case NMIST:  m_nmist = 0; break;   // NMIRES
        default: break;
    }
    return true;
}

void Antic::reset() {
    std::memset(m_regs, 0, sizeof(m_regs));
    m_line = 0;
    m_cycle = 0;
    m_frames = 0;
    m_nmist = 0;
    m_nmiHold = 0;
    m_wsync = false;
    m_stealPending = 0;
    m_dlPc = 0;
    m_memScan = 0;
    m_instr = 0;
    m_mode = 0;
    m_row = 0;
    m_lastRow = -1;
    m_waitVbl = false;
    m_prevVscrol = false;
    m_dliThisLine = false;
    m_lineBytes = 0;
    m_hscrolShift = 0;
    m_frame.assign((size_t)FRAME_W * FRAME_H, 0xFF000000u);
    if (m_nmiLine) m_nmiLine->set(false);
}

uint8_t Antic::fetch(uint16_t addr) {
    return m_dmaBus ? m_dmaBus->read8(addr) : 0xFF;
}

void Antic::pressResetKey() {
    raiseNmi(NMI_RESET);
}

void Antic::raiseNmi(uint8_t bit) {
    // Setting the DLI or VBI status bit clears the other
    if (bit == NMI_DLI)      m_nmist = (uint8_t)((m_nmist & ~NMI_VBI) | NMI_DLI);
    else if (bit == NMI_VBI) m_nmist = (uint8_t)((m_nmist & ~NMI_DLI) | NMI_VBI);
    else                     m_nmist |= bit;
    if ((m_regs[NMIEN] & bit) || bit == NMI_RESET) {
        // Held to the next line start, so the CPU sees the edge even after a WSYNC
        if (m_nmiLine) { m_nmiLine->set(false); m_nmiLine->set(true); }
        m_nmiHold = 1;
    }
}

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------

void Antic::tick(uint64_t cycles) {
    m_totalCycles += cycles;
    for (uint64_t i = 0; i < cycles; ++i) {
        if (m_cycle == 0) startLine();
        if (m_cycle == 8 && m_dliThisLine) raiseNmi(NMI_DLI);
        if (m_cycle == RENDER_CYCLE) {
            drawLine();
            m_wsync = false;
        }
        if (++m_cycle == CYCLES_PER_LINE) {
            m_cycle = 0;
            if (++m_line == m_lines) {
                m_line = 0;
                ++m_frames;
            }
        }
        // DMA cycles are taken while the CPU waits (the scheduler's blocked
        // ticks); cycles the CPU ran do not repay them
    }
}

void Antic::startLine() {
    if (m_nmiHold) {
        m_nmiHold = 0;
        if (m_nmiLine) m_nmiLine->set(false);
    }
    m_dliThisLine = false;
    m_stealPending = 0;

    if (m_line == VBLANK_LINE) {
        raiseNmi(NMI_VBI);
        m_waitVbl = false;
        m_row = 0;
        m_lastRow = -1;          // fetch an instruction on the first display line
        m_mode = 0;
        m_prevVscrol = false;
    }
    if (m_line < FIRST_LINE || m_line >= VBLANK_LINE) return;

    m_stealPending += 9;         // memory refresh

    // Player/missile DMA
    uint8_t dma = m_regs[DMACTL];
    if (dma & (DMA_MISSILE | DMA_PLAYER)) {
        bool single = dma & DMA_PM_1LINE;
        uint16_t base = single ? (uint16_t)((m_regs[PMBASE] & 0xF8) << 8)
                               : (uint16_t)((m_regs[PMBASE] & 0xFC) << 8);
        uint16_t idx = single ? m_line : (uint16_t)(m_line >> 1);
        uint16_t mAddr = (uint16_t)(base + (single ? 0x300 : 0x180) + idx);
        uint8_t missiles = fetch(mAddr);
        ++m_stealPending;
        if (m_gtia) m_gtia->dmaMissiles(missiles);
        if (dma & DMA_PLAYER) {
            for (int p = 0; p < 4; ++p) {
                uint16_t pAddr = single ? (uint16_t)(base + 0x400 + p * 0x100 + idx)
                                        : (uint16_t)(base + 0x200 + p * 0x80 + idx);
                uint8_t d = fetch(pAddr);
                if (m_gtia) m_gtia->dmaPlayer(p, d);
            }
            m_stealPending += 4;
        }
    }

    // Display list
    if (!(dma & DMA_DL) || m_waitVbl) {
        m_mode = 0;
        return;
    }
    if (m_row > m_lastRow) fetchInstruction();
    if ((m_instr & 0x80) && m_row == m_lastRow) m_dliThisLine = true;

    // Character modes fetch glyph data every scan line, one per character
    if (m_mode >= 2 && m_mode <= 7 && (dma & DMA_PF_MASK)) m_stealPending += m_lineBytes;
}

void Antic::fetchInstruction() {
    auto next = [&]() {
        uint8_t b = fetch(m_dlPc);
        // The display list counter's low 10 bits count; the rest stay
        m_dlPc = (uint16_t)((m_dlPc & 0xFC00) | ((m_dlPc + 1) & 0x03FF));
        ++m_stealPending;
        return b;
    };
    m_instr = next();
    int mode = m_instr & 0x0F;
    m_row = 0;

    if (mode == 0) {                       // blank lines
        m_mode = 0;
        m_lastRow = (m_instr >> 4) & 7;
        return;
    }
    if (mode == 1) {                       // JMP / JVB
        uint8_t lo = next(), hi = next();
        m_dlPc = (uint16_t)(lo | (hi << 8));
        m_mode = 0;
        m_lastRow = 0;
        if (m_instr & 0x40) m_waitVbl = true;   // JVB: blank to the vertical blank
        return;
    }

    if (m_instr & 0x40) {                  // LMS
        uint8_t lo = next(), hi = next();
        m_memScan = (uint16_t)(lo | (hi << 8));
    }
    m_mode = mode;

    // Vertical fine scroll: the first scrolled line starts at VSCROL, the
    // line after the scrolled region ends at it
    bool vs = m_instr & 0x20;
    int lines = MODE_LINES[mode];
    int vscrol = m_regs[VSCROL] & 0x0F;
    int first = (vs && !m_prevVscrol) ? vscrol : 0;
    int last = (!vs && m_prevVscrol) ? vscrol : lines - 1;
    if (last < first) last = first;
    m_row = first;
    m_lastRow = last;
    m_prevVscrol = vs;

    // Fetch the mode line's bytes (character names or map data)
    int width = m_regs[DMACTL] & DMA_PF_MASK;
    bool hs = m_instr & 0x10;
    int fetchWidth = (hs && width && width < 3) ? width + 1 : width;
    m_lineBytes = width ? bytesForWidth(mode, fetchWidth) : 0;
    for (int i = 0; i < m_lineBytes && i < (int)sizeof(m_lineData); ++i)
        m_lineData[i] = fetch((uint16_t)((m_memScan & 0xF000) | ((m_memScan + i) & 0x0FFF)));
    m_memScan = (uint16_t)((m_memScan & 0xF000) | ((m_memScan + m_lineBytes) & 0x0FFF));
    m_stealPending += m_lineBytes;
    m_hscrolShift = hs ? (m_regs[HSCROL] & 0x0F) * 2 : 0;
    m_hsFetchWidth = fetchWidth;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void Antic::buildPlayfieldLine(uint8_t* pf) {
    std::memset(pf, GTIA::PF_BK, GTIA::LINE_PIXELS);
    int width = m_regs[DMACTL] & DMA_PF_MASK;
    if (m_mode < 2 || !width || (m_regs[DMACTL] & DMA_DL) == 0 || m_waitVbl) return;

    const int ws = WIN_START[width], we = WIN_END[width];
    int x = WIN_START[m_hsFetchWidth] + m_hscrolShift;
    auto put = [&](uint8_t v) {
        if (x >= ws && x < we) pf[x] = v;
        ++x;
    };
    const int mode = m_mode;
    const uint8_t chactl = m_regs[CHACTL];
    const bool hires = mode == 2 || mode == 3 || mode == 15;
    const uint8_t hiresOff = GTIA::PF_2 | GTIA::PF_HIRES_MODE;
    const uint8_t hiresOn = hiresOff | GTIA::PF_HIRES;

    if (hires) {
        for (int i = ws; i < we; ++i) pf[i] = hiresOff;   // the hires playfield is PF2
    }

    if (mode <= 7) {
        // Character modes: glyph row for this scan line
        int row = m_row;
        if (mode == 5 || mode == 7) row >>= 1;
        bool blankRow = false;
        for (int i = 0; i < m_lineBytes; ++i) {
            uint8_t name = m_lineData[i];
            int r = row;
            if (mode == 3) {
                // 10 scan lines: $60-$7F (descenders) blank lines 0-1 and put
                // rows 0-1 at the bottom; the others blank lines 8-9
                bool desc = (name & 0x60) == 0x60;
                if (desc) { blankRow = m_row < 2; r = m_row >= 8 ? m_row - 8 : m_row; }
                else      { blankRow = m_row >= 8; r = m_row; }
            }
            if (chactl & 0x04) r = 7 - (r & 7);           // upside down
            r &= 7;
            uint16_t chbase = (mode >= 6) ? (uint16_t)((m_regs[CHBASE] & 0xFE) << 8)
                                          : (uint16_t)((m_regs[CHBASE] & 0xFC) << 8);
            uint8_t code = (mode >= 6) ? (name & 0x3F) : (name & 0x7F);
            uint8_t data = blankRow ? 0 : fetch((uint16_t)(chbase + code * 8 + r));

            if (mode == 2 || mode == 3) {
                if (name & 0x80) {
                    if (chactl & 0x01) data = 0;          // inverse characters blank
                    if (chactl & 0x02) data ^= 0xFF;      // inverse video
                }
                for (int b = 7; b >= 0; --b) put((data >> b) & 1 ? hiresOn : hiresOff);
            } else if (mode == 4 || mode == 5) {
                for (int b = 6; b >= 0; b -= 2) {
                    int v = (data >> b) & 3;
                    static const uint8_t CLS[4] = { GTIA::PF_BK, GTIA::PF_0, GTIA::PF_1, GTIA::PF_2 };
                    uint8_t c = (v == 3 && (name & 0x80)) ? (uint8_t)GTIA::PF_3 : CLS[v];
                    put(c); put(c);
                }
            } else {   // 6, 7: five colours, the character's top bits pick PF0-PF3
                uint8_t fg = (uint8_t)(GTIA::PF_0 + (name >> 6));
                for (int b = 7; b >= 0; --b) {
                    uint8_t c = (data >> b) & 1 ? fg : (uint8_t)GTIA::PF_BK;
                    put(c); put(c);
                }
            }
        }
        return;
    }

    // Map modes
    for (int i = 0; i < m_lineBytes; ++i) {
        uint8_t d = m_lineData[i];
        switch (mode) {
            case 8: case 10: case 13: case 14: {      // 2 bits a pixel
                int w = mode == 8 ? 8 : mode == 10 ? 4 : 2;
                for (int b = 6; b >= 0; b -= 2) {
                    int v = (d >> b) & 3;
                    uint8_t c = v == 0 ? (uint8_t)GTIA::PF_BK : (uint8_t)(GTIA::PF_0 + v - 1);
                    for (int k = 0; k < w; ++k) put(c);
                }
                break;
            }
            case 9: case 11: case 12: {               // 1 bit a pixel: BK / PF0
                int w = mode == 9 ? 4 : 2;
                for (int b = 7; b >= 0; --b) {
                    uint8_t c = (d >> b) & 1 ? GTIA::PF_0 : GTIA::PF_BK;
                    for (int k = 0; k < w; ++k) put(c);
                }
                break;
            }
            default:                                  // 15: hires, 1 bit a pixel
                for (int b = 7; b >= 0; --b) put((d >> b) & 1 ? hiresOn : hiresOff);
                break;
        }
    }
}

void Antic::drawLine() {
    if (m_line < FIRST_LINE || m_line >= VBLANK_LINE) return;
    uint8_t pf[GTIA::LINE_PIXELS];
    buildPlayfieldLine(pf);
    uint32_t* out = &m_frame[(size_t)(m_line - FIRST_LINE) * FRAME_W];
    if (m_gtia) {
        m_gtia->renderLine(pf, out);
    } else {
        for (int i = 0; i < FRAME_W; ++i) out[i] = pf[i] ? 0xFFFFFFFFu : 0xFF000000u;
    }
    if ((m_regs[DMACTL] & DMA_DL) && !m_waitVbl) ++m_row;
}

void Antic::renderFrame(uint32_t* buffer) {
    std::memcpy(buffer, m_frame.data(), m_frame.size() * sizeof(uint32_t));
}
