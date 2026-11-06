// Atari 2600 TIA, after Stella (src/emucore/tia/TIA.cxx), GPL-2.0-or-later,
// Christian Speckner (DirtyHairy), Stephen Anthony and the Stella team.
// See tia.h for what is left out.

#include "tia.h"
#include "tia_palette.h"

#include <algorithm>
#include <cmath>

using namespace tiacore;

namespace {

// Register numbers (TIAConstants.hxx TIARegister)
enum : uint8_t {
    VSYNC = 0x00, VBLANK = 0x01, WSYNC = 0x02, RSYNC = 0x03, NUSIZ0 = 0x04, NUSIZ1 = 0x05,
    COLUP0 = 0x06, COLUP1 = 0x07, COLUPF = 0x08, COLUBK = 0x09, CTRLPF = 0x0a, REFP0 = 0x0b,
    REFP1 = 0x0c, PF0 = 0x0d, PF1 = 0x0e, PF2 = 0x0f, RESP0 = 0x10, RESP1 = 0x11, RESM0 = 0x12,
    RESM1 = 0x13, RESBL = 0x14, AUDC0 = 0x15, AUDC1 = 0x16, AUDF0 = 0x17, AUDF1 = 0x18,
    AUDV0 = 0x19, AUDV1 = 0x1a, GRP0 = 0x1b, GRP1 = 0x1c, ENAM0 = 0x1d, ENAM1 = 0x1e,
    ENABL = 0x1f, HMP0 = 0x20, HMP1 = 0x21, HMM0 = 0x22, HMM1 = 0x23, HMBL = 0x24,
    VDELP0 = 0x25, VDELP1 = 0x26, VDELBL = 0x27, RESMP0 = 0x28, RESMP1 = 0x29, HMOVE = 0x2a,
    HMCLR = 0x2b, CXCLR = 0x2c,

    CXM0P = 0x00, CXM1P = 0x01, CXP0FB = 0x02, CXP1FB = 0x03, CXM0FB = 0x04, CXM1FB = 0x05,
    CXBLPF = 0x06, CXPPMM = 0x07, INPT0 = 0x08, INPT1 = 0x09, INPT2 = 0x0a, INPT3 = 0x0b,
    INPT4 = 0x0c, INPT5 = 0x0d
};

// Pseudo-registers for the delay queue (TIA::DummyRegisters)
enum : uint8_t { SHUFFLE_P0 = 0xF0, SHUFFLE_P1 = 0xF1, SHUFFLE_BL = 0xF2 };

// Colour-clock delays of pipelined writes (TIA.cxx Delay). With Stella's
// developer settings off, PF writes take Delay::pf and the player / ball
// pattern shuffles take 1.
enum : uint8_t {
    DELAY_HMOVE = 6, DELAY_PF = 2, DELAY_GRP = 1, DELAY_SHUFFLE_PLAYER = 1, DELAY_SHUFFLE_BALL = 1,
    DELAY_HMP = 2, DELAY_HMM = 2, DELAY_HMBL = 2, DELAY_HMCLR = 2, DELAY_REFP = 1, DELAY_ENABL = 1,
    DELAY_ENAM = 1, DELAY_VBLANK = 1
};

// RESPx/RESMx/RESBL counter targets (TIA.cxx ResxCounter)
enum : uint8_t { RESX_HBLANK = 159, RESX_LATE_HBLANK = 158, RESX_FRAME = 157 };
constexpr uint8_t RESX_LATE_HBLANK_THRESHOLD = 73;

constexpr uint8_t COLOR_HBLANK = 0;   ///< the HMOVE comb colour (myColorHBlank, debug colours off)

bool both(uint16_t mask, uint16_t a, uint16_t b) { return (mask & a & b) != 0; }

}  // namespace

