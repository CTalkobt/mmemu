// TED registers, timers, interrupts and the raster/DMA sequencer.
//
// Ported from VICE (src/plus4/ted.c, ted-mem.c, ted-fetch.c, ted-badline.c,
// ted-irq.c, ted-timer.c, ted-timing.c), by Andreas Boose, Ettore
// Perazzoli and Tibor Biczo. GPL-2.0-or-later.
//
// VICE drives the chip from alarms on the CPU clock (draw at cycle 114 =
// cycle 0 of the next line, matrix fetch at cycle 4, raster IRQ at the line
// start, timer underflows). Here tick() walks the same events in TED
// double-clock cycles. Not ported: VICE's per-pixel "raster changes"
// (register writes take effect for the whole line drawn at its end, except
// the X scroll and 38/40 column switch, which VICE also defers to the next
// line past cycles 18/17/56), the RMW double-write special cases of
// $FF09/$FF0A/$FF0B (the CPU's dummy write does the same job), the
// open-border tricks, and the "grey dot" a colour register write draws.

#include "ted.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {
// ted-mem.c: unused bits in TED registers, always 1 when read.
const uint8_t UNUSED_BITS[64] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   // $FF00-$FF07
    0x00, 0x00, 0xA0, 0x00, 0xFC, 0x00, 0x00, 0x00,   // $FF08-$FF0F
    0x00, 0x00, 0x00, 0x00, 0x07, 0x80, 0x80, 0x80,   // $FF10-$FF17
    0x80, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   // $FF18-$FF1F
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF    // $FF3E/$FF3F
};

bool isIllegalMode(int m) { return m >= 5 && m <= 7; }
}  // namespace

Ted::Ted(Standard s) {
    m_back.assign((size_t)FRAME_WIDTH * PAL_FRAME_HEIGHT, 0xFF000000u);
    m_front = m_back;
    setStandard(s);
    Ted::reset();
}

void Ted::setStandard(Standard s) {
    m_std = s;
    // ted-timing.c, TED_NORMAL_BORDERS
    if (s == Standard::PAL) {
        m_screenHeight = PAL_LINES;
        m_vsyncLine = 257;
        m_firstDisplayedLine = 19;
        m_lastDisplayedLine = 306;
    } else {
        m_screenHeight = NTSC_LINES;
        m_vsyncLine = 229;
        m_firstDisplayedLine = 19;
        m_lastDisplayedLine = 260;
    }
    buildPalette();
    soundInit();
}

void Ted::setIrqLine(ISignalLine* line) {
    m_irqLine = line;
    m_irqOut = false;
    setIrqLineState();
}

void Ted::setIrqLineState() {
    // ted-irq.c: ted_irq_set_line()
    bool want;
    if (m_irqStatus & m_regs[0x0A] & 0xFE) {
        m_irqStatus |= 0x80;
        want = true;
    } else {
        m_irqStatus &= 0x7F;
        want = false;
    }
    if (want != m_irqOut) {
        m_irqOut = want;
        if (m_irqLine) m_irqLine->set(want);
    }
}

// ---------------------------------------------------------------------------
// Reset
// ---------------------------------------------------------------------------

