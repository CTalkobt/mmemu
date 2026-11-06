// Atari 2600 TIA building blocks, after Stella (src/emucore/tia/: Player,
// Missile, Ball, Playfield, LatchedInput, AnalogReadout, AudioChannel, Audio,
// DrawCounterDecodes, DelayQueue, frame-manager/FrameManager),
// GPL-2.0-or-later, Christian Speckner (DirtyHairy), Stephen Anthony and the
// Stella team. See tia_objects.h for what is left out.

#include "tia_objects.h"

#include <cmath>

namespace tiacore {

// ---------------------------------------------------------------------------
// DrawCounterDecodes
// ---------------------------------------------------------------------------

DrawCounterDecodes::DrawCounterDecodes() {
    uint8_t* tables[] = {m_decodes0, m_decodes1, m_decodes2, m_decodes3, m_decodes4, m_decodes6};
    for (uint8_t* t : tables) {
        for (int i = 0; i < 160; ++i) t[i] = 0;
        t[156] = 1;                              // the main copy, for every mode
    }
    m_decodes1[12] = 2;                          // two copies close (+16)
    m_decodes2[28] = 2;                          // two copies medium (+32)
    m_decodes3[12] = 2; m_decodes3[28] = 3;      // three copies close (+16, +32)
    m_decodes4[60] = 2;                          // two copies wide (+64)
    m_decodes6[28] = 2; m_decodes6[60] = 3;      // three copies medium (+32, +64)

    const uint8_t* byMode[8] = {m_decodes0, m_decodes1, m_decodes2, m_decodes3,
                                m_decodes4, m_decodes0, m_decodes6, m_decodes0};
    for (int i = 0; i < 8; ++i) m_playerDecodes[i] = m_missileDecodes[i] = byMode[i];
}

const DrawCounterDecodes& DrawCounterDecodes::get() {
    static const DrawCounterDecodes instance;
    return instance;
}

// ---------------------------------------------------------------------------
// DelayQueue
// ---------------------------------------------------------------------------

void DelayQueue::reset() {
    for (auto& m : m_members) m.size = 0;
    m_index = 0;
    m_pendingCount = 0;
}

bool DelayQueue::push(uint8_t address, uint8_t value, uint8_t delay) {
    // Stella throws here; with the fixed delays below 16 neither can happen
    if (delay >= LENGTH) return false;
    Member& m = m_members[(m_index + delay) % LENGTH];
    if (m.size == CAPACITY) return false;
    m.entries[m.size].address = address;
    m.entries[m.size].value = value;
    ++m.size;
    ++m_pendingCount;
    return true;
}

// ---------------------------------------------------------------------------
// Player
// ---------------------------------------------------------------------------

void Player::reset() {
    m_decodesOffset = 0;
    m_decodes = DrawCounterDecodes::get().playerDecodes()[m_decodesOffset];
    m_hmmClocks = 0;
    m_counter = 0;
    isMoving = false;
    m_isRendering = false;
    m_renderCounter = 0;
    m_copy = 1;
    m_patternOld = 0;
    m_patternNew = 0;
    m_isReflected = false;
    m_isDelaying = false;
    m_color = 0;
    collision = m_collisionMaskDisabled;
    m_sampleCounter = 0;
    m_dividerPending = 0;
    m_dividerChangeCounter = -1;
    m_pattern = 0;
    setDivider(1);
}

void Player::grp(uint8_t pattern) {
    const uint8_t oldPatternNew = m_patternNew;
    m_patternNew = pattern;
    if (!m_isDelaying && m_patternNew != oldPatternNew) updatePattern();
}

void Player::nusiz(uint8_t value, bool hblank) {
    m_decodesOffset = value & 0x07;

    switch (m_decodesOffset) {
        case 5:  m_dividerPending = 2; break;
        case 7:  m_dividerPending = 4; break;
        default: m_dividerPending = 1; break;
    }

    const uint8_t* oldDecodes = m_decodes;
    m_decodes = DrawCounterDecodes::get().playerDecodes()[m_decodesOffset];

    // Changing NUSIZ can trigger a decode in the same cycle (Stella issue #1012)
    const uint8_t prevCounter = (uint8_t)((m_counter + H_PIXEL - 1) % H_PIXEL);
    if (!m_isRendering && m_decodes[prevCounter]) {
        m_isRendering = true;
        m_sampleCounter = 0;
        m_renderCounter = RENDER_COUNTER_OFFSET;
        m_copy = (int8_t)m_decodes[prevCounter];
    }

    if (m_decodes != oldDecodes && m_isRendering &&
        (m_renderCounter - RENDER_COUNTER_OFFSET) < 2 &&
        !m_decodes[(m_counter - m_renderCounter + RENDER_COUNTER_OFFSET + H_PIXEL - 1) % H_PIXEL]) {
        m_isRendering = false;
    }

    if (m_dividerPending == m_divider) return;

    // Stella's effective description of NUSIZ changes during decode and rendering
    if (m_isRendering) {
        const int8_t delta = (int8_t)(m_renderCounter - RENDER_COUNTER_OFFSET);

        switch ((m_divider << 4) | m_dividerPending) {
            case 0x12:
            case 0x14:
                if (hblank) {
                    if (delta < 4) setDivider(m_dividerPending);
                    else m_dividerChangeCounter = (delta < 5 ? 1 : 0);
                } else {
                    if (delta < 3) setDivider(m_dividerPending);
                    else m_dividerChangeCounter = 1;
                }
                break;

            case 0x21:
            case 0x41:
                if (delta < (hblank ? 4 : 3)) {
                    setDivider(m_dividerPending);
                } else if (delta < (hblank ? 6 : 5)) {
                    setDivider(m_dividerPending);
                    --m_renderCounter;
                } else {
                    m_dividerChangeCounter = (hblank ? 0 : 1);
                }
                break;

            case 0x42:
            case 0x24:
                if (m_renderCounter < 1 || (hblank && (m_renderCounter % m_divider == 1)))
                    setDivider(m_dividerPending);
                else
                    m_dividerChangeCounter = (int8_t)(m_divider - (m_renderCounter - 1) % m_divider);
                break;

            default:
                setDivider(m_dividerPending);
                break;
        }
    } else {
        setDivider(m_dividerPending);
    }
}

void Player::resp(uint8_t counter) {
    m_counter = counter;

    // RESP during the draw counter decode (Andrew Towers' notes)
    if (m_isRendering && (m_renderCounter - RENDER_COUNTER_OFFSET) < 4)
        m_renderCounter = (int8_t)(RENDER_COUNTER_OFFSET + (counter - 157));
}

void Player::refp(uint8_t value) {
    const bool old = m_isReflected;
    m_isReflected = (value & 0x08) != 0;
    if (old != m_isReflected) updatePattern();
}

void Player::vdelp(uint8_t value) {
    const bool old = m_isDelaying;
    m_isDelaying = (value & 0x01) != 0;
    if (old != m_isDelaying) updatePattern();
}

void Player::shufflePatterns() {
    const uint8_t oldPatternOld = m_patternOld;
    m_patternOld = m_patternNew;
    if (m_isDelaying && m_patternOld != oldPatternOld) updatePattern();
}

uint8_t Player::getRespClock() const {
    switch (m_divider) {
        case 2:  return (uint8_t)((m_counter + H_PIXEL - 8) % H_PIXEL);
        case 4:  return (uint8_t)((m_counter + H_PIXEL - 12) % H_PIXEL);
        default: return (uint8_t)((m_counter + H_PIXEL - 5) % H_PIXEL);
    }
}

void Player::updatePattern() {
    m_pattern = m_isDelaying ? m_patternOld : m_patternNew;

    // Not reflected: bit-reverse, so tick() always samples from bit 0
    if (!m_isReflected) {
        const uint8_t p = m_pattern;
        m_pattern = (uint8_t)(((p & 0x01) << 7) | ((p & 0x02) << 5) | ((p & 0x04) << 3) | ((p & 0x08) << 1) |
                              ((p & 0x10) >> 1) | ((p & 0x20) >> 3) | ((p & 0x40) >> 5) | ((p & 0x80) >> 7));
    }

    if (m_isRendering && m_renderCounter >= m_renderCounterTripPoint)
        collision = (m_pattern & (1u << m_sampleCounter)) ? cmask::ALL : m_collisionMaskDisabled;
}

// ---------------------------------------------------------------------------
// Missile
// ---------------------------------------------------------------------------

void Missile::reset() {
    m_decodesOffset = 0;
    m_decodes = DrawCounterDecodes::get().missileDecodes()[m_decodesOffset];
    m_isEnabled = false;
    m_enam = false;
    m_resmp = 0;
    m_hmmClocks = 0;
    m_counter = 0;
    isMoving = false;
    m_width = 1;
    m_effectiveWidth = 1;
    m_isRendering = false;
    m_isVisible = false;
    m_renderCounter = 0;
    m_copy = 1;
    m_color = 0;
    collision = m_collisionMaskDisabled;
}

void Missile::enam(uint8_t value) {
    const bool old = m_enam;
    m_enam = (value & 0x02) != 0;
    if (old != m_enam) updateEnabled();
}

void Missile::resm(uint8_t counter, bool hblank) {
    m_counter = counter;

    if (m_isRendering) {
        if (m_renderCounter < 0) {
            m_renderCounter = (int8_t)(RENDER_COUNTER_OFFSET + (counter - 157));
        } else {
            // The following is an effective description of the behaviour of the
            // missile when RESMx is strobed while it is drawing (Stella)
            switch (m_width) {
                case 8:
                    m_renderCounter = (int8_t)((counter - 157) + ((m_renderCounter >= 4) ? 4 : 0));
                    break;
                case 4:
                    m_renderCounter = (int8_t)(counter - 157);
                    break;
                case 2:
                    if (hblank) m_isRendering = m_renderCounter > 1;
                    else if (m_renderCounter == 0) ++m_renderCounter;
                    break;
                default:
                    if (hblank) m_isRendering = m_renderCounter > 0;
                    break;
            }
        }
    }
}

void Missile::resmp(uint8_t value) {
    const uint8_t r = value & 0x02;
    if (r == m_resmp) return;
    m_resmp = r;
    updateEnabled();
}

void Missile::nusiz(uint8_t value) {
    static constexpr uint8_t WIDTHS[4] = {1, 2, 4, 8};
    m_decodesOffset = value & 0x07;
    m_width = WIDTHS[(value & 0x30) >> 4];
    m_decodes = DrawCounterDecodes::get().missileDecodes()[m_decodesOffset];

    if (m_isRendering && m_renderCounter >= (int)m_width) m_isRendering = false;
}

// ---------------------------------------------------------------------------
// Ball
// ---------------------------------------------------------------------------

void Ball::reset() {
    m_color = 0;
    collision = m_collisionMaskDisabled;
    m_isEnabledOld = false;
    m_isEnabledNew = false;
    m_isEnabled = false;
    m_isDelaying = false;
    m_signalActive = false;
    m_hmmClocks = 0;
    m_counter = 0;
    isMoving = false;
    m_effectiveWidth = 1;
    m_lastMovementTick = 0;
    m_width = 1;
    m_isRendering = false;
    m_renderCounter = 0;
}

void Ball::enabl(uint8_t value) {
    const bool old = m_isEnabledNew;
    m_isEnabledNew = (value & 0x02) != 0;
    if (m_isEnabledNew != old && !m_isDelaying) updateEnabled();
}

void Ball::resbl(uint8_t counter) {
    m_counter = counter;
    m_isRendering = true;
    m_renderCounter = (int8_t)(RENDER_COUNTER_OFFSET + (counter - 157));
}

void Ball::ctrlpf(uint8_t value) {
    static constexpr uint8_t WIDTHS[4] = {1, 2, 4, 8};
    m_width = WIDTHS[(value & 0x30) >> 4];
}

void Ball::vdelbl(uint8_t value) {
    const bool old = m_isDelaying;
    m_isDelaying = (value & 0x01) != 0;
    if (old != m_isDelaying) updateEnabled();
}

void Ball::shuffleStatus() {
    const bool old = m_isEnabledOld;
    m_isEnabledOld = m_isEnabledNew;
    if (m_isEnabledOld != old && m_isDelaying) updateEnabled();
}

// ---------------------------------------------------------------------------
// Playfield
// ---------------------------------------------------------------------------

void Playfield::reset() {
    m_pattern = 0;
    m_reflected = false;
    m_refp = false;
    m_pf0 = m_pf1 = m_pf2 = 0;
    m_x = 0;
    m_objectColor = 0;
    m_colorLeft = m_colorRight = 0;
    m_colorP0 = m_colorP1 = 0;
    m_scoreMode = false;
    collision = cmask::NONE;
}

// The pattern holds the 20 playfield bits in display order: PF0 bits 4-7,
// PF1 bits 7-0, PF2 bits 0-7.
void Playfield::pf0(uint8_t value) {
    if (m_pf0 == value >> 4) return;
    m_pattern = (m_pattern & 0x000FFFF0u) | (uint32_t)(value >> 4);
    m_pf0 = value >> 4;
}

void Playfield::pf1(uint8_t value) {
    if (m_pf1 == value) return;
    m_pattern = (m_pattern & 0x000FF00Fu)
        | ((value & 0x80u) >> 3)
        | ((value & 0x40u) >> 1)
        | ((value & 0x20u) << 1)
        | ((value & 0x10u) << 3)
        | ((value & 0x08u) << 5)
        | ((value & 0x04u) << 7)
        | ((value & 0x02u) << 9)
        | ((value & 0x01u) << 11);
    m_pf1 = value;
}

void Playfield::pf2(uint8_t value) {
    if (m_pf2 == value) return;
    m_pattern = (m_pattern & 0x00000FFFu) | ((uint32_t)value << 12);
    m_pf2 = value;
}

void Playfield::ctrlpf(uint8_t value) {
    m_reflected = (value & 0x01) != 0;
    m_scoreMode = (value & 0x06) == 0x02;
    applyColors();
}

void Playfield::applyColors() {
    if (m_scoreMode) {
        m_colorLeft = m_colorP0;
        m_colorRight = m_colorP1;
    } else {
        m_colorLeft = m_colorRight = m_objectColor;
    }
}

// ---------------------------------------------------------------------------
// LatchedInput
// ---------------------------------------------------------------------------

void LatchedInput::vblank(uint8_t value) {
    if (value & 0x40) {
        m_modeLatched = true;
    } else {
        m_modeLatched = false;
        m_latchedValue = 0x80;
    }
}

uint8_t LatchedInput::inpt(bool pinState) {
    uint8_t value = pinState ? 0 : 0x80;
    if (m_modeLatched) {
        m_latchedValue &= value;
        value = m_latchedValue;
    }
    return value;
}

uint8_t LatchedInput::peek(bool pinState) const {
    const uint8_t value = pinState ? 0 : 0x80;
    return m_modeLatched ? (uint8_t)(m_latchedValue & value) : value;
}

// ---------------------------------------------------------------------------
// AnalogReadout
// ---------------------------------------------------------------------------

void AnalogReadout::reset(uint64_t timestamp, bool pal) {
    m_u = 0;
    m_isDumped = false;
    m_connection = Connection{};
    m_timestamp = timestamp;
    setConsoleTiming(pal);
}

void AnalogReadout::setConsoleTiming(bool pal) {
    m_clockFreq = pal ? 50.0 * 228 * 312 : 60.0 * 228 * 262;
    m_uThresh = U_SUPP * (1. - std::exp(-TRIPPOINT_LINES * 228 / m_clockFreq / (MAX_POT_RESISTANCE + R0) / C));
}

void AnalogReadout::vblank(uint8_t value, uint64_t timestamp) {
    updateCharge(timestamp);
    if (value & 0x80) m_isDumped = true;
    else m_isDumped = false;
    m_timestamp = timestamp;
}

double AnalogReadout::chargeAt(uint64_t timestamp) const {
    const double dt = (double)(timestamp - m_timestamp);
    if (m_isDumped) return m_u * std::exp(-dt / R_DUMP / C / m_clockFreq);
    switch (m_connection.type) {
        case ConnectionType::VCC:
            return U_SUPP * (1 - (1 - m_u / U_SUPP) *
                             std::exp(-dt / (m_connection.resistance + R0) / C / m_clockFreq));
        case ConnectionType::GROUND:
            return m_u * std::exp(-dt / (m_connection.resistance + R0) / C / m_clockFreq);
        default:
            return m_u;
    }
}

void AnalogReadout::updateCharge(uint64_t timestamp) {
    m_u = chargeAt(timestamp);
    m_timestamp = timestamp;
}

uint8_t AnalogReadout::inpt(uint64_t timestamp) {
    updateCharge(timestamp);
    return (!m_isDumped && m_u > m_uThresh) ? 0x80 : 0;
}

uint8_t AnalogReadout::peek(uint64_t timestamp) const {
    return (!m_isDumped && chargeAt(timestamp) > m_uThresh) ? 0x80 : 0;
}

void AnalogReadout::update(Connection connection, uint64_t timestamp) {
    if (connection != m_connection) {
        updateCharge(timestamp);
        m_connection = connection;
    }
}

// ---------------------------------------------------------------------------
// AudioChannel
// ---------------------------------------------------------------------------

void AudioChannel::reset() {
    m_audc = m_audf = m_audv = 0;
    m_clockEnable = m_noiseFeedback = m_noiseCounterBit4 = m_pulseCounterHold = false;
    m_divCounter = m_pulseCounter = m_noiseCounter = 0;
}

void AudioChannel::phase0() {
    if (m_clockEnable) {
        m_noiseCounterBit4 = m_noiseCounter & 0x01;

        switch (m_audc & 0x03) {
            case 0x00:
            case 0x01:
                m_pulseCounterHold = false;
                break;
            case 0x02:
                m_pulseCounterHold = (m_noiseCounter & 0x1e) != 0x02;
                break;
            case 0x03:
                m_pulseCounterHold = !m_noiseCounterBit4;
                break;
        }

        if ((m_audc & 0x03) == 0x00) {
            m_noiseFeedback =
                ((m_pulseCounter ^ m_noiseCounter) & 0x01) ||
                !(m_noiseCounter || (m_pulseCounter != 0x0a)) ||
                !(m_audc & 0x0c);
        } else {
            m_noiseFeedback =
                (((m_noiseCounter & 0x04) ? 1u : 0u) ^ (m_noiseCounter & 0x01u)) ||
                m_noiseCounter == 0;
        }
    }

    m_clockEnable = m_divCounter == m_audf;

    if (m_divCounter == m_audf || m_divCounter == 0x1f) m_divCounter = 0;
    else ++m_divCounter;
}

void AudioChannel::phase1() {
    if (!m_clockEnable) return;

    bool pulseFeedback = false;
    switch (m_audc >> 2) {
        case 0x00:
            pulseFeedback =
                (((m_pulseCounter & 0x02) ? 1u : 0u) ^ (m_pulseCounter & 0x01u)) &&
                (m_pulseCounter != 0x0a) &&
                (m_audc & 0x03);
            break;
        case 0x01:
            pulseFeedback = !(m_pulseCounter & 0x08);
            break;
        case 0x02:
            pulseFeedback = !m_noiseCounterBit4;
            break;
        case 0x03:
            pulseFeedback = !((m_pulseCounter & 0x02) || !(m_pulseCounter & 0x0e));
            break;
    }

    m_noiseCounter >>= 1;
    if (m_noiseFeedback) m_noiseCounter |= 0x10;

    if (!m_pulseCounterHold) {
        m_pulseCounter = (uint8_t)(~(m_pulseCounter >> 1) & 0x07);
        if (pulseFeedback) m_pulseCounter |= 0x08;
    }
}

// ---------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------

namespace {
constexpr double R_MAX = 30.;
constexpr double R = 1.;

int16_t mixingTableEntry(uint8_t v, uint8_t vMax) {
    return (int16_t)std::floor(0x7fff * (double)v / (double)vMax *
                               (R_MAX + R * (double)vMax) / (R_MAX + R * (double)v));
}
}  // namespace

Audio::Audio() {
    for (uint8_t i = 0; i <= 0x1e; ++i) m_mixingTableSum[i] = mixingTableEntry(i, 0x1e);
    reset();
}

void Audio::reset() {
    m_sumChannel0 = m_sumChannel1 = m_sumCt = 0;
    m_counter = 0;
    m_channel0.reset();
    m_channel1.reset();
    m_lastSample = 0.0f;
    m_sampleCount = 0;
}

void Audio::tick(uint32_t colorClocks) {
    while (colorClocks > 0) {
        enum class Event : uint8_t { PHASE0, PHASE1, LINE_WRAP };
        uint32_t toEvent;
        Event event;

        if (m_counter < 10)       { toEvent = 10 - m_counter;  event = Event::PHASE0; }
        else if (m_counter < 38)  { toEvent = 38 - m_counter;  event = Event::PHASE1; }
        else if (m_counter < 82)  { toEvent = 82 - m_counter;  event = Event::PHASE0; }
        else if (m_counter < 150) { toEvent = 150 - m_counter; event = Event::PHASE1; }
        else                      { toEvent = 228 - m_counter; event = Event::LINE_WRAP; }

        const uint32_t chunk = colorClocks < toEvent ? colorClocks : toEvent;

        m_sumChannel0 += (uint32_t)m_channel0.actualVolume() * chunk;
        m_sumChannel1 += (uint32_t)m_channel1.actualVolume() * chunk;
        m_sumCt += chunk;
        m_counter = (uint8_t)(m_counter + chunk);
        colorClocks -= chunk;

        if (chunk == toEvent) {
            switch (event) {
                case Event::PHASE0:
                    m_channel0.phase0();
                    m_channel1.phase0();
                    break;
                case Event::PHASE1:
                    m_channel0.phase1();
                    m_channel1.phase1();
                    createSample();
                    break;
                case Event::LINE_WRAP:
                    m_counter = 0;
                    break;
            }
        }
    }
}

void Audio::createSample() {
    const uint8_t sample0 = (uint8_t)(m_sumChannel0 / m_sumCt);
    const uint8_t sample1 = (uint8_t)(m_sumChannel1 / m_sumCt);
    m_sumChannel0 = m_sumChannel1 = m_sumCt = 0;
    m_lastSample = (float)m_mixingTableSum[sample0 + sample1] / 32768.0f;
    ++m_sampleCount;
}

// ---------------------------------------------------------------------------
// FrameManager
// ---------------------------------------------------------------------------

void FrameManager::setPal(bool pal) {
    m_frameLines = pal ? FRAME_SIZE_PAL : FRAME_SIZE_NTSC;
    m_height = pal ? BASE_HEIGHT_PAL : BASE_HEIGHT_NTSC;
    m_yStart = pal ? YSTART_PAL : YSTART_NTSC;
}

void FrameManager::reset() {
    m_vsync = m_vblank = false;
    m_currentFrameTotalLines = m_currentFrameFinalLines = 0;
    m_totalFrames = 0;
    m_state = State::WAIT_FOR_VSYNC_START;
    m_lineInState = 0;
    m_vsyncLineCount = 0;
    m_y = m_lastY = 0;
    m_vsyncPending = false;
    m_vsyncPendingLines = 0;
}

bool FrameManager::setState(State s) {
    if (m_state == s) return false;
    m_state = s;
    m_lineInState = 0;

    bool complete = false;
    switch (m_state) {
        case State::WAIT_FOR_FRAME_START:
            // notifyFrameComplete
            m_currentFrameFinalLines = m_currentFrameTotalLines;
            m_currentFrameTotalLines = 0;
            ++m_totalFrames;
            complete = true;
            m_vsyncLineCount = 0;
            break;
        case State::FRAME:
            m_vsyncLineCount = 0;
            m_y = 0;
            break;
        default:
            break;
    }
    return complete;
}

bool FrameManager::nextLine() {
    ++m_currentFrameTotalLines;

    const State previousState = m_state;
    bool complete = false;
    ++m_lineInState;

    // A VSYNC pulse counts only once it has lasted two full lines
    if (m_vsyncPending && ++m_vsyncPendingLines >= 2) {
        m_vsyncPending = false;
        complete |= setState(State::WAIT_FOR_VSYNC_END);
    }

    switch (m_state) {
        case State::WAIT_FOR_VSYNC_START:
            // Without VBLANK, bail out after too many lines
            if (((m_currentFrameTotalLines > m_frameLines - 3) || m_totalFrames == 0) &&
                (m_vblank || m_currentFrameTotalLines > FRAME_SIZE_PAL * 2))
                ++m_vsyncLineCount;
            if (m_vsyncLineCount > MAX_LINES_VSYNC) complete |= setState(State::WAIT_FOR_FRAME_START);
            break;

        case State::WAIT_FOR_VSYNC_END:
            if (m_vblank || m_currentFrameTotalLines > FRAME_SIZE_PAL * 2) ++m_vsyncLineCount;
            if (m_vsyncLineCount > MAX_LINES_VSYNC) complete |= setState(State::WAIT_FOR_FRAME_START);
            break;

        case State::WAIT_FOR_FRAME_START:
            if (m_lineInState >= m_yStart) complete |= setState(State::FRAME);
            break;

        case State::FRAME:
            if (m_lineInState >= m_height) {
                m_lastY = m_yStart + m_y;
                complete |= setState(State::WAIT_FOR_VSYNC_START);
            }
            break;
    }

    if (m_state == State::FRAME && previousState == State::FRAME) ++m_y;
    return complete;
}

bool FrameManager::setVsync(bool vsync) {
    if (vsync == m_vsync) return false;
    m_vsync = vsync;

    if (m_vsync) {
        // Rising edge: promoted to waitForVsyncEnd by nextLine() after 2 lines
        if (m_state == State::WAIT_FOR_VSYNC_END || m_vsyncPending) return false;
        m_vsyncPending = true;
        m_vsyncPendingLines = 0;
        return false;
    }

    // Falling edge
    m_vsyncPending = false;
    if (m_state != State::WAIT_FOR_VSYNC_END) return false;   // pulse never reached 2 lines
    return setState(State::WAIT_FOR_FRAME_START);
}

int32_t FrameManager::missingScanlines() const {
    if (m_lastY == m_yStart + m_y) return 0;
    return (int32_t)m_height - (int32_t)m_y;
}

}  // namespace tiacore