Tia::Tia(Standard s)
    : m_std(s),
      m_palette(NTSC_PALETTE),
      m_playfield(cmask::PLAYFIELD),
      m_missile0(cmask::MISSILE0),
      m_missile1(cmask::MISSILE1),
      m_player0(cmask::PLAYER0),
      m_player1(cmask::PLAYER1),
      m_ball(cmask::BALL),
      m_backBuffer(H_PIXEL * FrameManager::BASE_HEIGHT_PAL, 0),
      m_frontBuffer(H_PIXEL * FrameManager::BASE_HEIGHT_PAL, 0),
      m_audioBuf(AUDIO_BUF, 0.0f) {
    setStandard(s);
}

void Tia::setStandard(Standard s) {
    m_std = s;
    m_palette = s == Standard::NTSC ? NTSC_PALETTE : s == Standard::PAL ? PAL_PALETTE : SECAM_PALETTE;
    m_frameManager.setPal(s != Standard::NTSC);
    reset();
}

void Tia::reset() {
    m_hctr = 0;
    m_movementInProgress = false;
    m_extendedHblank = false;
    m_movementClock = 0;
    m_priority = Priority::NORMAL;
    m_hblank = true;
    m_collisionMask = cmask::NONE;
    m_hctrDelta = 0;
    m_halt = false;

    m_shadow.fill(0);
    for (uint8_t reg : {HMP0, HMP1, HMM0, HMM1, HMBL}) m_shadow[reg] = 0x80;

    m_background.reset();
    m_playfield.reset();
    m_missile0.reset();
    m_missile1.reset();
    m_player0.reset();
    m_player1.reset();
    m_ball.reset();

    m_input0.reset();
    m_input1.reset();

    m_audio.reset();

    m_timestamp = 0;
    const bool pal = m_std != Standard::NTSC;
    for (size_t i = 0; i < m_analogReadouts.size(); ++i) {
        m_analogReadouts[i].reset(m_timestamp, pal);
        m_analogReadouts[i].update(m_analogConnections[i], m_timestamp);
    }

    m_delayQueue.reset();
    m_frameManager.reset();

    std::fill(m_backBuffer.begin(), m_backBuffer.end(), 0);
    std::fill(m_frontBuffer.begin(), m_frontBuffer.end(), 0);

    m_audioRead = m_audioWrite = m_audioCount = 0;
    m_audioAcc = 0;
}

// ---------------------------------------------------------------------------
// Register reads
// ---------------------------------------------------------------------------

uint8_t Tia::collisionBits(uint8_t reg) const {
    const uint16_t m = m_collisionMask;
    switch (reg) {
        case CXM0P:
            return (uint8_t)((both(m, cmask::MISSILE0, cmask::PLAYER0) ? 0x40 : 0) |
                             (both(m, cmask::MISSILE0, cmask::PLAYER1) ? 0x80 : 0));
        case CXM1P:
            return (uint8_t)((both(m, cmask::MISSILE1, cmask::PLAYER1) ? 0x40 : 0) |
                             (both(m, cmask::MISSILE1, cmask::PLAYER0) ? 0x80 : 0));
        case CXP0FB:
            return (uint8_t)((both(m, cmask::PLAYER0, cmask::BALL) ? 0x40 : 0) |
                             (both(m, cmask::PLAYER0, cmask::PLAYFIELD) ? 0x80 : 0));
        case CXP1FB:
            return (uint8_t)((both(m, cmask::PLAYER1, cmask::BALL) ? 0x40 : 0) |
                             (both(m, cmask::PLAYER1, cmask::PLAYFIELD) ? 0x80 : 0));
        case CXM0FB:
            return (uint8_t)((both(m, cmask::MISSILE0, cmask::BALL) ? 0x40 : 0) |
                             (both(m, cmask::MISSILE0, cmask::PLAYFIELD) ? 0x80 : 0));
        case CXM1FB:
            return (uint8_t)((both(m, cmask::MISSILE1, cmask::BALL) ? 0x40 : 0) |
                             (both(m, cmask::MISSILE1, cmask::PLAYFIELD) ? 0x80 : 0));
        case CXBLPF:
            return both(m, cmask::BALL, cmask::PLAYFIELD) ? 0x80 : 0;
        case CXPPMM:
            return (uint8_t)((both(m, cmask::MISSILE0, cmask::MISSILE1) ? 0x40 : 0) |
                             (both(m, cmask::PLAYER0, cmask::PLAYER1) ? 0x80 : 0));
        default:
            return 0;
    }
}