void Ted::reset() {
    // ted_powerup() / ted_reset()
    std::memset(m_regs, 0, sizeof(m_regs));
    m_irqStatus = 0;
    m_rasterIrqLine = 0;
    m_allowBadLines = false;
    m_idleState = m_drawIdleState = false;
    m_forceDisplayState = false;
    m_memoryFetchDone = 0;
    m_memptr = m_memptrCol = m_memCounter = m_chrPosCount = 0;
    m_memCounterInc = 0;
    m_badLine = false;
    m_ycounterResetChecked = false;
    m_forceBlackOverscan = false;
    m_idleData = 0;
    m_kbdval = 0;

    m_t1Start = m_t2Start = m_t3Start = 0;
    for (int i = 0; i < 3; ++i) { m_tval[i] = 0; m_timerRunning[i] = false; }

    m_tvLine = 0;
    m_rasterCounter = (uint16_t)m_vsyncLine;
    m_cycle = 0;
    m_reverseMode = false;
    m_cursorVisible = false;
    m_cursorPhase = 0;
    m_crsrpos = 0;
    m_fastMode = true;
    m_blank = true;
    m_blankEnabled = true;
    m_blankThisLine = false;
    m_ysmooth = 0;
    m_ycounter = 0;
    m_characterFetchOn = false;
    m_lastVideoMode = -1;
    m_idleBgColor = 0;
    m_xsmooth = 0;
    m_xsmoothNext = -1;
    m_displayXStart = LEFT_BORDER;
    m_displayXStop = LEFT_BORDER + 320;
    m_displayXStartNext = m_displayXStopNext = -1;
    std::memset(m_vbuf, 0, sizeof(m_vbuf));
    std::memset(m_cbuf, 0, sizeof(m_cbuf));
    std::memset(m_cbufTmp, 0, sizeof(m_cbufTmp));

    m_stealPending = 0;
    m_totalCycles = 0;
    m_audioAcc = 0;
    m_audioRead = m_audioWrite = m_audioCount = 0;

    // ted_sound_reset()
    m_snd.voice0Sign = m_snd.voice1Sign = 0;
    m_snd.voice0Accu = m_snd.voice1Accu = 0;
    m_snd.noiseShiftRegister = 0xFF;
    m_snd.digital = 0;
    m_snd.voice0CachedOutput = m_snd.voice1CachedOutput = 0;
    m_snd.digitalCachedOutput = 0;
    for (uint8_t r = 0x0E; r <= 0x12; ++r) soundStore(r, 0);

    if (m_irqOut) {
        m_irqOut = false;
        if (m_irqLine) m_irqLine->set(false);
    }

    // ted_reset_registers(): $00-$3D written with 0, then $3E (ROM on)
    for (uint8_t r = 0; r <= 0x3D; ++r) store(r, 0);
    store(0x3E, 0);
}

// ---------------------------------------------------------------------------
// Register access
// ---------------------------------------------------------------------------

bool Ted::ioRead(IBus*, uint32_t addr, uint8_t* val) {
    if ((addr & 0xFFC0) != 0xFF00) return false;
    uint8_t reg = addr & 0x3F;
    if (reg >= 0x20 && reg < 0x3E) return false;          // RAM/ROM (plus4mem.c)
    if (reg >= 0x3E && romSelected()) return false;       // ROM is visible there
    *val = readReg(reg, true);
    return true;
}

bool Ted::ioPeek(IBus*, uint32_t addr, uint8_t* val) {
    if ((addr & 0xFFC0) != 0xFF00) return false;
    uint8_t reg = addr & 0x3F;
    if (reg >= 0x20 && reg < 0x3E) return false;
    if (reg >= 0x3E && romSelected()) return false;
    *val = readReg(reg, false);
    return true;
}

bool Ted::ioWrite(IBus*, uint32_t addr, uint8_t val) {
    if ((addr & 0xFFC0) != 0xFF00) return false;
    uint8_t reg = addr & 0x3F;
    if (reg >= 0x20 && reg < 0x3E) return false;          // written to RAM
    store(reg, val);
    return true;
}

uint8_t Ted::readReg(uint8_t reg, bool /*sideEffects: none of TED's reads change state here*/) {
    switch (reg) {
    case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05:
        return timerRead(reg);
    case 0x08:
        return m_kbdval;
    case 0x09:
        // ted09_read(): bits 0, 2 and 5 read 1. (VICE's ted09_peek uses 0x21,
        // leaving bit 2 clear; a peek here returns what a read would.)
        return (uint8_t)(m_irqStatus | 0x25);
    case 0x0A:
        return (uint8_t)((m_regs[0x0A] & 0x5F) | 0xA0);
    case 0x0E: case 0x0F: case 0x10: case 0x11:
        return soundRead(reg);
    case 0x12:
        return (uint8_t)((soundRead(0x12) & 3) | m_regs[0x12] | 0xC0);
    case 0x1A:
        return (uint8_t)(((m_memCounter & 0x300) >> 8) | 0xFC);
    case 0x1B:
        return (uint8_t)(m_memCounter & 0xFF);
    case 0x1C:
        return (uint8_t)(((m_rasterCounter & 0x100) >> 8) | 0xFE);
    case 0x1D:
        return (uint8_t)(m_rasterCounter & 0xFF);
    case 0x1E: {
        int xpos = (m_cycle - 16) * 4;
        if (xpos < 0) xpos = CYCLES_PER_LINE * 4 + xpos;
        return (uint8_t)((xpos / 2) & 0xFE);
    }
    case 0x1F:
        return (uint8_t)(0x80 | ((m_cursorPhase & 0x0F) << 3) | m_ycounter);
    default:
        return (uint8_t)(m_regs[reg] | UNUSED_BITS[reg]);
    }
}

