// The TCC1014 GIME of the Color Computer 3. Register behaviour after
// Sockmaster's GIME reference (https://www.6809.org.uk/sock/gime.html) and
// the CoCo 3 service manual; video timing figures and the VDG-compatible
// rendering as measured by XRoar's author (tcc1014.c, GPL-3.0-or-later):
// mmsim's own code.

#include "gime.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

// Lines of top border, by VRES LPF: 192, 200, "infinite", 225 (XRoar's measurements)
const uint32_t TOP_BORDER[2][4] = {
    {36, 34, 65535, 19},       // 60 Hz
    {63, 59, 65535, 46},       // 50 Hz
};
const uint32_t ACTIVE_LINES[4] = {192, 199, 65535, 225};

const uint32_t GFX_BYTES[8] = {16, 20, 32, 40, 64, 80, 128, 160};
const uint32_t TEXT_CHARS[8] = {32, 40, 32, 40, 64, 80, 64, 80};

// Lines per row as the row counter's reset mask (the counter restarts when
// the masked bits are all set): LPR 1, 1, 2, 8, 9, 10, 11, infinite
const uint32_t LPR_MASK[8] = {0, 1, 2, 8, 9, 10, 11, 16};
// VDG-compatible graphics: by SAM V (3 lines a row for G1/G2C, 2, 1)
const uint32_t SAM_V_MASK[8] = {3, 3, 3, 2, 2, 1, 1, 1};
// VDG-compatible text: the rows a character has, by VSC (12 with $0F, BASIC's)
const uint32_t VSC_MASK[16] = {11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 4, 3, 2, 1, 12};

// The first scan line of the frame shown (the 192-line active area centred)
constexpr uint32_t FIRST_SHOWN_60 = 19;
constexpr uint32_t FIRST_SHOWN_50 = 46;

// The palette's uses in the VDG-compatible modes
enum : uint8_t {
    P_GREEN, P_YELLOW, P_BLUE, P_RED, P_WHITE, P_CYAN, P_MAGENTA, P_ORANGE,
    P_RG0_BG, P_RG0_FG, P_RG1_BG, P_RG1_FG, P_TEXT_BG, P_TEXT_FG, P_TEXT1_BG, P_TEXT1_FG,
};

template <class T> void put(std::vector<uint8_t>& v, const T& x) {
    const auto* p = reinterpret_cast<const uint8_t*>(&x);
    v.insert(v.end(), p, p + sizeof(T));
}
template <class T> void get(const uint8_t*& p, const uint8_t* end, T& x) {
    if (p + sizeof(T) > end) return;
    std::memcpy(&x, p, sizeof(T));
    p += sizeof(T);
}

}  // namespace

// Horizontal timing, pixel ticks from HSYNC falling (XRoar's scope
// measurements of each revision): sync, back porch, left border, active
// area, where the vertical events (VBORD, FSYNC) fall on a line, and the
// front porch (the right border runs to it)
const Gime::Timing Gime::TIMING[2] = {
    // 1986
    {2, 80,
     {{61, 57}, {63, 63}},
     {{{106, 110}, {44, 48}}, {{104, 104}, {42, 42}}},
     {512, 640},
     {{{225, 221}, {161, 157}}, {{227, 227}, {163, 163}}},
     {{27, 31}, {25, 25}}},
    // 1987
    {1, 72,
     {{61, 57}, {63, 63}},
     {{{106, 110}, {42, 48}}, {{106, 106}, {42, 42}}},
     {512, 640},
     {{{137, 133}, {137, 133}}, {{139, 139}, {139, 139}}},
     {{35, 39}, {33, 33}}},
};

Gime::Gime(Revision rev)
    : m_rev(rev), m_frame((size_t)OUT_W * OUT_H, 0), m_lineFlags(OUT_H, 0), m_lineOrigin(OUT_H, 0) {
    reset();
}

// 128K, 512K; 1M and 2M are 512K banks through the Disto board
void Gime::setRamKb(int kb) {
    m_ramKb = kb >= 2048 ? 2048 : kb >= 1024 ? 1024 : kb >= 512 ? 512 : 128;
    m_ramMask = (uint32_t)m_ramKb * 1024u - 1u;
    m_datMask = m_ramKb >= 2048 ? 0xC0 : m_ramKb >= 1024 ? 0x40 : 0x00;
    for (auto& d : m_dat) d &= m_datMask;
    m_datVideo &= m_datMask;
}

void Gime::reset() {
    std::memset(m_reg, 0, sizeof m_reg);
    std::memset(m_palette, 0, sizeof m_palette);
    // The page registers are not cleared by /RESET on the hardware; BASIC
    // sets them. Power on (here) leaves them 0.
    m_sam = 0;
    m_pia1b = 0;
    m_pia1bData = false;
    m_irqState = m_firqState = 0;
    m_timerCount = 0;
    m_timerTicks = 0;
    m_blink = false;
    m_scanline = 0;
    m_lineTick = 0;
    m_sync = true;
    m_active = false;
    m_lcount = 0;
    m_row = 0;
    m_haveCache = false;
    m_vbDone = m_hbDone = m_hsRisen = false;
    m_rendering = false;
    m_lineY = -1;
    samChanged();
    m_tVb = timing().tHsVb[0][0][0];
    m_tRb = 1000;
    m_tAa = 512;
    outputs();
}