uint8_t Tia::readRegister(uint8_t reg, bool sideEffects) {
    reg &= 0x0F;
    if (reg <= CXPPMM) return collisionBits(reg);

    switch (reg) {
        case INPT0:
        case INPT1:
        case INPT2:
        case INPT3: {
            auto& a = m_analogReadouts[reg - INPT0];
            return (sideEffects ? a.inpt(m_timestamp) : a.peek(m_timestamp)) & 0x80;
        }
        case INPT4:
            return (sideEffects ? m_input0.inpt(m_buttons[0]) : m_input0.peek(m_buttons[0])) & 0x80;
        case INPT5:
            return (sideEffects ? m_input1.inpt(m_buttons[1]) : m_input1.peek(m_buttons[1])) & 0x80;
        default:
            return 0;
    }
}

uint8_t Tia::read(uint8_t reg, uint8_t dataBus) {
    // D5-D0 are not driven by the TIA: the last byte on the data bus shows through
    return (uint8_t)(readRegister(reg, true) | (dataBus & 0x3F));
}

uint8_t Tia::peek(uint8_t reg) const {
    return const_cast<Tia*>(this)->readRegister(reg, false);
}

// ---------------------------------------------------------------------------
// Register writes
// ---------------------------------------------------------------------------

uint8_t Tia::resxCounter() const {
    return m_hblank ? (m_hctr >= RESX_LATE_HBLANK_THRESHOLD ? RESX_LATE_HBLANK : RESX_HBLANK) : RESX_FRAME;
}

