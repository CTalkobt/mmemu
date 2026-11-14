#include <cstdlib>
#include <iterator>
#include <algorithm>
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
    m_keyLines = 0x07; m_d030 = 0;      // VIC-IIe: no K line selected, 1 MHz
    // Power-on state: display enabled, 25 rows, 40 cols, standard text
    m_regs[SCR1] = SCR1_DEN | SCR1_RSEL | 0x03; // scroll Y = 3
    m_regs[SCR2] = SCR2_CSEL | 0x08;             // scroll X = 0, CSEL=1
    m_regs[EXTCOL] = 0x0E; // light blue border (common C64 default)
    m_regs[BGCOL0] = 0x06; // blue background
    m_regs[VMEM]   = 0x14; // screen at $0400, char at $1000

    m_cycleAccum = 0;
    m_rasterLine = 0;
    m_denOnLine30 = false;
    m_sprSprColl = 0;
    m_sprBgColl  = 0;
    m_seq = SpriteSeq{};
    m_vborder = true;
    m_lpLow = m_lpPending = m_lpTriggered = false;
    m_frameStartPending = false;
    m_collT = 0;

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
    if (m_ultimax) return ultimaxPeek((m_bankBase + offset) & 0xFFFF);

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

uint8_t VIC2::peekVideo(uint32_t addr) const {
    if (!m_dmaBus) return 0xFF;
    addr &= 0xFFFF;
    uint32_t bank = addr & 0xC000, offset = addr & 0x3FFF;
    if (m_ultimax) return ultimaxPeek(addr);
    // Character ROM shadow at $1000-$1FFF of banks 0 and 2 (see dmaPeek)
    if (m_charRom && (bank == 0x0000 || bank == 0x8000) && offset >= 0x1000 && offset <= 0x1FFF)
        return m_charRom[offset & 0x0FFF];
    return m_dmaBus->peek8(addr);
}

uint8_t VIC2::ultimaxPeek(uint32_t addr) const {
    if ((addr & 0x3000) == 0x3000) {                       // ROMH $1000-$1FFF
        uint32_t off = addr & 0x1FFF;
        return m_ultimaxRomh && off < m_ultimaxRomhSize ? m_ultimaxRomh[off] : 0xFF;
    }
    return m_dmaBus->peek8(addr);
}

bool VIC2::spriteInfo(int n, SpriteInfo& out) const {
    if (n < 0 || n > 7) return false;
    out = SpriteInfo{};
    out.enabled    = (m_regs[SPENA] >> n) & 1;
    out.multicolor = (m_regs[SPMC] >> n) & 1;
    out.color      = m_regs[SP0COL + n] & 0x0F;
    out.mc0        = m_regs[SPMC0] & 0x0F;
    out.mc1        = m_regs[SPMC1] & 0x0F;
    uint8_t ptr    = peekVideo(videoAddr(screenBase() + 0x03F8 + n));
    out.dataAddr   = videoAddr((uint32_t)ptr * 64);
    return true;
}