// ---------------------------------------------------------------------------
// The CPU's side
// ---------------------------------------------------------------------------
Gime::Decode Gime::decode(uint16_t a) const {
    if (a < 0xFF00) {
        const bool constant = a >= 0xFE00 && (m_reg[0] & 0x08);
        const bool mmu = !constant && (m_reg[0] & 0x40);
        const unsigned slot = (m_reg[1] & 1) * 8 + (a >> 13);
        const unsigned page = constant ? 0x3F : mmu ? m_mmu[slot] : (0x38u | (a >> 13));
        if (!constant && !m_ty && page >= 0x3C) {
            const bool external = (m_reg[0] & 0x02) ? (m_reg[0] & 0x01) != 0 : page >= 0x3E;
            return {external ? S_CTS : S_ROM, 0};
        }
        const uint32_t z = (uint32_t)page << 13 | (a & 0x1FFFu);
        if (m_datMask) {
            // The Disto board's bank: the page register's bits 6-7 (bank 0 without the MMU)
            const uint32_t bank = mmu ? (uint32_t)(m_dat[slot] >> 6) : 0u;
            return {S_RAM, (bank << 19 | (z & 0x7FFFFu)) & m_ramMask};
        }
        return {S_RAM, z & m_ramMask};
    }
    if (a < 0xFF40) return {(a & 0x10) ? S_NONE : (a & 0x20) ? S_PIA1 : S_PIA0, 0};
    if (a < 0xFF60) return {((m_reg[0] & 0x04) || a >= 0xFF50) ? S_SCS : S_NONE, 0};
    if (a < 0xFF90) return {S_NONE, 0};
    if (a < 0xFFE0) return {S_GIME, 0};
    return {S_ROM, 0};
}

bool Gime::peek(uint16_t a, uint8_t& v) const {
    if (a == 0xFF92) { v = m_irqState; return true; }
    if (a == 0xFF93) { v = m_firqState; return true; }
    if (a >= 0xFFA0 && a < 0xFFB0) { v = (uint8_t)(m_mmu[a & 15] | m_dat[a & 15]); return true; }
    if (a >= 0xFFB0 && a < 0xFFC0) { v = m_palette[a & 15]; return true; }
    if (a == 0xFF9B && m_datMask) { v = (uint8_t)(m_datVideo >> 6); return true; }
    return false;     // the rest are write-only
}

uint8_t Gime::readMask(uint16_t a) const {
    if (m_datMask && a >= 0xFFA0 && a < 0xFFB0) return 0xFF;
    if (m_datMask && a == 0xFF9B) return 0x03;
    return 0x3F;
}

bool Gime::read(uint16_t a, uint8_t& v) {
    if (!peek(a, v)) return false;
    if (a == 0xFF92) { m_irqState = 0; outputs(); }
    else if (a == 0xFF93) { m_firqState = 0; outputs(); }
    return true;
}

void Gime::write(uint16_t a, uint8_t v) {
    // The line so far in the old registers (the page registers aren't the video's)
    if (a >= 0xFF90 && a < 0xFFE0 && !(a >= 0xFFA0 && a < 0xFFB0)) renderTo(m_lineTick + (uint32_t)cycleTicks());
    if (a >= 0xFF90 && a < 0xFFA0) writeReg(a & 15, v);
    else if (a >= 0xFFA0 && a < 0xFFB0) {
        m_mmu[a & 15] = v & 0x3F;
        m_dat[a & 15] = v & m_datMask;
    }
    else if (a >= 0xFFB0 && a < 0xFFC0) m_palette[a & 15] = v & 0x3F;
    else if (a >= 0xFFC0 && a < 0xFFE0) {
        const uint16_t bit = (uint16_t)(1u << ((a >> 1) & 15));
        if (a & 1) m_sam |= bit;
        else m_sam &= (uint16_t)~bit;
        samChanged();
    }
}

void Gime::snoopPia1(uint16_t a, uint8_t v) {
    if ((a & 3) == 3) {
        m_pia1bData = v & 0x04;
    } else if ((a & 3) == 2 && m_pia1bData) {
        renderTo(m_lineTick + (uint32_t)cycleTicks());
        m_pia1b = v & 0xF8;
        modeChanged();
    }
}

void Gime::writeReg(int r, uint8_t v) {
    const uint8_t changed = m_reg[r] ^ v;
    m_reg[r] = v;
    switch (r) {
        case 0:                             // INIT0
            modeChanged();
            outputs();
            break;
        case 1:                             // INIT1: TINS, TR
            if (changed & 0x20) restartTimer();
            break;
        case 2:
        case 3: {                           // IRQENR / FIRQENR
            uint8_t& st = r == 2 ? m_irqState : m_firqState;
            st &= v;
            const uint8_t holding = (uint8_t)((m_timerCount == 0 ? INT_TMR : 0) | m_inputs);
            st |= (uint8_t)(holding & changed & v);
            outputs();
            break;
        }
        case 4:                             // timer MSB: restarts it
            restartTimer();
            break;
        case 0xB:                           // the Disto board's video bank
            m_datVideo = (uint8_t)(((v & 3) << 6) & m_datMask);
            break;
        case 8: case 9: case 0xA: case 0xC: case 0xF:
            modeChanged();
            break;
        default:
            break;
    }
}