void Ted::store(uint8_t reg, uint8_t value) {
    reg &= 0x3F;
    switch (reg) {
    case 0x00: timerStoreLow(0, value);  break;
    case 0x01: timerStoreHigh(0, value); break;
    case 0x02: timerStoreLow(1, value);  break;
    case 0x03: timerStoreHigh(1, value); break;
    case 0x04: timerStoreLow(2, value);  break;
    case 0x05: timerStoreHigh(2, value); break;
    case 0x06: store06(value); break;
    case 0x07: store07(value); break;
    case 0x08:
        // ted08_store(): the keyboard/joystick value is latched on the write
        m_regs[0x08] = value;
        m_kbdval = m_keyboard ? m_keyboard(value) : 0xFF;
        break;
    case 0x09:
        // write 1 to acknowledge; bit 7 is recomputed
        m_irqStatus &= ~((value & 0x5E) | 0x80);
        setIrqLineState();
        break;
    case 0x0A:
        m_regs[0x0A] = value & 0x5F;
        setIrqLineState();
        irqCheckState(value, true);
        break;
    case 0x0B:
        if (value == m_regs[0x0B]) break;
        m_regs[0x0B] = value;
        irqCheckState(value, false);
        break;
    case 0x0C: case 0x0D: {
        if (m_regs[reg] == value) break;
        if (reg & 1) m_crsrpos = (m_crsrpos & 0x300) | value;
        else         m_crsrpos = (m_crsrpos & 0xFF) | ((value & 3) << 8);
        m_regs[reg] = value;
        break;
    }
    case 0x0E: case 0x0F: case 0x10: case 0x11:
        soundStore(reg, value);
        break;
    case 0x12:
        m_regs[0x12] = value & 0x3C;
        soundStore(0x12, value);
        break;
    case 0x13:
        if ((m_regs[0x13] & 0xFE) == (value & 0xFE)) break;
        if ((m_regs[0x13] ^ value) & 2) m_fastMode = !(value & 2);
        m_regs[0x13] = (uint8_t)((m_regs[0x13] & 0x01) | (value & 0xFE));
        break;
    case 0x14:
        m_regs[0x14] = value;
        break;
    case 0x15:
        value &= 0x7F;
        if (!m_forceBlackOverscan) m_idleBgColor = value;
        m_regs[0x15] = value;
        break;
    case 0x16: case 0x17: case 0x18: case 0x19:
        m_regs[reg] = value & 0x7F;
        break;
    case 0x1A: case 0x1B:
        m_regs[reg] = value;
        if (reg == 0x1A) m_memCounter = ((value & 3) << 8) + (m_memCounter & 0xFF);
        else             m_memCounter = (m_memCounter & 0x300) | value;
        break;
    case 0x1C: case 0x1D:
        storeRaster(reg, value);
        break;
    case 0x1E:
        break;    // VICE: FIXME, the horizontal counter is not writable there
    case 0x1F:
        store1F(value);
        break;
    case 0x3E:
        m_regs[0x13] |= 0x01;
        if (m_romSelect) m_romSelect(true);
        break;
    case 0x3F:
        m_regs[0x13] &= 0xFE;
        if (m_romSelect) m_romSelect(false);
        break;
    default:
        break;
    }
}