void Tia::write(uint8_t reg, uint8_t value) {
    const uint8_t address = reg & 0x3F;

    switch (address) {
        case WSYNC:
            m_halt = true;
            break;

        case RSYNC:
            applyRsync();
            m_shadow[address] = value;
            break;

        case VSYNC:
            if (m_frameManager.setVsync((value & 0x02) != 0)) onFrameComplete();
            m_shadow[address] = value;
            break;

        case VBLANK:
            m_input0.vblank(value);
            m_input1.vblank(value);
            for (auto& a : m_analogReadouts) a.vblank(value, m_timestamp);
            m_delayQueue.push(VBLANK, value, DELAY_VBLANK);
            break;

        case AUDV0: m_audio.channel0().audv(value); m_shadow[address] = value; break;
        case AUDV1: m_audio.channel1().audv(value); m_shadow[address] = value; break;
        case AUDF0: m_audio.channel0().audf(value); m_shadow[address] = value; break;
        case AUDF1: m_audio.channel1().audf(value); m_shadow[address] = value; break;
        case AUDC0: m_audio.channel0().audc(value); m_shadow[address] = value; break;
        case AUDC1: m_audio.channel1().audc(value); m_shadow[address] = value; break;

        case HMOVE:
            m_delayQueue.push(HMOVE, value, DELAY_HMOVE);
            break;

        case COLUBK:
            value &= 0xFE;
            m_background.setColor(value);
            m_shadow[address] = value;
            break;

        case COLUP0:
            value &= 0xFE;
            m_playfield.setColorP0(value);
            m_missile0.setColor(value);
            m_player0.setColor(value);
            m_shadow[address] = value;
            break;

        case COLUP1:
            value &= 0xFE;
            m_playfield.setColorP1(value);
            m_missile1.setColor(value);
            m_player1.setColor(value);
            m_shadow[address] = value;
            break;

        case CTRLPF:
            m_priority = (value & 0x04) ? Priority::PFP : (value & 0x02) ? Priority::SCORE : Priority::NORMAL;
            m_playfield.ctrlpf(value);
            m_ball.ctrlpf(value);
            m_shadow[address] = value;
            break;

        case COLUPF:
            value &= 0xFE;
            m_playfield.setColor(value);
            m_ball.setColor(value);
            m_shadow[address] = value;
            break;

        case PF0: m_delayQueue.push(PF0, value, DELAY_PF); break;
        case PF1: m_delayQueue.push(PF1, value, DELAY_PF); break;
        case PF2: m_delayQueue.push(PF2, value, DELAY_PF); break;

        case ENAM0: m_delayQueue.push(ENAM0, value, DELAY_ENAM); break;
        case ENAM1: m_delayQueue.push(ENAM1, value, DELAY_ENAM); break;

        case RESM0:
            m_missile0.resm(resxCounter(), m_hblank);
            m_shadow[address] = value;
            break;

        case RESM1:
            m_missile1.resm(resxCounter(), m_hblank);
            m_shadow[address] = value;
            break;

        case RESMP0: m_missile0.resmp(value); m_shadow[address] = value; break;
        case RESMP1: m_missile1.resmp(value); m_shadow[address] = value; break;

        case NUSIZ0:
            m_missile0.nusiz(value);
            m_player0.nusiz(value, m_hblank);
            m_shadow[address] = value;
            break;

        case NUSIZ1:
            m_missile1.nusiz(value);
            m_player1.nusiz(value, m_hblank);
            m_shadow[address] = value;
            break;

        case HMM0: m_delayQueue.push(HMM0, value, DELAY_HMM); break;
        case HMM1: m_delayQueue.push(HMM1, value, DELAY_HMM); break;
        case HMCLR: m_delayQueue.push(HMCLR, value, DELAY_HMCLR); break;

        case GRP0:
            m_delayQueue.push(GRP0, value, DELAY_GRP);
            m_delayQueue.push(SHUFFLE_P1, 0, DELAY_SHUFFLE_PLAYER);
            break;

        case GRP1:
            m_delayQueue.push(GRP1, value, DELAY_GRP);
            m_delayQueue.push(SHUFFLE_P0, 0, DELAY_SHUFFLE_PLAYER);
            m_delayQueue.push(SHUFFLE_BL, 0, DELAY_SHUFFLE_BALL);
            break;

        case RESP0:
            m_player0.resp(resxCounter());
            m_shadow[address] = value;
            break;

        case RESP1:
            m_player1.resp(resxCounter());
            m_shadow[address] = value;
            break;

        case REFP0: m_delayQueue.push(REFP0, value, DELAY_REFP); break;
        case REFP1: m_delayQueue.push(REFP1, value, DELAY_REFP); break;

        case VDELP0: m_player0.vdelp(value); m_shadow[address] = value; break;
        case VDELP1: m_player1.vdelp(value); m_shadow[address] = value; break;

        case HMP0: m_delayQueue.push(HMP0, value, DELAY_HMP); break;
        case HMP1: m_delayQueue.push(HMP1, value, DELAY_HMP); break;

        case ENABL: m_delayQueue.push(ENABL, value, DELAY_ENABL); break;

        case RESBL:
            m_ball.resbl(resxCounter());
            m_shadow[address] = value;
            break;

        case VDELBL: m_ball.vdelbl(value); m_shadow[address] = value; break;
        case HMBL: m_delayQueue.push(HMBL, value, DELAY_HMBL); break;

        case CXCLR:
            m_collisionMask = cmask::NONE;
            m_shadow[address] = value;
            break;

        default:
            break;
    }
}