void Gime::samChanged() {
    m_ty = m_sam & 0x8000;
    m_r1 = m_sam & 0x1000;
    m_samF = (uint16_t)((m_sam >> 3) & 0x7F);
    m_samV = (uint8_t)(m_sam & 7);
    modeChanged();
}

// Bytes per row, the dots a byte makes and the row counter's mask, from the mode now
void Gime::modeChanged() {
    if (coco()) {
        const bool gna = m_pia1b & 0x80;
        if (!gna || !(m_samV & 1)) { m_bpr = 32; m_resolution = 1; }
        else { m_bpr = 16; m_resolution = 0; }
        m_rowMask = gna ? SAM_V_MASK[m_samV] : VSC_MASK[m_reg[0xC] & 15];
    } else {
        const unsigned hres = (m_reg[9] >> 2) & 7;
        if (m_reg[8] & 0x80) {
            m_bpr = GFX_BYTES[hres];
            m_resolution = (int)(hres >> 1);
        } else {
            m_bpr = TEXT_CHARS[hres];
            m_resolution = (hres & 4) ? 2 : 1;
        }
        m_rowMask = LPR_MASK[m_reg[8] & 7];
    }
}

// ---------------------------------------------------------------------------
// Interrupts and the timer
// ---------------------------------------------------------------------------
void Gime::raise(uint8_t bits) {
    m_irqState |= (uint8_t)(bits & m_reg[2] & 0x3F);
    m_firqState |= (uint8_t)(bits & m_reg[3] & 0x3F);
    outputs();
}

void Gime::outputs() {
    const bool irq = (m_reg[0] & 0x20) && (m_irqState & 0x3F);
    const bool firq = (m_reg[0] & 0x10) && (m_firqState & 0x3F);
    const bool changed = irq != m_irqOut || firq != m_firqOut;
    m_irqOut = irq;
    m_firqOut = firq;
    if (changed && onIrq) onIrq();
}

void Gime::setInterruptInput(uint8_t bit, bool active) {
    bit &= 7;
    const bool was = m_inputs & bit;
    if (active) m_inputs |= bit;
    else m_inputs &= (uint8_t)~bit;
    if (active && !was) raise(bit);
}

void Gime::restartTimer() {
    const int v = ((m_reg[4] & 0x0F) << 8) | m_reg[5];
    m_timerCount = v ? v + timing().timerOffset : 0;
    m_timerTicks = m_timerCount * 4;            // TINS 1: 3.58 MHz, a count per 4 pixel ticks
}

void Gime::timerExpired() {
    m_blink = !m_blink;
    restartTimer();
    raise(INT_TMR);
}

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------
void Gime::advance(uint64_t ticks) {
    while (ticks) {
        // To the next thing that happens on the line, or the timer at 3.58 MHz
        uint32_t next = LINE_TICKS - m_lineTick;
        if (!m_hsRisen && (uint32_t)timing().tHS > m_lineTick) next = std::min(next, (uint32_t)timing().tHS - m_lineTick);
        if (!m_vbDone && (uint32_t)m_tVb > m_lineTick) next = std::min(next, (uint32_t)m_tVb - m_lineTick);
        if (!m_hbDone && (uint32_t)m_tRb > m_lineTick) next = std::min(next, (uint32_t)m_tRb - m_lineTick);
        const bool fastTimer = (m_reg[1] & 0x20) && m_timerCount > 0;
        if (fastTimer) next = std::min(next, (uint32_t)m_timerTicks);
        const uint32_t step = (uint32_t)std::min<uint64_t>(ticks, next);
        ticks -= step;
        m_now += step;
        m_lineTick += step;
        if (fastTimer) {
            m_timerTicks -= (int)step;
            m_timerCount = (m_timerTicks + 3) / 4;
            if (m_timerTicks <= 0) timerExpired();
        }
        if (m_lineTick >= (uint32_t)LINE_TICKS) {
            m_lineTick = 0;
            hsFall();
            continue;
        }
        if (!m_hsRisen && m_lineTick >= (uint32_t)timing().tHS) {
            m_hsRisen = true;
            if (onHs) onHs(true, m_now);
        }
        if (!m_vbDone && m_lineTick >= (uint32_t)m_tVb) { m_vbDone = true; vbPoint(); }
        if (!m_hbDone && m_lineTick >= (uint32_t)m_tRb) { m_hbDone = true; hbPoint(); }
    }
}

// HSYNC falls: a new line (the timer counts here at the line rate)
void Gime::hsFall() {
    renderTo(LINE_TICKS);                   // the rest of the line ending
    if (m_lineY >= 0) {
        // The colour burst's phase (VMODE BPI) as the line ends
        uint8_t& f = m_lineFlags[(size_t)m_lineY];
        f = (uint8_t)((f & ~LF_BPI) | ((m_reg[8] & 0x20) ? LF_BPI : 0));
    }
    m_rendering = false;
    m_lineY = -1;
    m_hsRisen = m_vbDone = m_hbDone = false;
    m_haveCache = false;
    ++m_scanline;
    m_tVb = timing().tHsVb[hiRes()][wide()][gfx()];
    m_tRb = 1000;
    if (onHs) onHs(false, m_now);
    if (!(m_reg[1] & 0x20) && m_timerCount > 0) {
        if (--m_timerCount == 0) timerExpired();
    }
}