void Ted::store06(uint8_t value) {
    const int cycle = m_cycle;
    const unsigned line = m_rasterCounter;

    // bad line tricks
    if (line == FIRST_DMA_LINE && (value & 0x10)) {
        m_allowBadLines = true;
        m_ycounter = 0;
    }
    if (m_ysmooth != (value & 7) && line <= (unsigned)LAST_DMA_LINE) {
        // ted_badline_check_state()
        bool wasBad = m_badLine;
        bool nowBad = m_allowBadLines && (int)(value & 7) == (int)(line & 7);
        if (wasBad && !nowBad) {
            // line_becomes_good()
            m_badLine = false;
            if (cycle > 0) {
                m_drawIdleState = m_idleState = false;
                if (cycle > FETCH_CYCLE + 2 && !m_ycounterResetChecked) m_ycounterResetChecked = true;
            }
        }
    }
    m_ysmooth = value & 7;

    // check_lower_upper_border(): 24 <-> 25 row switch
    if ((value ^ m_regs[0x06]) & 8) {
        if (value & 8) {
            if (line == (unsigned)ROW24_STOP && cycle > 0) m_blankEnabled = true;
            else if (!m_blank && line == (unsigned)ROW24_START && cycle > 0) m_blankEnabled = false;
        } else {
            if (!m_blank && line == (unsigned)ROW25_START && cycle > 0) m_blankEnabled = false;
            else if (line == (unsigned)ROW25_STOP && cycle > 0) m_blankEnabled = true;
        }
    }

    m_blank = !(value & 0x10);    // DEN
    m_regs[0x06] = value;
    updateVideoMode();
}

void Ted::store07(uint8_t value) {
    const int cycle = m_cycle;
    if ((value & 7) != (m_regs[0x07] & 7)) {
        if ((cycle - 15) / 2 <= 1) { m_xsmooth = value & 7; m_xsmoothNext = -1; }
        else m_xsmoothNext = value & 7;
    }
    // check_lateral_border(): bit 3 selects 38/40 columns
    if ((value & 8) != (m_regs[0x07] & 8)) {
        int start = (value & 8) ? LEFT_BORDER : LEFT_BORDER + 7;
        int stop  = (value & 8) ? LEFT_BORDER + 320 : LEFT_BORDER + 311;
        if (cycle <= 17) { m_displayXStart = start; m_displayXStartNext = -1; }
        else m_displayXStartNext = start;
        if (cycle <= 56) { m_displayXStop = stop; m_displayXStopNext = -1; }
        else m_displayXStopNext = stop;
        // CSEL 0 -> 1 at cycle 17: the border is not turned off on this line
        if ((value & 8) && cycle == 17 && !(m_regs[0x07] & 8)) m_blankThisLine = true;
    }
    m_reverseMode = (value & 0x80) != 0;
    m_regs[0x07] = value;
    updateVideoMode();
}

void Ted::updateVideoMode() {
    int mode = videoMode();
    if (mode != m_lastVideoMode) {
        if (isIllegalMode(mode)) {
            m_idleBgColor = 0;           // overscan forced to black
            m_forceBlackOverscan = true;
        } else {
            m_idleBgColor = m_regs[0x15] & 0x7F;
            m_forceBlackOverscan = false;
        }
        m_lastVideoMode = mode;
    }
}

void Ted::irqCheckState(uint8_t value, bool high) {
    // ted-irq.c: ted_irq_check_state()
    unsigned irqLine = high ? ((m_rasterIrqLine & 0xFF) | ((value & 1) << 8))
                            : ((m_rasterIrqLine & 0x100) | value);
    if (irqLine == m_rasterIrqLine) return;
    unsigned old = m_rasterIrqLine;
    m_rasterIrqLine = (uint16_t)irqLine;
    // writing the current line as the compare value raises the IRQ at once
    if ((m_regs[0x0A] & 0x02) && irqLine == m_rasterCounter && m_rasterCounter != old) {
        m_irqStatus |= 0x02;
        setIrqLineState();
    }
}

void Ted::storeRaster(uint8_t reg, uint8_t value) {
    m_regs[reg] = value;
    if (reg == 0x1C) m_rasterCounter = (uint16_t)(((value & 1) << 8) + (m_rasterCounter & 0xFF));
    else             m_rasterCounter = (uint16_t)((m_rasterCounter & 0x100) + value);
}

void Ted::store1F(uint8_t value) {
    int newCount = (value >> 3) & 0x0F;
    int phase = m_cursorPhase & 0x10;
    if ((m_cursorPhase & 0x0F) == 0x0F && newCount != 0x0F) phase ^= 0x10;
    m_cursorPhase = phase | newCount;
    m_cursorVisible = (m_cursorPhase & 0x10) != 0;
    m_ycounter = value & 7;
}