/** A pipelined write coming due (TIA::delayedWrite). */
void Tia::delayedWrite(uint8_t address, uint8_t value) {
    if (address < 64) m_shadow[address] = value;

    switch (address) {
        case VBLANK:
            m_frameManager.setVblank((value & 0x02) != 0);
            break;

        case HMOVE:
            m_movementClock = 0;
            m_movementInProgress = true;
            if (!m_extendedHblank) {
                clearHmoveComb();
                m_extendedHblank = true;
            }
            m_missile0.startMovement();
            m_missile1.startMovement();
            m_player0.startMovement();
            m_player1.startMovement();
            m_ball.startMovement();
            break;

        case PF0: m_playfield.pf0(value); break;
        case PF1: m_playfield.pf1(value); break;
        case PF2: m_playfield.pf2(value); break;

        case HMM0: m_missile0.hmm(value); break;
        case HMM1: m_missile1.hmm(value); break;

        case HMCLR:
            m_missile0.hmm(0); m_shadow[HMM0] = 0;
            m_missile1.hmm(0); m_shadow[HMM1] = 0;
            m_player0.hmp(0);  m_shadow[HMP0] = 0;
            m_player1.hmp(0);  m_shadow[HMP1] = 0;
            m_ball.hmbl(0);    m_shadow[HMBL] = 0;
            break;

        case GRP0: m_player0.grp(value); break;
        case GRP1: m_player1.grp(value); break;
        case SHUFFLE_P0: m_player0.shufflePatterns(); break;
        case SHUFFLE_P1: m_player1.shufflePatterns(); break;
        case SHUFFLE_BL: m_ball.shuffleStatus(); break;

        case HMP0: m_player0.hmp(value); break;
        case HMP1: m_player1.hmp(value); break;
        case HMBL: m_ball.hmbl(value); break;
        case REFP0: m_player0.refp(value); break;
        case REFP1: m_player1.refp(value); break;
        case ENABL: m_ball.enabl(value); break;
        case ENAM0: m_missile0.enam(value); break;
        case ENAM1: m_missile1.enam(value); break;

        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// Clocking (TIA::cycle and friends)
// ---------------------------------------------------------------------------

void Tia::clock(uint32_t colorClocks) {
    while (colorClocks-- > 0) cycleOne();
}

/**
 * One colour clock: audio, the due delayed writes, HMOVE movement, the
 * HBLANK or visible-line object clocks, collisions (gated by VBLANK, as on
 * real hardware) and the horizontal counter.
 */
void Tia::cycleOne() {
    m_audio.tick(1);
    m_audioAcc += (uint64_t)m_sampleRate;
    const uint64_t hz = clockHz(m_std);
    if (m_audioAcc >= hz) {
        m_audioAcc -= hz;
        pushSample(m_audio.lastSample());
    }

    m_delayQueue.execute([this](uint8_t address, uint8_t value) { delayedWrite(address, value); });

    tickMovement();
    if (m_hblank) tickHblank();
    else tickHframe();

    if (!m_frameManager.vblank()) {
        m_collisionMask |= (uint16_t)(m_player0.collision & m_player1.collision & m_missile0.collision &
                                      m_missile1.collision & m_ball.collision & m_playfield.collision);
    }

    if (++m_hctr >= H_CLOCKS) nextLine();
    ++m_timestamp;
}

// HMOVE sends a movement clock every 4 colour clocks; the counter compares
// against each object's HMxx value (stopping it) up to 15.
void Tia::tickMovement() {
    if (!m_movementInProgress) return;

    if ((m_hctr & 0x03) == 0) {
        const uint8_t movementCounter = (uint8_t)(m_movementClock > 15 ? 0 : m_movementClock);

        m_missile0.movementTick(movementCounter, m_hctr, m_hblank);
        m_missile1.movementTick(movementCounter, m_hctr, m_hblank);
        m_player0.movementTick(movementCounter, m_hblank);
        m_player1.movementTick(movementCounter, m_hblank);
        m_ball.movementTick(movementCounter, m_hblank);

        m_movementInProgress = m_missile0.isMoving || m_missile1.isMoving || m_player0.isMoving ||
                               m_player1.isMoving || m_ball.isMoving;

        ++m_movementClock;
    }
}

// HBLANK lasts 68 clocks, or 76 after an HMOVE (the "comb"); the playfield
// still runs in those last 8.
void Tia::tickHblank() {
    m_missile0.tickClkpInHblank();
    m_missile1.tickClkpInHblank();
    m_player0.tickClkpInHblank();
    m_player1.tickClkpInHblank();
    m_ball.tickClkpInHblank();

    switch (m_hctr) {
        case 0:
            m_extendedHblank = false;
            break;
        case H_BLANK_CLOCKS - 1:
            if (!m_extendedHblank) m_hblank = false;
            break;
        case H_BLANK_CLOCKS + 7:
            if (m_extendedHblank) m_hblank = false;
            break;
        default:
            break;
    }

    if (m_extendedHblank && (int32_t)m_hctr - m_hctrDelta > (int32_t)H_BLANK_CLOCKS - 1)
        m_playfield.tick((uint32_t)((int32_t)m_hctr - (int32_t)H_BLANK_CLOCKS - m_hctrDelta));
}

void Tia::tickHframe() {
    const uint32_t x = (uint32_t)((int32_t)m_hctr - (int32_t)H_BLANK_CLOCKS - m_hctrDelta);

    m_playfield.tick(x);
    m_player0.tick();
    m_player1.tick();
    m_missile0.tick(m_hctr, m_player0);
    m_missile1.tick(m_hctr, m_player1);
    m_ball.tick();

    if (m_frameManager.isRendering()) renderPixel(x);
}

void Tia::renderPixel(uint32_t x) {
    if (x >= H_PIXEL) return;

    uint8_t color = 0;

    if (!m_frameManager.vblank()) {
        switch (m_priority) {
            case Priority::PFP:
                // BL/PF => P0/M0 => P1/M1 => BK
                if (m_playfield.isOn())      color = m_playfield.getColor();
                else if (m_ball.isOn())      color = m_ball.getColor();
                else if (m_player0.isOn())   color = m_player0.getColor();
                else if (m_missile0.isOn())  color = m_missile0.getColor();
                else if (m_player1.isOn())   color = m_player1.getColor();
                else if (m_missile1.isOn())  color = m_missile1.getColor();
                else                         color = m_background.getColor();
                break;

            case Priority::SCORE:
                // PF/P0/M0 => P1/M1 => BL => BK on the left half, P0/M0 => PF/P1/M1 => BL => BK
                // on the right; the same, since PF takes the P0/P1 colours
                if (m_player0.isOn())        color = m_player0.getColor();
                else if (m_missile0.isOn())  color = m_missile0.getColor();
                else if (m_playfield.isOn()) color = m_playfield.getColor();
                else if (m_player1.isOn())   color = m_player1.getColor();
                else if (m_missile1.isOn())  color = m_missile1.getColor();
                else if (m_ball.isOn())      color = m_ball.getColor();
                else                         color = m_background.getColor();
                break;

            case Priority::NORMAL:
                // P0/M0 => P1/M1 => BL/PF => BK
                if (m_player0.isOn())        color = m_player0.getColor();
                else if (m_missile0.isOn())  color = m_missile0.getColor();
                else if (m_player1.isOn())   color = m_player1.getColor();
                else if (m_missile1.isOn())  color = m_missile1.getColor();
                else if (m_playfield.isOn()) color = m_playfield.getColor();
                else if (m_ball.isOn())      color = m_ball.getColor();
                else                         color = m_background.getColor();
                break;
        }
    }

    m_backBuffer[(size_t)m_frameManager.getY() * H_PIXEL + x] = color;
}

void Tia::nextLine() {
    m_hctr = 0;
    m_hblank = true;
    m_hctrDelta = 0;

    if (m_frameManager.nextLine()) onFrameComplete();

    m_missile0.nextLine();
    m_missile1.nextLine();
    m_player0.nextLine();
    m_player1.nextLine();
    m_ball.nextLine();
    m_playfield.nextLine();

    m_halt = false;   // the CPU's RDY is released at the start of the line
}

// RSYNC: restart the line 3 clocks before its end; the rest of the visible
// line is blanked.
void Tia::applyRsync() {
    const uint32_t x = m_hctr > H_BLANK_CLOCKS ? m_hctr - H_BLANK_CLOCKS : 0;

    m_hctrDelta = (int32_t)H_CLOCKS - 3 - m_hctr;
    if (m_frameManager.isRendering() && x < H_PIXEL) {
        uint8_t* row = &m_backBuffer[(size_t)m_frameManager.getY() * H_PIXEL];
        std::fill(row + x, row + H_PIXEL, 0);
    }

    m_hctr = (uint8_t)(H_CLOCKS - 3);
}

void Tia::clearHmoveComb() {
    if (m_frameManager.isRendering() && m_hblank) {
        uint8_t* row = &m_backBuffer[(size_t)m_frameManager.getY() * H_PIXEL];
        std::fill(row, row + 8, COLOR_HBLANK);
    }
}

void Tia::onFrameComplete() {
    // Blank the lines not drawn this frame
    const int32_t missing = m_frameManager.missingScanlines();
    if (missing > 0) {
        const size_t from = (size_t)m_frameManager.getY() * H_PIXEL;
        const size_t to = std::min(m_backBuffer.size(), from + (size_t)missing * H_PIXEL);
        if (from < to) std::fill(m_backBuffer.begin() + (long)from, m_backBuffer.begin() + (long)to, 0);
    }
    m_frontBuffer = m_backBuffer;
}

// ---------------------------------------------------------------------------
// Inputs
// ---------------------------------------------------------------------------

void Tia::setButton(int port, bool pressed) {
    if (port < 0 || port > 1) return;
    m_buttons[port] = pressed;
}

void Tia::setPaddle(int idx, double position) {
    if (idx < 0 || idx > 3) return;
    AnalogReadout::Connection c;
    if (position < 0.0) {
        c.type = AnalogReadout::ConnectionType::DISCONNECTED;
    } else {
        if (position > 1.0) position = 1.0;
        c.type = AnalogReadout::ConnectionType::VCC;
        c.resistance = (uint32_t)std::lround(position * AnalogReadout::MAX_POT_RESISTANCE);
    }
    m_analogConnections[idx] = c;
    m_analogReadouts[idx].update(c, m_timestamp);
}

// ---------------------------------------------------------------------------
// Video output
// ---------------------------------------------------------------------------

uint32_t Tia::paletteColor(uint8_t color) const {
    return 0xFF000000u | m_palette[color >> 1];
}

IVideoOutput::VideoDimensions Tia::getDimensions() const {
    const int h = frameHeight();
    return {FRAME_WIDTH, h, FRAME_WIDTH, h};
}

void Tia::renderFrame(uint32_t* buffer) {
    const int h = frameHeight();
    for (int y = 0; y < h; ++y) {
        const uint8_t* src = &m_frontBuffer[(size_t)y * H_PIXEL];
        uint32_t* dst = buffer + (size_t)y * FRAME_WIDTH;
        for (uint32_t x = 0; x < H_PIXEL; ++x) {
            const uint32_t c = paletteColor(src[x]);
            dst[2 * x] = c;
            dst[2 * x + 1] = c;
        }
    }
}

// ---------------------------------------------------------------------------
// Audio output
// ---------------------------------------------------------------------------

void Tia::setSampleRate(int hz) {
    if (hz <= 0) return;
    m_sampleRate = hz;
    m_audioAcc = 0;
}

void Tia::pushSample(float v) {
    if (m_audioCount >= AUDIO_BUF) return;   // full: drop
    m_audioBuf[m_audioWrite] = v;
    m_audioWrite = (m_audioWrite + 1) % AUDIO_BUF;
    ++m_audioCount;
}

int Tia::pullSamples(float* buffer, int maxSamples) {
    const int n = maxSamples < m_audioCount ? maxSamples : m_audioCount;
    for (int i = 0; i < n; ++i) {
        buffer[i] = m_audioBuf[m_audioRead];
        m_audioRead = (m_audioRead + 1) % AUDIO_BUF;
    }
    m_audioCount -= n;
    return n;
}