// Near the left border: the row counter, the video address, the border
// colour and the frame's vertical progress (FSYNC, VBORD)
void Gime::vbPoint() {
    const bool cc = coco();
    if (m_active) {
        if ((m_row & m_rowMask) == m_rowMask) {
            m_row = 0;
            m_rowAdvance = true;
        }
        if (!cc && (m_reg[8] & 7) == 7) m_rowAdvance = false;    // infinite lines per row
        if (m_rowAdvance) m_b += m_stride;
    }
    m_xoff = (uint32_t)(m_reg[0xF] & 0x7F) << 1;
    m_fetchX = m_xoff;
    const bool hven = m_reg[0xF] & 0x80;
    if (cc || gfx()) m_stride = hven ? 256 : m_bpr;
    else m_stride = hven ? 256 : (m_bpr << (m_reg[9] & 1));

    // The border: BRDR in the CoCo 3 modes; in the VDG modes black for
    // text (green or orange for SG6), the CSS foreground for graphics
    if (cc) {
        const bool gna = m_pia1b & 0x80, gm1 = m_pia1b & 0x20, gm2 = m_pia1b & 0x40, css = m_pia1b & 0x08;
        if (!gna) m_border = (!gm1 && gm2) ? (css ? 0x26 : 0x12) : 0;
        else m_border = m_palette[css ? P_RG1_FG : P_RG0_FG];
    } else {
        m_border = m_reg[0xA] & 0x3F;
    }

    const Timing& t = timing();
    const int hr = hiRes(), w = wide(), g = gfx();
    m_tAa = t.tAA[w];
    m_tLb = (uint32_t)(t.tHS + t.tBP[hr][g]);
    m_tAaStart = m_tLb + (uint32_t)t.tLB[hr][w][g];
    m_tRb = (int)m_tAaStart + m_tAa;
    m_tFp = (uint32_t)(LINE_TICKS - t.tFP[hr][g]);
    m_activeX = 32 + (t.tLB[hr][w][g] - t.tLB[hr][1][g]);
    m_xOrigin = (int)m_tAaStart - m_activeX;
    m_npix = m_tLb;

    ++m_lcount;
    const unsigned lpf = (m_reg[9] >> 5) & 3;
    if (m_scanline >= m_lF) {
        // The frame ends: FSYNC falls; the next frame's geometry is latched
        if (onFs) onFs(false, m_now);
        const bool h50 = m_reg[8] & 0x08;
        m_lAA = cc ? 192 : ACTIVE_LINES[lpf];
        m_lF = h50 ? 314 : 263;
        m_lTB = cc ? (h50 ? 63 : 36) : TOP_BORDER[h50 ? 1 : 0][lpf];
        m_sync = true;
        m_lcount = 0;
        m_scanline = 0;
    } else if (m_sync) {
        // Four lines of sync, three of blanking
        if (m_lcount == 4) {
            if (onFs) onFs(true, m_now);
        } else if (m_lcount >= 7) {
            const uint32_t y = (uint32_t)m_reg[0xD] << 11 | (uint32_t)m_reg[0xE] << 3;
            m_b = cc ? ((y & 0x701FF) | (uint32_t)m_samF << 9) : y;
            m_sync = false;
            m_lcount = 0;
        }
    } else if (!m_active) {
        if (m_lcount >= m_lTB) {
            m_row = 0;
            if (!cc) {
                m_row = m_reg[0xC] & 15;            // VSC: the first row starts part way
                if ((m_row & m_rowMask) == m_rowMask) m_row = 0;
            }
            m_active = true;
            m_lcount = 0;
        }
    } else {
        // LPF "infinite" written during the active area runs it on (Sockmaster)
        const uint32_t lines = (!cc && lpf == 2) ? 65535 : m_lAA;
        if (m_lcount >= lines) {
            m_active = false;
            m_lTB = 65535;                        // border to the end of the frame
            m_lcount = 0;
            raise(INT_VBORD);
        }
    }
    lineStart();
}

// The line's picture begins at the left border: a border or blank line is
// filled now (its colour was latched above); an active one is drawn as the
// beam goes (renderTo), so writes part way along it show from there on
void Gime::lineStart() {
    m_rendering = false;
    m_lineY = frameLine((int)m_scanline);
    if (m_lineY < 0) return;
    m_lineFlags[(size_t)m_lineY] = 0;
    m_lineOrigin[(size_t)m_lineY] = (int16_t)m_xOrigin;
    uint8_t* out = &m_frame[(size_t)m_lineY * OUT_W];
    if (!m_active || m_sync) {
        std::fill(out, out + OUT_W, m_sync && !m_active ? 0 : m_border);
        return;
    }
    m_rendering = true;
    m_renderY = m_lineY;
}

// The right border starts: the active area is done, the row counts on, HBORD
void Gime::hbPoint() {
    renderTo((uint32_t)m_tRb);
    m_rowAdvance = false;
    if (m_active) {
        m_row = (m_row + 1) & 15;
        if ((m_row & m_rowMask) == m_rowMask) m_rowAdvance = true;
    }
    raise(INT_HBORD);
}