// ---------------------------------------------------------------------------
// Timers (ted-timer.c): values held in double-clock cycles, so they count
// down once every two ticks; reads return value >> 1.
// ---------------------------------------------------------------------------

void Ted::timerStoreLow(int t, uint8_t value) {
    if (t == 0) {
        m_t1Start = (uint16_t)((m_t1Start & 0xFF00) | value);
        m_tval[0] = (int64_t)m_t1Start << 1;
    } else if (t == 1) {
        m_t2Start = (uint16_t)((m_t2Start & 0xFF00) | value);
        m_tval[1] = (int64_t)m_t2Start << 1;
    } else {
        m_t3Start = (uint16_t)((m_t3Start & 0xFF00) | value);
        m_tval[2] = (int64_t)m_t3Start << 1;
    }
    m_timerRunning[t] = false;
}

void Ted::timerStoreHigh(int t, uint8_t value) {
    uint16_t start;
    if (t == 0)      start = m_t1Start = (uint16_t)((m_t1Start & 0x00FF) | (value << 8));
    else if (t == 1) start = m_t2Start = (uint16_t)((m_t2Start & 0x00FF) | (value << 8));
    else             start = m_t3Start = (uint16_t)((m_t3Start & 0x00FF) | (value << 8));
    // the alarm comes after start (0 = 65536) single cycles
    m_tval[t] = (int64_t)(start == 0 ? 65536 : start) * 2;
    m_timerRunning[t] = true;
}

uint8_t Ted::timerRead(int reg) const {
    int t = reg >> 1;
    uint32_t v = (uint32_t)m_tval[t];
    return (reg & 1) ? (uint8_t)(v >> 9) : (uint8_t)(v >> 1);
}

void Ted::timersAdvance(int64_t n) {
    for (int t = 0; t < 3; ++t) {
        if (!m_timerRunning[t]) continue;
        m_tval[t] -= n;
        while (m_tval[t] <= 0) {
            if (t == 0) {
                // timer 1 reloads from its latch
                m_tval[0] += (int64_t)(m_t1Start == 0 ? 65536 : m_t1Start) * 2;
                m_irqStatus |= 0x08;
            } else {
                // timers 2 and 3 run on from $FFFF
                m_tval[t] += 65536 * 2;
                if (t == 1) { m_t2Start = 0; m_irqStatus |= 0x10; }
                else        { m_t3Start = 0; m_irqStatus |= 0x40; }
            }
            setIrqLineState();
        }
    }
}

// ---------------------------------------------------------------------------
// Clocking
// ---------------------------------------------------------------------------

bool Ted::cpuSingleClock() const {
    if (!m_fastMode) return true;                    // $FF13 bit 1
    if (m_cycle >= 4 && m_cycle < 101) {
        if (m_characterFetchOn) return true;         // every even cycle 4-100 is TED's
        if (m_cycle >= 91) return true;              // 5 refresh cycles, 92-100
    }
    return false;
}

void Ted::advance(int n) {
    timersAdvance(n);
    m_audioAcc += (uint64_t)n * (uint64_t)m_sampleRate;
    const uint64_t hz = clockHz(m_std);
    while (m_audioAcc >= hz) {
        m_audioAcc -= hz;
        renderSample();
    }
    m_totalCycles += (uint64_t)n;
    m_cycle += n;
}

void Ted::tick(uint64_t cycles) {
    while (cycles > 0) {
        int target = m_cycle < FETCH_CYCLE ? FETCH_CYCLE : CYCLES_PER_LINE;
        int n = (int)std::min<uint64_t>(cycles, (uint64_t)(target - m_cycle));
        advance(n);
        cycles -= (uint64_t)n;
        if (m_cycle == FETCH_CYCLE && m_rasterCounter <= LAST_DMA_LINE) doMatrixFetch();
        if (m_cycle >= CYCLES_PER_LINE) {
            endOfLine();
            m_cycle = 0;
            lineStart();
        }
    }
}