bool VIC2::charsetInfo(CharsetInfo& out) const {
    out = CharsetInfo{};
    out.addr       = videoAddr(charBitmapBase());
    out.multicolor = (m_regs[SCR2] & SCR2_MCM) != 0;
    out.bitmap     = (m_regs[SCR1] & SCR1_BMM) != 0;
    for (int i = 0; i < 4; ++i) out.bg[i] = m_regs[BGCOL0 + i] & 0x0F;
    return true;
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

bool VIC2::ioPeek(IBus* bus, uint32_t addr, uint8_t* val) {
    // The (virtual) ioRead of this chip or a VIC-III/IV, with read side effects off
    m_peeking = true;
    bool handled = ioRead(bus, addr, val);
    m_peeking = false;
    return handled;
}

bool VIC2::ioRead(IBus* /*bus*/, uint32_t addr, uint8_t* val) {
    if ((addr & ~(addrMask() | m_decodeMask)) != m_baseAddr) return false;

    uint8_t reg = addr & 0x3F;

    if (m_vicIIe && reg == 0x2F) { *val = 0xF8 | m_keyLines; return true; }   // VICE vicii-mem.c
    if (m_vicIIe && reg == 0x30) { *val = 0xFC | m_d030; return true; }
    if (reg > 0x2E) {
        // $D02F–$D03F: unused, read as $FF
        *val = 0xFF;
        return true;
    }

    switch (reg) {
        case RASTER:
            *val = (uint8_t)(visibleLine() & 0xFF);
            break;
        case SCR1:
            *val = (m_regs[SCR1] & ~SCR1_RASTER8) |
                   ((visibleLine() >> 8) ? SCR1_RASTER8 : 0);
            break;
        case IRQ:
            // Bit 7 is set if any enabled IRQ source is active; bits 4-6
            // aren't there and read 1 (VICE: irq_status | 0x70). The
            // collisions so far count.
            if (!m_peeking && (m_regs[SPENA] || m_seq.active)) evalCollisions(m_rasterLine, collReadT());
            *val = 0x70 | m_regs[IRQ] | ((m_regs[IRQ] & m_regs[IRQEN] & 0x0F) ? IRQ_ANY : 0);
            break;
        case SSCOL:
            if (!m_peeking && (m_regs[SPENA] || m_seq.active)) evalCollisions(m_rasterLine, collReadT());
            *val = m_sprSprColl;
            if (!m_peeking) m_sprSprColl = 0; // cleared on read
            break;
        case SBCOL:
            if (!m_peeking && (m_regs[SPENA] || m_seq.active)) evalCollisions(m_rasterLine, collReadT());
            *val = m_sprBgColl;
            if (!m_peeking) m_sprBgColl = 0;
            break;
        // Bits that aren't there read 1 (VICE vicii-mem.c): code run from the
        // registers depends on it (testprogs VICII/lplatency runs $D017-$D01C)
        case SCR2:  *val = m_regs[SCR2] | 0xC0; break;
        case VMEM:  *val = m_regs[VMEM] | 0x01; break;
        case IRQEN: *val = m_regs[IRQEN] | 0xF0; break;
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
    if ((addr & ~(addrMask() | m_decodeMask)) != m_baseAddr) return false;

    uint8_t reg = addr & 0x3F;
    if (m_vicIIe && reg == 0x2F) { m_keyLines = val & 0x07; return true; }
    if (m_vicIIe && reg == 0x30) { m_d030 = val & 0x03; return true; }
    if (reg > 0x2E) return true; // writes to unused regs are ignored

    // The sprite sequencer runs to this cycle on the registers as they were
    if ((reg <= MSIGX || reg == SPENA || reg == SPBGPR || reg == SPMC || reg == XXPAND || reg == YXPAND ||
         reg == IRQ || reg == IRQEN) &&
        (m_regs[SPENA] || m_seq.active))
        evalCollisions(m_rasterLine, collReadT());

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
        case LPX:
        case LPY:
            // Read-only collision and light pen registers; writes are ignored.
            break;
        default:
            m_regs[reg] = val;
            if (reg == SCR1 && m_rasterLine == 0x30 && (val & SCR1_DEN)) m_denOnLine30 = true;
            break;
    }
    return true;
}

// ---------------------------------------------------------------------------
// BA (#266): VICE viciisc vicii-chip-model.c, the BaSpr columns of
// cycle_tab_pal and cycle_tab_ntsc (6567R8): per cycle (from 0), the sprites
// whose fetches hold BA low; cycles 12-54 (11-53 here) are the badline's
// ---------------------------------------------------------------------------
namespace {
constexpr uint8_t S(int a) { return (uint8_t)(1 << a); }
constexpr uint8_t S(int a, int b) { return (uint8_t)(S(a) | S(b)); }
constexpr uint8_t S(int a, int b, int c) { return (uint8_t)(S(a, b) | S(c)); }
const uint8_t BA_PAL[63] = {
    S(3, 4), S(3, 4, 5), S(4, 5), S(4, 5, 6), S(5, 6), S(5, 6, 7), S(6, 7), S(6, 7), S(7), S(7), 0,   // 1-11
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,                                // 12-54
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    S(0), S(0), S(0, 1), S(0, 1), S(0, 1, 2), S(1, 2), S(1, 2, 3), S(2, 3), S(2, 3, 4)};          // 55-63
const uint8_t BA_NTSC[65] = {
    S(3, 4, 5), S(4, 5), S(4, 5, 6), S(5, 6), S(5, 6, 7), S(6, 7), S(6, 7), S(7), S(7), 0, 0,        // 1-11
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,                                // 12-54
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, S(0), S(0), S(0, 1), S(0, 1), S(0, 1, 2), S(1, 2), S(1, 2, 3), S(2, 3), S(2, 3, 4), S(3, 4)}; // 55-65
}  // namespace

bool VIC2::isBadline(int line) const {
    if (!m_badlineEnable || line < 0x30 || line > 0xF7) return false;
    const bool allowed = m_denOnLine30 || (line == 0x30 && (m_regs[SCR1] & SCR1_DEN));
    return allowed && (line & 7) == (m_regs[SCR1] & SCR1_YSCROLL);
}

// Sprite n's DMA is on for the fetches of `row` (the line its DMA check
// matched Y, through 21 lines, 42 expanded); Y matches the raster's low 8
// bits, so a sprite high enough comes round again below line 255
bool VIC2::spriteDma(int n, int row) const {
    const int y = m_regs[SP0Y + 2 * n];
    const int h = (m_regs[YXPAND] >> n) & 1 ? 42 : 21;
    for (int start = y; start < m_linesPerFrame; start += 256) {
        int d = row - start;
        if (d < 0) d += m_linesPerFrame;              // a sprite started late in the last frame
        if (d < h) return true;
    }
    return false;
}

bool VIC2::baAt(int line, int cycle) const {
    if (cycle >= 11 && cycle <= 53) return isBadline(line);
    const uint8_t* tab = m_cyclesPerLine == NTSC_CYCLES_PER_LINE ? BA_NTSC : BA_PAL;
    const uint8_t mask = (uint8_t)(tab[cycle % m_cyclesPerLine] & m_regs[SPENA]);
    if (!mask) return false;
    // Fetches at the start of a line belong to the previous line's DMA
    int row = cycle < 11 ? line - 1 : line;
    if (row < 0) row += m_linesPerFrame;
    for (int n = 0; n < 8; ++n)
        if ((mask >> n) & 1 && spriteDma(n, row)) return true;
    return false;
}

// The line and cycle (from 0) `ahead` cycles after the next to run.
// m_cycleAccum is the cycle last ticked (the one a CPU access in it sees).
void VIC2::cycleAhead(int ahead, int& line, int& cycle) const {
    line = m_rasterLine;
    int64_t c = (int64_t)(m_cycleAccum % (uint64_t)m_cyclesPerLine) + 1 + ahead;
    while (c >= m_cyclesPerLine) {
        c -= m_cyclesPerLine;
        if (++line >= m_linesPerFrame) line = 0;
    }
    while (c < 0) {                                   // `ahead` < 0: cycles already run
        c += m_cyclesPerLine;
        line = line == 0 ? m_linesPerFrame - 1 : line - 1;
    }
    cycle = (int)c;
}

bool VIC2::baLow(int ahead) const {
    if (fastMode()) return false;                     // the VIC-IIe at 2 MHz takes nothing
    int line, c;
    cycleAhead(ahead, line, c);
    return baAt(line, c);
}

// VICE vicii_cycle_reu (x64sc): where sprite 0's DMA turns on, the VIC-II
// sets BA late, so the REU still gets that first cycle; the CPU checks
// later and doesn't (VICE testprogs REU/reutiming2 e-m2, e3-m2)
bool VIC2::baLowForDma(int ahead) const {
    if (!baLow(ahead)) return false;
    int line, c;
    cycleAhead(ahead, line, c);
    const int first = m_cyclesPerLine == NTSC_CYCLES_PER_LINE ? 55 : 54;
    if (c != first || !(m_regs[SPENA] & 1)) return true;
    const int y = m_regs[SP0Y];
    const bool starts = (line & 0xFF) == y && !spriteDma(0, line == 0 ? m_linesPerFrame - 1 : line - 1);
    return !starts;
}

// ---------------------------------------------------------------------------
// IOHandler: tick — drives the raster counter and fires raster IRQ
// ---------------------------------------------------------------------------

void VIC2::tick(uint64_t cycles) {
    // With sprites on, cycle by cycle: the graphics and sprite data are taken
    // as fetched, for the collisions (memory changed during a line counts
    // from the cycle it changed: VICE testprogs REU/bonzai checkchar)
    if (m_regs[SPENA] || m_seq.active || m_lpPending) {
        while (cycles--) {
            ++m_ticks;
            if (++m_cycleAccum >= (uint64_t)m_cyclesPerLine) {
                m_cycleAccum -= m_cyclesPerLine;
                nextLine();
            }
            if (m_frameStartPending && m_cycleAccum >= 1) frameStart();
            if (m_lpPending && m_ticks >= m_lpAt) { m_lpPending = false; triggerLightPen(false); }
            if (m_regs[SPENA] || m_seq.active) {
                captureFetch((int)m_cycleAccum);
                // A collision IRQ that is enabled and not yet flagged is
                // raised in the cycle the collision happens, not when the
                // line ends (testprogs interrupts/irq-ackn-bug/irq-ack-vicii)
                if (((m_regs[IRQEN] & IRQ_MMC) && !(m_regs[IRQ] & IRQ_MMC)) ||
                    ((m_regs[IRQEN] & IRQ_MBC) && !(m_regs[IRQ] & IRQ_MBC)))
                    evalCollisions(m_rasterLine, collReadT());
            }
        }
        return;
    }
    m_ticks += cycles;
    m_cycleAccum += cycles;
    while (m_cycleAccum >= (uint64_t)m_cyclesPerLine) {
        m_cycleAccum -= m_cyclesPerLine;
        if (m_frameStartPending) frameStart();
        nextLine();
    }
    if (m_frameStartPending && m_cycleAccum >= 1) frameStart();
}

// The fetches of cycle c (from 0) of this line that the collisions use: a
// g-access (cycles 15-54) and the second data cycle of a sprite
void VIC2::captureFetch(int c) {
    if (c >= 15 && c <= 54) {
        uint8_t g; bool mc;
        if (gfxFetch(m_rasterLine, c - 15, g, mc)) m_gCap[c - 15] = (int16_t)(g | (mc ? 0x100 : 0));
    }
    const int n = sprDma0In(c == 0 ? m_cyclesPerLine - 1 : c - 1);
    if (n >= 0) {
        uint32_t data;
        if (spriteData(n, c, data)) m_sCap[n] = (int32_t)data;
    }
}

// The graphics byte at column `col` of `line` and whether it is multicolour;
// false while the vertical border is on (VICE draw_graphics8 loads no
// graphics then). Outside the display rows it is the idle fetch ($3FFF,
// $39FF with ECM; no colour, so multicolour only in bitmap mode): it shows,
// and collides, in an opened border (VICE testprogs REU/bonzai spritetiming)
bool VIC2::gfxFetch(int line, int col, uint8_t& g, bool& mc) const {
    (void)col;
    if (m_vborder) return false;
    const uint8_t scr1 = m_regs[SCR1], scr2 = m_regs[SCR2];
    const int rel = line - (0x30 + (scr1 & SCR1_YSCROLL));
    if (!m_denOnLine30 || line >= 0xF8 || rel < 0 || rel / 8 >= 25) {
        g = dmaPeek((scr1 & SCR1_ECM) ? 0x39FF : 0x3FFF);
        mc = (scr1 & SCR1_BMM) && (scr2 & SCR2_MCM);
        return true;
    }
    const int cell = (rel / 8) * 40 + col, rl = rel % 8;
    const bool bmm = scr1 & SCR1_BMM, ecm = scr1 & SCR1_ECM, mcm = scr2 & SCR2_MCM;
    const uint32_t cbBase = charBitmapBase();
    const bool bank02 = m_bankBase == 0x0000 || m_bankBase == 0x8000;
    const bool useCharRom = m_charRom && !bmm && bank02 && cbBase == 0x1000;
    const uint8_t code = dmaPeek(screenBase() + cell);
    const uint8_t colour = m_colorRam ? (m_colorRam[cell & 0x3FF] & 0x0F) : 0x0F;
    if (bmm) {
        g = dmaPeek(cbBase + cell * 8 + rl);
        mc = mcm;
    } else {
        const uint8_t ch = ecm ? (code & 0x3F) : code;
        g = useCharRom ? charRomByte(ch * 8 + rl) : dmaPeek(cbBase + ch * 8 + rl);
        mc = mcm && (colour & 0x08);
    }
    return true;
}

// Sprite n's 24 data bits as fetched in cycle c (from 0) of this line, if its
// DMA is on (sprites 3-7 fetch at the start of a line for the line before)
bool VIC2::spriteData(int n, int c, uint32_t& data) const {
    int fetchRow = c < 11 ? m_rasterLine - 1 : m_rasterLine;
    if (fetchRow < 0) fetchRow += m_linesPerFrame;
    const bool yExp = (m_regs[YXPAND] >> n) & 1;
    const int y = m_regs[SP0Y + 2 * n], h = yExp ? 42 : 21;
    int dd = -1;
    for (int start = y; start < m_linesPerFrame && dd < 0; start += 256) {
        int k = fetchRow - start;
        if (k < 0) k += m_linesPerFrame;
        if (k < h) dd = k;
    }
    if (dd < 0) return false;
    const uint32_t a = (uint32_t)dmaPeek(screenBase() + 0x03F8 + n) * 64 + (yExp ? dd / 2 : dd) * 3;
    data = ((uint32_t)dmaPeek(a) << 16) | ((uint32_t)dmaPeek(a + 1) << 8) | dmaPeek(a + 2);
    return true;
}

// The end of a line: its collisions, then the next line's raster events
void VIC2::nextLine() {
    {
        if (m_regs[SPENA] || m_seq.active) evalCollisions(m_rasterLine, lineDots());
        m_collT = 0;
        std::fill(std::begin(m_gCap), std::end(m_gCap), (int16_t)-1);
        std::fill(std::begin(m_sCap), std::end(m_sCap), (int32_t)-1);
        m_rasterLine++;

        if (m_rasterLine >= (uint16_t)m_linesPerFrame) {
            m_rasterLine = 0;
            m_frameStartPending = true;         // the counter reads 0 from cycle 1
        }
        if (m_rasterLine == 0x30) m_denOnLine30 = (m_regs[SCR1] & SCR1_DEN) != 0;
        // The vertical border flip-flop (VICE check_vborder_top/bottom): on at
        // the bottom line for RSEL, off at the top one if DEN is set; moving
        // RSEL past the bottom line keeps it off (an opened border)
        {
            const bool rsel = m_regs[SCR1] & SCR1_RSEL;
            if (m_rasterLine == (rsel ? 0xFB : 0xF7)) m_vborder = true;
            if (m_rasterLine == (rsel ? 0x33 : 0x37) && (m_regs[SCR1] & SCR1_DEN)) m_vborder = false;
        }
        if (m_rasterLine == 0xF8) m_denOnLine30 = false;

        // Badline detection (#20): a badline occurs when the raster is in the
        // display area, DEN is set, and the lower 3 bits of the raster line
        // match the Y-scroll value.  On a badline the VIC steals ~40 bus
        // cycles from the CPU to fetch character pointers.
        if (m_stallBacklog && isBadline(m_rasterLine))
            *m_stallBacklog += 40 + m_badlineExtra;

        // Check for raster IRQ (line 0's waits for the frame start)
        if (!m_frameStartPending) rasterCompare();
    }
}

void VIC2::rasterCompare() {
    uint16_t rasterCmp = ((m_regs[SCR1] & SCR1_RASTER8) << 1) | m_regs[RASTER];
    if (m_rasterLine == rasterCmp) {
        if (!(m_regs[IRQ] & IRQ_RST)) {
            m_regs[IRQ] |= IRQ_RST;
            updateIrq();
        }
    }
}

// The frame starts in cycle 1 of line 0 (VICE vicii_cycle_start_of_frame):
// until then the raster counter still reads the last line. The light pen can
// trigger again, at once if its line is still low; line 0's raster compare
void VIC2::frameStart() {
    m_frameStartPending = false;
    m_lpTriggered = false;
    if (m_lpLow) triggerLightPen(true);
    rasterCompare();
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
    renderSprites(buf, m_fgMask.data());
}

void VIC2::renderBackground(uint32_t* buf) {
    uint32_t extCol = palette(m_regs[EXTCOL] & 0x0F);
    uint32_t bg0    = palette(m_regs[BGCOL0] & 0x0F);
    uint32_t bg1    = palette(m_regs[BGCOL1] & 0x0F);
    uint32_t bg2    = palette(m_regs[BGCOL2] & 0x0F);
    uint32_t bg3    = palette(m_regs[BGCOL3] & 0x0F);

    // Fill with border color
    for (int i = 0; i < FRAME_W * FRAME_H; ++i) buf[i] = extCol;
    // Foreground mask for sprite-background priority: hires 1 bits and
    // multicolour 10/11 pairs are foreground; 0 and 01 are background.
    m_fgMask.assign((size_t)FRAME_W * FRAME_H, 0);
    uint8_t* fg = m_fgMask.data();

    bool bmm = (m_regs[SCR1] & SCR1_BMM) != 0;
    bool ecm = (m_regs[SCR1] & SCR1_ECM) != 0;
    bool mcm = (m_regs[SCR2] & SCR2_MCM) != 0;

    uint32_t scrBase  = screenBase();
    uint32_t cbBase   = charBitmapBase();

    // On C64, character generator ROM is always seen by the VIC at bank 
    // offsets $1000-$1FFF and $9000-$9FFF BUT only in Banks 0 and 2.
    bool isBank0or2 = (m_bankBase == 0x0000 || m_bankBase == 0x8000);
    // Without a character ROM of its own (the C128 maps it in its DMA bus) every fetch goes through the bus
    bool useCharRom = m_charRom && !bmm && isBank0or2 && (cbBase == 0x1000);

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
                            bool on = (bits >> b) & 1;
                            buf[fy * FRAME_W + fx] = on ? fgCol : bgCol;
                            fg[fy * FRAME_W + fx] = on;
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
                                fg[fy * FRAME_W + fx] = fg[fy * FRAME_W + fx + 1] = (sel >= 2);
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
                                fg[fy * FRAME_W + fx] = fg[fy * FRAME_W + fx + 1] = (sel >= 2);
                            }
                        }
                    } else {
                        // Standard text: 1 bit per pixel, fg/bg
                        uint32_t fgCol = palette(colorNibble);
                        for (int b = 7; b >= 0; --b) {
                            int fx = px + (7 - b);
                            if (fx < DISPLAY_X || fx >= DISPLAY_X + DISPLAY_W) continue;
                            bool on = (bits >> b) & 1;
                            buf[fy * FRAME_W + fx] = on ? fgCol : bgPx;
                            fg[fy * FRAME_W + fx] = on;
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

void VIC2::renderSprites(uint32_t* buf, const uint8_t* fgMask) {
    if (!m_regs[SPENA]) return;
    uint32_t scrBase = screenBase();
    uint32_t mc0 = palette(m_regs[SPMC0] & 0x0F);
    uint32_t mc1 = palette(m_regs[SPMC1] & 0x0F);

    // The highest-priority (lowest-numbered) sprite with a non-transparent
    // pixel decides it; its $D01B bit then decides whether a background
    // foreground pixel covers it.
    std::vector<uint32_t> col((size_t)FRAME_W * FRAME_H);
    std::vector<int8_t>   owner((size_t)FRAME_W * FRAME_H, -1);

    for (int sp = 7; sp >= 0; --sp) {           // 0 drawn last -> on top
        if (!(m_regs[SPENA] & (1 << sp))) continue;

        int spX = m_regs[SP0X + sp * 2] | ((m_regs[MSIGX] & (1 << sp)) ? 0x100 : 0);
        int spY = m_regs[SP0Y + sp * 2];
        int fx0 = spX - SPRITE_X_ORIGIN + DISPLAY_X;
        int fy0 = spY - SPRITE_Y_ORIGIN + DISPLAY_Y;

        bool xExp  = (m_regs[XXPAND] & (1 << sp)) != 0;
        bool yExp  = (m_regs[YXPAND] & (1 << sp)) != 0;
        bool multi = (m_regs[SPMC]   & (1 << sp)) != 0;
        uint32_t spColor = palette(m_regs[SP0COL + sp] & 0x0F);
        // Multicolour pairs: 01 = $D025, 10 = sprite colour, 11 = $D026
        const uint32_t mcCols[4] = {0, mc0, spColor, mc1};

        uint32_t dataAddr = (uint32_t)dmaPeek(scrBase + 0x03F8 + sp) * 64;
        int xScale = xExp ? 2 : 1;
        int yScale = yExp ? 2 : 1;

        for (int r = 0; r < 21; ++r) {
            uint32_t rowBits = ((uint32_t)dmaPeek(dataAddr + r * 3) << 16) |
                               ((uint32_t)dmaPeek(dataAddr + r * 3 + 1) << 8) |
                                (uint32_t)dmaPeek(dataAddr + r * 3 + 2);
            if (!rowBits) continue;
            for (int dy = 0; dy < yScale; ++dy) {
                int py = fy0 + r * yScale + dy;
                if (py < DISPLAY_Y || py >= DISPLAY_Y + DISPLAY_H) continue;
                for (int b = 0; b < 24; ++b) {
                    uint32_t c;
                    if (multi) {
                        int sel = (rowBits >> (22 - (b & ~1))) & 3;   // pixel pairs
                        if (!sel) continue;
                        c = mcCols[sel];
                    } else {
                        if (!((rowBits >> (23 - b)) & 1)) continue;
                        c = spColor;
                    }
                    for (int dx = 0; dx < xScale; ++dx) {
                        int px = fx0 + b * xScale + dx;
                        if (px < DISPLAY_X || px >= DISPLAY_X + DISPLAY_W) continue;
                        col[(size_t)py * FRAME_W + px] = c;
                        owner[(size_t)py * FRAME_W + px] = (int8_t)sp;
                    }
                }
            }
        }
    }

    for (size_t i = 0; i < owner.size(); ++i) {
        int sp = owner[i];
        if (sp < 0) continue;
        if (fgMask && (m_regs[SPBGPR] & (1 << sp)) && fgMask[i]) continue;
        buf[i] = col[i];
    }
}

// ---------------------------------------------------------------------------
// Sprite collisions: VICE's sprite sequencer (viciisc vicii-draw-cycle.c
// draw_sprites8), run pixel by pixel over a line's pixel times
// ---------------------------------------------------------------------------

// The X the VIC-II puts out in cycle c (from 0) of a line, rounded down to 8:
// VICE's cycle_tab_pal / cycle_tab_ntsc Phi1 xpos
int VIC2::cycleXpos(int c) const {
    if (m_cyclesPerLine == NTSC_CYCLES_PER_LINE) return c <= 12 ? 0x198 + 8 * c : (8 * (c - 13)) & 0x1F8;
    return c <= 12 ? 0x190 + 8 * c : 8 * (c - 13);
}

// The sprite whose pointer and first data byte are fetched in cycle c (from
// 0), and the one whose last two bytes are; -1 for none (VICE SprPtr/SprDma0,
// SprDma1/SprDma2: PAL sprite 0 in cycles 58 and 59, 3 in cycles 1 and 2)
int VIC2::sprDma0In(int c) const {
    const int first = m_cyclesPerLine == NTSC_CYCLES_PER_LINE ? 58 : 57;    // sprite 0's (from 0)
    for (int n = 0; n < 8; ++n)
        if ((first + 2 * n) % m_cyclesPerLine == c) return n;
    return -1;
}

// In sprite X: a graphics pixel at 24 + XSCROLL + 8 * column + bit. A sprite
// starts when the X put out equals its X (as the X register was a cycle
// earlier), shifts a bit a pixel (two when X expanded, pairs when
// multicolour), and pauses while its own pointer and data are fetched (the
// "sprite gap"). Any two sprites' pixels meeting set $D01E (border
// included); a sprite pixel on a foreground graphics pixel (hires 1,
// multicolour 10/11) inside the display window sets $D01F. Each register
// raises its IRQ when it goes from 0.
void VIC2::evalCollisions(int line, int tTo) {
    const int tFrom = m_collT;
    if (tTo > lineDots()) tTo = lineDots();
    if (tTo <= tFrom) return;
    m_collT = tTo;

    // Foreground graphics on this line (pixel time), inside the display
    // window: as fetched where the line has got to, else from memory now
    // (the side border hides graphics but doesn't stop their collisions)
    uint8_t fg[65 * 8] = {};
    const int xs = m_regs[SCR2] & 7;
    for (int col = 0; col < 40; ++col) {
        uint8_t g; bool mc;
        if (line == m_rasterLine && m_gCap[col] >= 0) { g = (uint8_t)m_gCap[col]; mc = m_gCap[col] & 0x100; }
        else if (!gfxFetch(line, col, g, mc)) break;
        if (!g) continue;
        const int x0 = 24 + xs + col * 8;
        for (int p = 0; p < 8; ++p) {
            const bool on = mc ? ((g >> (6 - (p & ~1))) & 2) != 0 : ((g >> (7 - p)) & 1) != 0;
            if (on) fg[xToT(x0 + p)] = 1;
        }
    }

    SpriteSeq& q = m_seq;
    uint8_t ss = 0, sb = 0;
    for (int t = tFrom; t < tTo; ++t) {
        const int c = t >> 3, i = t & 7;
        const int xpos = cycleXpos(c);
        const int d0 = sprDma0In(c);
        const int d2 = sprDma0In(c == 0 ? m_cyclesPerLine - 1 : c - 1);   // the cycle after a sprite's first
        const uint8_t dma0 = d0 >= 0 ? (uint8_t)(1 << d0) : 0, dma2 = d2 >= 0 ? (uint8_t)(1 << d2) : 0;
        if (i == 2) q.active &= (uint8_t)~dma2;
        if (i == 3) q.halt |= dma0;
        if (i == 4) {
            if (c == (m_cyclesPerLine == NTSC_CYCLES_PER_LINE ? 58 : 57)) {   // ChkSprDisp
                q.pending = 0;
                for (int n = 0; n < 8; ++n)
                    if ((m_regs[SPENA] >> n) & 1 && spriteDma(n, line)) q.pending |= (uint8_t)(1 << n);
            }
            if (d2 >= 0) {                                              // as fetched, if it was
                uint32_t data;
                if (line == m_rasterLine && m_sCap[d2] >= 0) q.sbuf[d2] = (uint32_t)m_sCap[d2];
                else if (spriteData(d2, c, data)) q.sbuf[d2] = data;
            }
        }
        if (i == 6) { q.pri = m_regs[SPBGPR]; q.expx = m_regs[XXPAND]; }
        if (i == 7) {                                                       // 6569: MC bits latch late
            const uint8_t next = m_regs[SPMC];
            q.mcFlops &= (uint8_t)~(next ^ q.mc);
            q.mc = next;
            q.halt &= (uint8_t)~dma2;
        }

        // Trigger: X matches (as the register was a cycle ago)
        const int x = xpos + i;
        if (q.pending & ~q.active & ~q.halt) {
            for (int n = 0; n < 8; ++n) {
                const uint8_t m = (uint8_t)(1 << n);
                if ((q.pending & m) && !(q.active & m) && !(q.halt & m) &&
                    (q.xPipe[n] & 0x1F8) == xpos && q.xPipe[n] == x) {
                    q.expxFlops |= m; q.mcFlops |= m; q.active |= m;
                }
            }
        }

        // Shift and collide
        if (q.active) {
            uint8_t mask = 0;
            for (int n = 7; n >= 0; --n) {
                const uint8_t m = (uint8_t)(1 << n);
                if (!(q.active & m)) continue;
                if (!q.sbuf[n] && !q.pixel[n]) { q.active &= (uint8_t)~m; continue; }
                if (!(q.halt & m)) {
                    if (q.expxFlops & m) {
                        if (q.mc & m) {
                            if (q.mcFlops & m) q.pixel[n] = (uint8_t)((q.sbuf[n] >> 22) & 3);
                            q.mcFlops ^= m;
                        } else {
                            q.pixel[n] = (uint8_t)(((q.sbuf[n] >> 23) & 1) << 1);   // 6569: colour latency
                        }
                        q.sbuf[n] = (q.sbuf[n] << 1) & 0xFFFFFF;
                    }
                    if (q.expx & m) q.expxFlops ^= m;
                    else            q.expxFlops |= m;
                }
                if (q.pixel[n]) mask |= m;
            }
            if (mask & (mask - 1)) ss |= mask;
            if (mask && fg[t]) sb |= mask;
        }

        if (i == 7) {
            for (int n = 0; n < 8; ++n)
                q.xPipe[n] = m_regs[SP0X + 2 * n] | ((m_regs[MSIGX] >> n) & 1 ? 0x100 : 0);
        }
    }

    if (ss) {
        if (!m_sprSprColl && !(m_regs[IRQ] & IRQ_MMC)) { m_regs[IRQ] |= IRQ_MMC; updateIrq(); }
        m_sprSprColl |= ss;
    }
    if (sb) {
        if (!m_sprBgColl && !(m_regs[IRQ] & IRQ_MBC)) { m_regs[IRQ] |= IRQ_MBC; updateIrq(); }
        m_sprBgColl |= sb;
    }
}

// ---------------------------------------------------------------------------
// The Phi1 fetch of the current cycle (VICE cycle_phi1_fetch, from its cycle
// tables): sprite pointers and data, DRAM refresh, graphics in the display,
// else the idle fetch ($3FFF, $39FF with ECM). An approximation of VICE's
// per-cycle fetch state: the display test is by line, as for collisions
// ---------------------------------------------------------------------------
int VIC2::phi1Value() const {
    if (!m_dmaBus) return -1;
    const int c = (int)(m_cycleAccum % (uint64_t)m_cyclesPerLine);
    const int line = m_rasterLine;
    const bool ecm = m_regs[SCR1] & SCR1_ECM;
    // Idle fetches are from $3FFF; a g-access in the idle state from $39FF
    // with ECM (testprogs VICII/phi1timing)
    const uint8_t idle = dmaPeek(0x3FFF);
    // Sprite n's pointer is fetched in Phi1 of its first DMA cycle, its first
    // data byte in Phi1 of the next (if its DMA is on)
    const int ptr = sprDma0In(c);
    if (ptr >= 0) return dmaPeek(screenBase() + 0x3F8 + ptr);
    const int prev = sprDma0In(c == 0 ? m_cyclesPerLine - 1 : c - 1);
    if (prev >= 0) {
        int row = c < 11 ? line - 1 : line;
        if (row < 0) row += m_linesPerFrame;
        if (!((m_regs[SPENA] >> prev) & 1) || !spriteDma(prev, row)) return idle;
        return dmaPeek((uint32_t)dmaPeek(screenBase() + 0x3F8 + prev) * 64);
    }
    if (c >= 10 && c <= 14)                                     // refresh: $3Fxx, 5 a line
        return dmaPeek(0x3F00 | ((0xFF - (line * 5 + (c - 10))) & 0xFF));
    if (c >= 15 && c <= 54) {                                   // g-accesses
        const uint8_t scr1 = m_regs[SCR1];
        const int rel = line - (0x30 + (scr1 & SCR1_YSCROLL));
        if (m_denOnLine30 && line >= 0x30 && line < 0xF8 && rel >= 0 && rel / 8 < 25) {
            const int cell = (rel / 8) * 40 + (c - 15), rl = rel % 8;
            if (scr1 & SCR1_BMM) return dmaPeek(charBitmapBase() + cell * 8 + rl);
            const uint8_t code = dmaPeek(screenBase() + cell);
            return dmaPeek(charBitmapBase() + (ecm ? (code & 0x3F) : code) * 8 + rl);
        }
        return ecm ? dmaPeek(0x39FF) : idle;
    }
    return idle;
}

// The light pen line went low or high (the C64: CIA1 PB4, joystick 1's
// fire). Falling, it triggers a cycle later (VICE vicii_set_light_pen)
void VIC2::setLightPen(bool low) {
    if (low && !m_lpLow) { m_lpPending = true; m_lpAt = m_ticks + 1; }
    m_lpLow = low;
}

// VICE vicii_trigger_light_pen_internal: once a frame, latch X (the cycle's
// X / 2, +2 on the 6569) and the raster line, and raise the light pen IRQ;
// a retrigger at the start of a frame latches X $D1 ($D5 with 65 cycles)
void VIC2::triggerLightPen(bool retrigger) {
    if (m_lpTriggered) return;
    m_lpTriggered = true;
    const int c = (int)(m_cycleAccum % (uint64_t)m_cyclesPerLine);
    const int line = visibleLine();
    if (line == m_linesPerFrame - 1 && c > 0) return;            // not on the last line
    int x = cycleXpos(c) / 2 + 2;
    if (retrigger) x = m_cyclesPerLine == NTSC_CYCLES_PER_LINE ? 0xD5 : 0xD1;
    m_regs[LPX] = (uint8_t)x;
    m_regs[LPY] = (uint8_t)line;
    m_regs[IRQ] |= IRQ_LP;
    updateIrq();
}

// "decode": the window the chip answers in, its 64 registers repeating (the
// C64 decodes the VIC-II through $D000-$D3FF: "0x3FF")
bool VIC2::setOption(const std::string& key, const std::string& value, std::string& err) {
    if (key != "decode") { err = "the VIC-II's option is decode (a window mask, e.g. 0x3FF)"; return false; }
    char* end = nullptr;
    const unsigned long m = std::strtoul(value.c_str(), &end, 0);
    if (!end || *end || m < 0x3F || ((m + 1) & m)) { err = "decode is a window mask: 0x3F ... 0x3FF"; return false; }
    m_decodeMask = (uint32_t)m;
    return true;
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