// Two bytes come from the RAM at a time (the high byte first); the
// horizontal offset is added as the line goes, within 256 bytes
uint8_t Gime::fetch() {
    if (m_haveCache) {
        m_haveCache = false;
        return m_cache;
    }
    const uint32_t a = (m_b + (m_fetchX & 0xFF)) & ~1u;
    m_fetchX += 2;
    const uint8_t hi = m_ram ? m_ram[videoPhys(a)] : 0xFF;
    m_cache = m_ram ? m_ram[videoPhys(a + 1)] : 0xFF;
    m_haveCache = true;
    return hi;
}

// The video's address in the RAM: 19 bits, in the Disto board's video bank
uint32_t Gime::videoPhys(uint32_t a) const {
    if (m_datMask) return ((uint32_t)(m_datVideo >> 6) << 19 | (a & 0x7FFFFu)) & m_ramMask;
    return a & m_ramMask;
}

// MOCH: the GIME's composite output gives only the intensity (a PAL board
// makes its composite from the RGB outputs, which MOCH doesn't touch)
uint8_t Gime::compositeMask() const {
    return ((m_reg[8] & 0x10) && m_monitor == Monitor::Composite && !m_pal) ? 0x30 : 0x3F;
}

int Gime::frameLine(int scanline) const {
    const int y = scanline - (int)(m_lF == 314 ? FIRST_SHOWN_50 : FIRST_SHOWN_60);
    return (y >= 0 && y < OUT_H) ? y : -1;
}

int Gime::activeY() const {
    return 7 + (int)(m_lTB == 65535 ? (coco() ? 36 : TOP_BORDER[0][(m_reg[9] >> 5) & 3]) : m_lTB) -
           (int)(m_lF == 314 ? FIRST_SHOWN_50 : FIRST_SHOWN_60);
}

/*
 * A line of the active area. Each byte fetched makes eight colours, in two
 * groups of four, which the resolution spreads over the line: 4 ticks each
 * (16 and 20 bytes a row), 2 (32, 40), 1 (64, 80) or, at 128 and 160
 * bytes, the first and third of each group (2 ticks a 16-colour pixel).
 *
 * VDG-compatible (COCO): text from the GIME's font (lower case for codes
 * $00-$1F with GM0, as an MC6847T1; GM1 inverts), SG4 blocks in the
 * colour of bits 4-6, and the CG/RG graphics, through palette 0-15 as
 * the VDG's colours (0-7 the CG colours, 8-11 the RG pairs, 12-15 text).
 *
 * CoCo 3 text: a character code (bits 0-6) and, with VRES CRES bit 0, an
 * attribute byte after it: bits 0-2 the background (palette 0-7), 3-5 the
 * foreground (palette 8-15), 6 underline (the row's last line), 7 blink
 * (the foreground hidden while the timer's blink flag is set). Without
 * attributes, palette 1 on palette 0. CoCo 3 graphics: 1, 2 or 4 bits a
 * pixel, the leftmost the most significant, through palette 0-1, 0-3 or
 * 0-15. MOCH on composite shows only the intensity (grey).
 *
 * The line is drawn as the beam goes (XRoar's model): up to where it is
 * before each write that could change it (the GIME's registers, the
 * palette, the SAM bits, PIA1 port B), at the right border and at the
 * line's end. Each byte fetched is drawn whole from the registers as they
 * are when the beam reaches it, so a change shows from the next byte (the
 * next character, or 2-32 ticks on) after the write's cycle ends. The
 * border colour and the line's horizontal timing are taken where the left
 * border starts (vbPoint), as on XRoar.
 */
void Gime::renderTo(uint32_t beam) {
    if (!m_rendering) return;
    if (beam > m_tFp) beam = m_tFp;
    uint8_t* out = &m_frame[(size_t)m_renderY * OUT_W];
    while (m_npix < beam) {
        if (m_npix < m_tAaStart || m_npix >= (uint32_t)m_tRb) {
            plot(out, m_npix, m_border);
            plot(out, m_npix + 1, m_border);
            m_npix += 2;
        } else {
            renderByte(out);
        }
    }
}