void Ted::doMatrixFetch() {
    // ted-fetch.c: do_matrix_fetch(), at cycle 4 of lines 0-$CB
    if (m_memoryFetchDone) return;
    m_memoryFetchDone = 1;
    m_memCounter = m_memptrCol;
    m_chrPosCount = m_memptr;

    const unsigned line = m_rasterCounter;
    if ((line & 7) == (unsigned)((m_ysmooth + 1) & 7) && m_allowBadLines
        && line > (unsigned)FIRST_DMA_LINE && line <= (unsigned)LAST_DMA_LINE) {
        // character pointers (screen matrix) for the row
        const uint16_t base = screenBase();
        const bool rom = romSelected();
        for (int i = 0; i < 40; ++i) {
            uint16_t a = (uint16_t)(base + ((m_memptrCol + i) & 0x3FF));
            m_vbuf[i] = fetchVideo(a, rom && a >= 0x8000);
        }
        m_drawIdleState = false;
        m_idleState = false;
        m_ycounterResetChecked = true;
        m_memoryFetchDone = 2;
        m_stealPending += DMA_STEAL;
        m_badLine = true;
    }

    if ((line & 7) == (unsigned)m_ysmooth && m_allowBadLines
        && line >= (unsigned)FIRST_DMA_LINE && line < (unsigned)LAST_DMA_LINE) {
        // attributes (colour/luminance) for the next row, one line early
        const uint16_t base = attributeBase();
        const bool rom = romSelected();
        for (int i = 0; i < 40; ++i) {
            uint16_t a = (uint16_t)(base + ((m_memptrCol + i) & 0x3FF));
            m_cbufTmp[i] = fetchVideo(a, rom && a >= 0x8000);
        }
        m_stealPending += DMA_STEAL;
        m_badLine = true;
    }
}

void Ted::endOfLine() {
    // ted.c: ted_raster_draw_alarm_handler()
    const bool inVisibleArea = m_tvLine >= m_firstDisplayedLine && m_tvLine <= m_lastDisplayedLine;
    if (inVisibleArea && m_tvLine < m_screenHeight) drawLine(m_tvLine - m_firstDisplayedLine);

    // changes VICE defers to the next line
    m_blankThisLine = false;
    if (m_xsmoothNext >= 0)      { m_xsmooth = m_xsmoothNext; m_xsmoothNext = -1; }
    if (m_displayXStartNext >= 0) { m_displayXStart = m_displayXStartNext; m_displayXStartNext = -1; }
    if (m_displayXStopNext >= 0)  { m_displayXStop = m_displayXStopNext; m_displayXStopNext = -1; }

    if (m_rasterCounter == LAST_DMA_LINE) m_idleState = true;

    ++m_tvLine;
    if (m_tvLine == m_screenHeight) {
        // raster.c ends the frame when its line count reaches the screen
        // height; ted.c ends it early only for a short frame (below)
        m_front = m_back;
        ++m_frameCount;
    }
    ++m_rasterCounter;
    if (m_rasterCounter == m_screenHeight) {
        m_memptr = m_memptrCol = m_memCounter = m_chrPosCount = 0;
        m_rasterCounter = 0;
        if (!m_blank) m_characterFetchOn = true;
        m_ycounter = 0;
    }
    if (m_rasterCounter == 512) m_rasterCounter = 0;
    if (m_rasterCounter == 0xCC) m_characterFetchOn = false;

    // vertical border flip-flop
    if (m_regs[0x06] & 8) {
        if (m_rasterCounter == ROW25_START && !m_blank) m_blankEnabled = false;
        if (m_rasterCounter == ROW25_STOP + 1) m_blankEnabled = true;
    } else {
        if (m_rasterCounter == ROW24_START && !m_blank) m_blankEnabled = false;
        if (m_rasterCounter == ROW24_STOP + 1) m_blankEnabled = true;
    }

    // vertical sync (or the screen rolls when the counter skips it)
    if ((m_tvLine - m_screenHeight) > 40 || m_rasterCounter == m_vsyncLine) {
        if (m_tvLine < m_screenHeight) {
            m_front = m_back;
            ++m_frameCount;
        }
        m_tvLine = 0;
        m_cursorPhase = (m_cursorPhase + 1) & 0x1F;
        m_cursorVisible = (m_cursorPhase & 0x10) != 0;
    }

    if (inVisibleArea) {
        if (!m_idleState) {
            m_memCounter = (m_memCounter + m_memCounterInc) & 0x3FF;
            m_chrPosCount = (m_chrPosCount + m_memCounterInc) & 0x3FF;
        }
        m_memCounterInc = 40;
        if (m_ycounter == 6) m_memptrCol = m_memCounter;
        if (m_ycounter == 7) m_memptr = m_chrPosCount;
        if (!m_idleState && m_allowBadLines) m_ycounter = (m_ycounter + 1) & 7;
        if (m_forceDisplayState) {
            m_idleState = false;
            m_forceDisplayState = false;
        }
        m_drawIdleState = m_idleState;
    }

    m_ycounterResetChecked = false;
    m_memoryFetchDone = 0;

    if (m_rasterCounter == FIRST_DMA_LINE) m_allowBadLines = !m_blank;
    if (m_allowBadLines && (m_rasterCounter & 7) == ((m_ysmooth + 1) & 7))
        std::memcpy(m_cbuf, m_cbufTmp, (size_t)std::min(m_memCounterInc, 40));
    if (m_idleState) m_idleData = fetchVideo(0xFFFF, false);   // VICE: mem_ram[0xffff]
}