// One byte's worth of the active area (with its attribute byte in text
// with attributes), in the registers as they are now
void Gime::renderByte(uint8_t* out) {
    const bool cc = coco();
    const bool gm0 = m_pia1b & 0x10, gm1 = m_pia1b & 0x20, css = m_pia1b & 0x08, gna = m_pia1b & 0x80;
    const uint8_t cmask = compositeMask();
    const unsigned hres = (m_reg[9] >> 2) & 7, cres = m_reg[9] & 3;
    uint8_t c[8];
    const uint8_t v = fetch();
    if (cc) {
        if (gna) m_lineFlags[(size_t)m_renderY] |= LF_VDG_GFX;
        const unsigned fontRow = m_row & 15;
        enum { SG, CG, RG, RG2 } mode;
        uint8_t g, fg = 0, bg = 0, cgBase = 0;
        if (gna) {
            g = v;
            fg = css ? P_RG1_FG : P_RG0_FG;
            bg = css ? P_RG1_BG : P_RG0_BG;
            cgBase = css ? P_WHITE : P_GREEN;
            if (!gm0) mode = CG;
            else mode = (m_resolution || (m_pia1b & 0x70) == 0x70) ? RG : RG2;
        } else if (v & 0x80) {
            g = fontRow < 6 ? (uint8_t)(v >> 2) : v;
            fg = (v >> 4) & 7;
            bg = P_RG0_BG;
            mode = SG;
        } else {
            bool inv = ((v & 0x40) != 0) != gm1;
            uint8_t ch = v & 0x7F;
            if (ch < 0x20) {
                ch |= gm0 ? 0x60 : 0x40;
                inv = inv != gm0;
            } else if (ch >= 0x60) {
                ch ^= 0x40;
            }
            g = fontRow < 12 ? gime_font::GLYPHS[ch][fontRow] : 0;
            if (inv) g = (uint8_t)~g;
            fg = css ? P_TEXT1_FG : P_TEXT_FG;
            bg = css ? P_TEXT1_BG : P_TEXT_BG;
            mode = RG;
        }
        for (int half = 0; half < 2; ++half) {
            uint8_t* q = c + half * 4;
            switch (mode) {
                case SG:
                    q[0] = q[1] = q[2] = q[3] = m_palette[(g & 2) ? fg : bg];
                    g = (uint8_t)(g << 1);
                    break;
                case CG:
                    q[0] = q[1] = m_palette[cgBase + ((g >> 6) & 3)];
                    q[2] = q[3] = m_palette[cgBase + ((g >> 4) & 3)];
                    g = (uint8_t)(g << 4);
                    break;
                case RG:
                    for (int i = 0; i < 4; ++i) q[i] = m_palette[(g & (0x80 >> i)) ? fg : bg];
                    g = (uint8_t)(g << 4);
                    break;
                case RG2:
                    q[0] = q[1] = m_palette[(g & 0x40) ? fg : bg];
                    q[2] = q[3] = m_palette[(g & 0x10) ? fg : bg];
                    g = (uint8_t)(g << 4);
                    break;
            }
        }
    } else if (m_reg[8] & 0x80) {
        // CoCo 3 graphics; 16 colours at 16 bytes a row lose each second byte
        if (hres == 0 && cres >= 2) m_cache = 0;
        for (int i = 0; i < 8; ++i) {
            switch (cres) {
                case 0: c[i] = m_palette[(v >> (7 - i)) & 1]; break;
                case 1: c[i] = m_palette[(v >> (6 - (i / 2) * 2)) & 3]; break;
                default: c[i] = m_palette[(v >> (4 - (i / 4) * 4)) & 15]; break;
            }
            c[i] &= cmask;
        }
    } else {
        // CoCo 3 text
        unsigned fontRow = (m_row + 1) & 15;
        if (fontRow > 11) fontRow = 0;
        uint8_t g = gime_font::GLYPHS[v & 0x7F][fontRow];
        uint8_t fg = 1, bg = 0;
        if (cres & 1) {
            const uint8_t attr = fetch();
            fg = (uint8_t)(8 | ((attr >> 3) & 7));
            bg = attr & 7;
            if ((attr & 0x80) && m_blink) fg = bg;
            if ((attr & 0x40) && (fontRow & m_rowMask) == m_rowMask) g = 0xFF;
        }
        for (int i = 0; i < 8; ++i) c[i] = (uint8_t)(m_palette[(g & (0x80 >> i)) ? fg : bg] & cmask);
    }

    // Spread the eight over the line
    uint32_t t = m_npix;
    for (int half = 0; half < 2; ++half) {
        const uint8_t* q = c + half * 4;
        switch (m_resolution) {
            case 0:
                for (int i = 0; i < 16; ++i) plot(out, t++, q[i / 4]);
                break;
            case 1:
                for (int i = 0; i < 8; ++i) plot(out, t++, q[i / 2]);
                break;
            case 2:
                for (int i = 0; i < 4; ++i) plot(out, t++, q[i]);
                break;
            default:
                plot(out, t, q[0]);
                plot(out, t + 1, q[2]);
                t += 2;
                break;
        }
    }
    m_npix = t;
}

// ---------------------------------------------------------------------------
// Colours
// ---------------------------------------------------------------------------
uint32_t Gime::rgb(uint8_t c) {
    const unsigned r = ((c >> 4) & 2) | ((c >> 2) & 1);
    const unsigned g = ((c >> 3) & 2) | ((c >> 1) & 1);
    const unsigned b = ((c >> 2) & 2) | (c & 1);
    return 0xFF000000u | (r * 0x55u) << 16 | (g * 0x55u) << 8 | (b * 0x55u);
}

// NTSC composite: the GIME switches between a few voltages at 3.58 MHz.
// For each intensity, the grey level and the colour signal's low and high
// levels (volts above blank at the composite port, XRoar's measurements);
// the hue is the colour signal's phase in GIME clock edges, 16 to a
// colour cycle for 15 hues (a gap between hues 11 and 12). As Y, U, V
// (U = amplitude x sin(phase), V = amplitude x cos(phase)).
static void compositeYuv(uint8_t c, double& y, double& u, double& v) {
    static const double GREY[4] = {0.000, 0.170, 0.380, 0.750};
    static const double LOW[4] = {-0.210, 0.000, 0.170, 0.380};
    static const double HIGH[4] = {0.380, 0.580, 0.750, 0.750};
    const int in = (c >> 4) & 3, hue = c & 15;
    u = v = 0;
    if (hue == 0 || (c & 0x3F) == 63) {
        y = GREY[in] / 0.75;
    } else {
        const int edge = ((hue + (hue >= 12 ? 1 : 0)) + 9) % 16;
        const double phase = 2.0 * M_PI * edge / 16.0 + 2.0 * M_PI * 15.0 / 1600.0;
        const double amp = (HIGH[in] - LOW[in]) / 2.0 / 0.75;
        y = (HIGH[in] + LOW[in]) / 2.0 / 0.75;
        u = amp * std::sin(phase);
        v = amp * std::cos(phase);
    }
}

static uint32_t yuvArgb(double y, double u, double v) {
    auto ch = [](double x) { return (uint32_t)std::lround(std::max(0.0, std::min(1.0, x)) * 255.0); };
    const uint32_t r = ch(y + 1.140 * v), g = ch(y - 0.395 * u - 0.581 * v), b = ch(y + 2.032 * u);
    return 0xFF000000u | r << 16 | g << 8 | b;
}

uint32_t Gime::composite(uint8_t c) {
    double y, u, v;
    compositeYuv(c, y, u, v);
    return yuvArgb(y, u, v);
}

// A PAL CoCo 3 doesn't use the GIME's composite output: the board encodes
// its RGB outputs for the TV (XRoar: "PAL CoCo 3s only emit the RGB
// palette, even over composite"). The levels are XRoar's measurements of
// the RGB outputs, 0, 1, 1.6 and 2 V, scaled to full range; the board's
// encoder itself is unmeasured (XRoar too only approximates it).
uint32_t Gime::palComposite(uint8_t c) {
    static const uint32_t LEVEL[4] = {0x00, 0x80, 0xCC, 0xFF};
    const unsigned r = ((c >> 4) & 2) | ((c >> 2) & 1);
    const unsigned g = ((c >> 3) & 2) | ((c >> 1) & 1);
    const unsigned b = ((c >> 2) & 2) | (c & 1);
    return 0xFF000000u | LEVEL[r] << 16 | LEVEL[g] << 8 | LEVEL[b];
}

uint32_t Gime::colour(uint8_t c) const {
    if (m_monitor == Monitor::Rgb) return rgb(c);
    return m_pal ? palComposite(c) : composite(c);
}

/*
 * NTSC artifact colours: a line drawn in a VDG-compatible graphics mode
 * is made into the composite signal a TV sees, one sample a pixel tick (4
 * a colour subcarrier cycle: a 912-tick line is exactly 228 cycles, so
 * the phase is the same every line), and decoded again: luma through a
 * low-pass that nulls the subcarrier and its second harmonic (1 2 3 4 3 2
 * 1, /16), U and V by demodulating against the subcarrier through the same
 * filter. A run of one colour decodes to exactly its composite colour; a
 * pattern at the subcarrier's rate (PMODE 4's 2-tick dots in pairs) becomes
 * a colour. Which colours depends on the subcarrier's phase against the
 * dots: the GIME's is fixed, but not measured here, so it is a setting
 * (BlueRed: a black dot then a white one shows blue, as on a CoCo 2 in that
 * phase); VMODE BPI inverts the burst and so swaps them.
 */
void Gime::decodeNtscLine(const uint8_t* in, int y, uint32_t* out, const uint32_t* lut) const {
    static const int W[7] = {1, 2, 3, 4, 3, 2, 1};
    // The subcarrier's phase at tick 0 of the VDG-compatible active area for
    // BlueRed: chosen so that black-white decodes to the hue of a CoCo 2's
    // artifact blue (#0080FF)
    static const double BETA = [] {
        const double ay = 0.299 * 0.0 + 0.587 * (128.0 / 255.0) + 0.114;
        const double au = 0.492 * (1.0 - ay), av = 0.877 * (0.0 - ay);
        const double n = std::sqrt(au * au + av * av);
        const double ca = au / n, sa = av / n;
        return std::atan2(-(ca - sa), -(ca + sa));
    }();
    double ys[64], us[64], vs[64];
    for (int i = 0; i < 64; ++i) compositeYuv((uint8_t)i, ys[i], us[i], vs[i]);
    double phase = BETA;
    if (m_artifact == Artifact::RedBlue) phase += M_PI;
    if (m_lineFlags[(size_t)y] & LF_BPI) phase += M_PI;
    // The VDG-compatible active area starts at a tick of 3 mod 4 on both revisions
    const int origin = m_lineOrigin[(size_t)y] + 1;
    double s4[4], c4[4];
    for (int k = 0; k < 4; ++k) {
        s4[k] = std::sin(M_PI / 2.0 * k + phase);
        c4[k] = std::cos(M_PI / 2.0 * k + phase);
    }
    double sig[OUT_W], sn[OUT_W], cs[OUT_W];
    for (int x = 0; x < OUT_W; ++x) {
        const int k = (x + origin) & 3;
        const uint8_t c = in[x] & 0x3F;
        sn[x] = s4[k];
        cs[x] = c4[k];
        sig[x] = ys[c] + us[c] * sn[x] + vs[c] * cs[x];
    }
    for (int x = 0; x < OUT_W; ++x) {
        bool solid = true;
        for (int k = -3; k <= 3 && solid; ++k) {
            const int i = std::max(0, std::min(OUT_W - 1, x + k));
            solid = in[i] == in[x];
        }
        if (solid) { out[x] = lut[in[x] & 0x3F]; continue; }
        double fy = 0, fu = 0, fv = 0;
        for (int k = -3; k <= 3; ++k) {
            const int i = std::max(0, std::min(OUT_W - 1, x + k));
            const double w = W[k + 3] / 16.0;
            fy += w * sig[i];
            fu += w * sig[i] * sn[i];
            fv += w * sig[i] * cs[i];
        }
        out[x] = yuvArgb(fy, 2.0 * fu, 2.0 * fv);
    }
}