void Ted::lineStart() {
    // raster compare IRQ at the start of the line (ted-irq.c alarm)
    if (m_rasterCounter == m_rasterIrqLine) {
        m_irqStatus |= 0x02;
        setIrqLineState();
    }
}

// ---------------------------------------------------------------------------
// Debugger
// ---------------------------------------------------------------------------

uint16_t Ted::charBase() const {
    uint8_t mask = ((m_regs[0x06] & 0x40) | (m_regs[0x07] & 0x80)) ? 0xF8 : 0xFC;
    return (uint16_t)((m_regs[0x13] & mask) << 8);
}

void Ted::getDeviceInfo(DeviceInfo& out) const {
    out.name = name();
    out.baseAddr = baseAddr();
    out.addrMask = addrMask();
    static const char* const names[0x20] = {
        "T1LO", "T1HI", "T2LO", "T2HI", "T3LO", "T3HI", "CTRL1", "CTRL2",
        "KBD", "IRQFLAG", "IRQMASK", "RASTERIRQ", "CURSORHI", "CURSORLO", "V1FREQLO", "V2FREQLO",
        "V2FREQHI", "SOUNDCTRL", "BITMAP", "CHARBASE", "SCREENBASE", "BGCOL0", "BGCOL1", "BGCOL2",
        "BGCOL3", "BORDER", "CHARPOSHI", "CHARPOSLO", "RASTERHI", "RASTERLO", "HPOS", "FLASH"
    };
    static const char* const desc[0x20] = {
        "Timer 1 low (latch)", "Timer 1 high (starts)", "Timer 2 low", "Timer 2 high",
        "Timer 3 low", "Timer 3 high", "ECM/BMM/DEN/RSEL/YSCROLL", "RVS-off/PAL/freeze/MCM/CSEL/XSCROLL",
        "Keyboard latch", "IRQ flags (write 1 to clear)", "IRQ enable / raster bit 8", "Raster compare",
        "Cursor position bits 9-8", "Cursor position bits 7-0", "Voice 1 frequency low", "Voice 2 frequency low",
        "Voice 2 frequency high", "DA/noise/voice enables/volume", "Bitmap base, char ROM, voice 1 freq high",
        "Char base, single clock, ROM status", "Screen/attribute base", "Background 0", "Background 1",
        "Background 2", "Background 3", "Border", "Character position high", "Character position low",
        "Raster line bit 8", "Raster line", "Horizontal position", "Flash counter / row counter"
    };
    for (int r = 0; r < 0x20; ++r) {
        uint8_t v = const_cast<Ted*>(this)->readReg((uint8_t)r, false);
        DeviceRegister reg{names[r], (uint32_t)r, v, desc[r], {}};
        switch (r) {
        case 0x06: reg.bitFields = {{"YSCROLL", 0, 3}, {"RSEL", 3, 1}, {"DEN", 4, 1}, {"BMM", 5, 1}, {"ECM", 6, 1}, {"TEST", 7, 1}}; break;
        case 0x07: reg.bitFields = {{"XSCROLL", 0, 3}, {"CSEL", 3, 1}, {"MCM", 4, 1}, {"FREEZE", 5, 1}, {"NTSC", 6, 1}, {"RVSOFF", 7, 1}}; break;
        case 0x09: case 0x0A: reg.bitFields = {{"RASTER", 1, 1}, {"LPEN", 2, 1}, {"T1", 3, 1}, {"T2", 4, 1}, {"T3", 6, 1}, {"IRQ", 7, 1}}; break;
        case 0x11: reg.bitFields = {{"VOLUME", 0, 4}, {"V1ON", 4, 1}, {"V2ON", 5, 1}, {"NOISE", 6, 1}, {"DA", 7, 1}}; break;
        case 0x12: reg.bitFields = {{"V1HI", 0, 2}, {"CHARROM", 2, 1}, {"BITMAP", 3, 3}}; break;
        case 0x13: reg.bitFields = {{"ROMON", 0, 1}, {"SINGLECLK", 1, 1}, {"CHARBASE", 2, 6}}; break;
        default: break;
        }
        out.registers.push_back(reg);
    }
    out.registers.push_back({"ROMSEL", 0x3E, (uint8_t)(romSelected() ? 1 : 0), "$FF3E write: ROM on, $FF3F write: RAM on", {}});

    static const char* const modes[8] = {"Standard text", "Multicolour text", "Hires bitmap", "Multicolour bitmap",
                                         "Extended colour text", "Illegal text", "Illegal bitmap 1", "Illegal bitmap 2"};
    char buf[64];
    auto add = [&](const char* k, const std::string& v) { out.state.push_back({k, v}); };
    add("Standard", m_std == Standard::PAL ? "PAL" : "NTSC");
    std::snprintf(buf, sizeof buf, "%u, cycle %d", m_rasterCounter, m_cycle);   add("Raster", buf);
    std::snprintf(buf, sizeof buf, "%u", m_rasterIrqLine);                      add("Raster compare", buf);
    add("Mode", modes[videoMode() & 7]);
    add("Display", m_blank ? "blanked (DEN off)" : "on");
    add("Clock", m_fastMode ? "double (fast)" : "single (forced)");
    for (int t = 0; t < 3; ++t) {
        std::snprintf(buf, sizeof buf, "$%04X %s", timerValue(t), m_timerRunning[t] ? "running" : "stopped");
        char k[16]; std::snprintf(k, sizeof k, "Timer %d", t + 1);
        add(k, buf);
    }
    std::snprintf(buf, sizeof buf, "$%04X", m_t1Start);                         add("Timer 1 latch", buf);
    std::snprintf(buf, sizeof buf, "$%04X", screenBase());                      add("Screen", buf);
    std::snprintf(buf, sizeof buf, "$%04X", attributeBase());                   add("Attributes", buf);
    std::snprintf(buf, sizeof buf, "$%04X (%s)", charBase(), (m_regs[0x12] & 4) ? "ROM" : "RAM"); add("Characters", buf);
    std::snprintf(buf, sizeof buf, "$%04X (%s)", bitmapBase(), (m_regs[0x12] & 4) ? "ROM" : "RAM"); add("Bitmap", buf);
    std::snprintf(buf, sizeof buf, "%d (%s)", m_crsrpos, m_cursorVisible ? "visible" : "hidden"); add("Cursor", buf);
    add("Upper 32K", romSelected() ? "ROM" : "RAM");
    std::snprintf(buf, sizeof buf, "$%03X / $%03X", voiceFreq(0), voiceFreq(1)); add("Voice freqs", buf);
}

std::vector<std::pair<std::string, uint32_t>> Ted::getDerivedValues() const {
    return {
        {"RASTER", m_rasterCounter},
        {"CYCLE", (uint32_t)m_cycle},
        {"SCREEN", screenBase()},
        {"ATTR", attributeBase()},
        {"CHARSET", charBase()},
        {"BITMAP", bitmapBase()},
        {"T1", timerValue(0)},
        {"T2", timerValue(1)},
        {"T3", timerValue(2)},
        {"MODE", (uint32_t)videoMode()},
    };
}