void Gime::renderArgb(uint32_t* out) const {
    uint32_t lut[64];
    for (int i = 0; i < 64; ++i) lut[i] = colour((uint8_t)i);
    const bool ntsc = m_monitor == Monitor::Composite && !m_pal && m_artifact != Artifact::Off;
    for (int y = 0; y < OUT_H; ++y) {
        const uint8_t* in = &m_frame[(size_t)y * OUT_W];
        uint32_t* o = out + (size_t)y * OUT_W;
        if (ntsc && (m_lineFlags[(size_t)y] & LF_VDG_GFX)) {
            decodeNtscLine(in, y, o, lut);
        } else {
            for (int x = 0; x < OUT_W; ++x) o[x] = lut[in[x] & 0x3F];
        }
    }
}

// ---------------------------------------------------------------------------
// Snapshots
// ---------------------------------------------------------------------------
std::vector<uint8_t> Gime::saveState() const {
    std::vector<uint8_t> v;
    v.insert(v.end(), m_reg, m_reg + 16);
    v.insert(v.end(), m_mmu, m_mmu + 16);
    v.insert(v.end(), m_palette, m_palette + 16);
    put(v, m_sam); put(v, m_pia1b); put(v, m_pia1bData);
    put(v, m_irqState); put(v, m_firqState); put(v, m_inputs);
    put(v, m_timerCount); put(v, m_timerTicks); put(v, m_blink);
    put(v, m_now); put(v, m_lineTick); put(v, m_scanline);
    put(v, m_tVb); put(v, m_tRb); put(v, m_tAa);
    put(v, m_vbDone); put(v, m_hbDone); put(v, m_hsRisen);
    put(v, m_lF); put(v, m_lTB); put(v, m_lAA); put(v, m_sync); put(v, m_active); put(v, m_lcount);
    put(v, m_b); put(v, m_row); put(v, m_rowAdvance); put(v, m_xoff); put(v, m_stride);
    put(v, m_border); put(v, m_fetchX); put(v, m_activeX);
    put(v, m_tLb); put(v, m_tAaStart); put(v, m_tFp); put(v, m_npix); put(v, m_xOrigin);
    put(v, m_rendering); put(v, m_renderY); put(v, m_lineY);
    put(v, m_dat); put(v, m_datVideo);
    return v;
}

void Gime::loadState(const std::vector<uint8_t>& s) {
    if (s.size() < 48) return;
    const uint8_t* p = s.data();
    const uint8_t* end = p + s.size();
    std::memcpy(m_reg, p, 16); p += 16;
    std::memcpy(m_mmu, p, 16); p += 16;
    std::memcpy(m_palette, p, 16); p += 16;
    get(p, end, m_sam); get(p, end, m_pia1b); get(p, end, m_pia1bData);
    get(p, end, m_irqState); get(p, end, m_firqState); get(p, end, m_inputs);
    get(p, end, m_timerCount); get(p, end, m_timerTicks); get(p, end, m_blink);
    get(p, end, m_now); get(p, end, m_lineTick); get(p, end, m_scanline);
    get(p, end, m_tVb); get(p, end, m_tRb); get(p, end, m_tAa);
    get(p, end, m_vbDone); get(p, end, m_hbDone); get(p, end, m_hsRisen);
    get(p, end, m_lF); get(p, end, m_lTB); get(p, end, m_lAA); get(p, end, m_sync); get(p, end, m_active); get(p, end, m_lcount);
    get(p, end, m_b); get(p, end, m_row); get(p, end, m_rowAdvance); get(p, end, m_xoff); get(p, end, m_stride);
    get(p, end, m_border); get(p, end, m_fetchX); get(p, end, m_activeX);
    get(p, end, m_tLb); get(p, end, m_tAaStart); get(p, end, m_tFp); get(p, end, m_npix); get(p, end, m_xOrigin);
    get(p, end, m_rendering); get(p, end, m_renderY); get(p, end, m_lineY);
    get(p, end, m_dat); get(p, end, m_datVideo);
    for (auto& d : m_dat) d &= m_datMask;
    m_datVideo &= m_datMask;
    if (m_renderY < 0 || m_renderY >= OUT_H) m_rendering = false;
    if (m_lineY >= OUT_H) m_lineY = -1;
    m_haveCache = false;
    samChanged();
    outputs();
    if (onIrq) onIrq();
}
