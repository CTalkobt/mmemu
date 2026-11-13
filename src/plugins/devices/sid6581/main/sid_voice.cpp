#include "sid_voice.h"

// Ported from reSIDfp's WaveformGenerator.cpp and EnvelopeGenerator.cpp
// (libsidplayfp, GPL-2.0-or-later; see sid_voice.h). The comments are
// reSIDfp's, shortened.

namespace {

// Cycles until a floating waveform DAC input (no waveform selected) starts to
// fade, and between the bits fading; measured on warm chips via OSC3 (VICE
// bugs #290 and #1128). reSIDfp's 6581R3 and 8580R5 values.
constexpr unsigned FLOATING_OUTPUT_TTL_6581  = 54000;
constexpr unsigned FLOATING_OUTPUT_FADE_6581 = 1400;
constexpr unsigned FLOATING_OUTPUT_TTL_8580  = 800000;
constexpr unsigned FLOATING_OUTPUT_FADE_8580 = 50000;

// Cycles until the test bit starts resetting the shift register to 1s, and
// between the bits.
constexpr unsigned SHIFT_REGISTER_RESET_6581 = 50000;
constexpr unsigned SHIFT_REGISTER_FADE_6581  = 15000;
constexpr unsigned SHIFT_REGISTER_RESET_8580 = 986000;
constexpr unsigned SHIFT_REGISTER_FADE_8580  = 314300;

// The shift register bits that feed the noise output (bit-reversed layout)
constexpr uint32_t SHIFT_MASK = ~((1u << 2) | (1u << 4) | (1u << 8) | (1u << 11) |
                                  (1u << 13) | (1u << 17) | (1u << 20) | (1u << 22));

// Whether a shift's second phase takes the combined output into the register
// (reSIDfp's do_writeback, fitted to testprogs SID/wb_testsuite and friends)
bool doWriteback(uint8_t oldW, uint8_t newW, bool is6581) {
    if (oldW <= 8) return false;            // no combined waveform before
    if (newW < 8) return false;
    if (newW == 8 && oldW != 0xF) return false;
    if (is6581 && ((((oldW & 3) == 1) && ((newW & 3) == 2)) ||
                   (((oldW & 3) == 2) && ((newW & 3) == 1))))
        return false;
    if (oldW == 0xC) return false;
    if (newW == 0xC) return false;
    return true;
}

uint32_t noiseWriteback(uint32_t o) {
    return ((o & (1u << 11)) >> 9) | ((o & (1u << 10)) >> 6) | ((o & (1u << 9)) >> 1) |
           ((o & (1u << 8)) << 3) | ((o & (1u << 7)) << 6) | ((o & (1u << 6)) << 11) |
           ((o & (1u << 5)) << 15) | ((o & (1u << 4)) << 18);
}

}  // namespace

// ---------------------------------------------------------------------------
// Waveform generator
// ---------------------------------------------------------------------------

uint32_t SidWave::wave(uint32_t ix) const {
    switch (m_waveform & 3) {
        case 0: return 0xFFF;
        case 1: return (((ix & 0x800) ? ix ^ 0xFFF : ix) << 1) & 0xFFF;   // triangle
        case 2: return ix;                                                // sawtooth
        default: return ix & (ix << 1) & 0xFFF;                           // saw + triangle
    }
}

uint32_t SidWave::pulldown(uint32_t v) const {
    return CombinedWaveformTable::pulldown((uint16_t)v, (uint8_t)(m_waveform << 4), m_chip);
}

void SidWave::clock() {
    if (m_test) {
        if (m_shiftRegReset != 0 && --m_shiftRegReset == 0) {
            shiftregBitfade();
            m_shiftLatch = m_shiftReg;
            setNoiseOutput();       // the noise output follows the reset
        }
        m_testOrReset = true;
        m_pulseOut = 0xFFF;         // the test bit holds the pulse high
        return;
    }
    const uint32_t old = m_acc;
    m_acc = (m_acc + m_freq) & 0xFFFFFF;
    const uint32_t risen = ~old & m_acc;
    m_msbRising = (risen & 0x800000) != 0;
    // Bit 19 rising starts the shift: phase 1 in two cycles, phase 2 in three
    if (risen & 0x080000) {
        m_shiftPipeline = 2;
    } else if (m_shiftPipeline != 0) {
        switch (--m_shiftPipeline) {
            case 0: shiftPhase2(m_waveform, m_waveform); break;
            case 1: m_testOrReset = false; m_shiftLatch = m_shiftReg; break;
        }
    }
}

uint32_t SidWave::output() {
    if (m_waveform != 0) {
        const uint32_t ix = (m_acc ^ (~m_prev->m_acc & m_ringMsbMask)) >> 12;
        m_waveOut = wave(ix) & (m_noPulse | m_pulseOut) & m_noNoiseOrNoiseOut;
        if (m_waveform & 0x7) m_waveOut = pulldown(m_waveOut);
        // The 8580's OSC3 is a cycle late for triangle and sawtooth
        if ((m_waveform & 3) && !m_is6581) {
            m_osc3 = m_triSawPipeline & (m_noPulse | m_pulseOut) & m_noNoiseOrNoiseOut;
            if (m_waveform & 0x7) m_osc3 = pulldown(m_osc3);
            m_triSawPipeline = wave(ix);
        } else {
            m_osc3 = m_waveOut;
        }
        // 6581: sawtooth's output top bit low pulls the accumulator MSB low
        if (m_is6581 && (m_waveform & 0x2) && !(m_waveOut & 0x800)) {
            m_msbRising = false;
            m_acc &= 0x7FFFFF;
        }
        writeShiftRegister();
    } else if (m_floatingTtl != 0 && --m_floatingTtl == 0) {
        waveBitfade();
    }
    // The pulse comparator's result takes effect the next cycle
    m_pulseOut = ((m_acc >> 12) >= m_pw) ? 0xFFF : 0x000;
    return m_waveOut;
}

void SidWave::synchronize() const {
    // No sync when the source is itself being synced as its MSB rises
    if (m_msbRising && m_next->m_sync && !(m_sync && m_prev->m_msbRising))
        m_next->m_acc = 0;
}

void SidWave::shiftPhase2(uint8_t waveformOld, uint8_t waveformNew) {
    if (doWriteback(waveformOld, waveformNew, m_is6581))
        m_shiftLatch = (m_shiftReg & SHIFT_MASK) | noiseWriteback(m_waveOut);
    // bit0 = (bit22 | test | reset) ^ bit17
    const uint32_t bit22 = ((m_testOrReset ? 1u : 0u) | m_shiftLatch) << 22;
    const uint32_t bit0 = (bit22 ^ (m_shiftLatch << 17)) & (1u << 22);
    m_shiftReg = (m_shiftLatch >> 1) | bit0;
    setNoiseOutput();
}

void SidWave::writeShiftRegister() {
    if (m_waveform <= 0x8) return;
    if (m_shiftPipeline != 1 && !m_test) {
        // The output pulls the register's bits low
        m_shiftReg &= SHIFT_MASK | noiseWriteback(m_waveOut);
        m_noiseOut &= m_waveOut;
    } else {
        // Shift phase 1: the output drives the bits
        m_noiseOut = m_waveOut;
    }
    setNoNoiseOrNoiseOutput();
}

void SidWave::setNoiseOutput() {
    const uint32_t s = m_shiftReg;
    m_noiseOut = ((s & (1u << 2)) << 9) | ((s & (1u << 4)) << 6) | ((s & (1u << 8)) << 1) |
                 ((s & (1u << 11)) >> 3) | ((s & (1u << 13)) >> 6) | ((s & (1u << 17)) >> 11) |
                 ((s & (1u << 20)) >> 15) | ((s & (1u << 22)) >> 18);
    setNoNoiseOrNoiseOutput();
}

void SidWave::writeControl(uint8_t control) {
    const uint8_t waveformPrev = m_waveform;
    const bool testPrev = m_test;
    m_waveform = (control >> 4) & 0x0F;
    m_test = (control & 0x08) != 0;
    m_sync = (control & 0x02) != 0;
    // Ring modulation without sawtooth: the accumulator MSB is replaced
    m_ringMsbMask = ((~control >> 5) & (control >> 2) & 0x1) << 23;

    if (m_waveform != waveformPrev) {
        m_noNoise = (m_waveform & 0x8) ? 0x000 : 0xFFF;
        setNoNoiseOrNoiseOutput();
        m_noPulse = (m_waveform & 0x4) ? 0x000 : 0xFFF;
        if (m_waveform == 0)        // the DAC input floats, then fades
            m_floatingTtl = m_is6581 ? FLOATING_OUTPUT_TTL_6581 : FLOATING_OUTPUT_TTL_8580;
    }
    if (m_test != testPrev) {
        if (m_test) {
            m_acc = 0;
            m_shiftPipeline = 0;
            m_shiftLatch = m_shiftReg;      // shift phase 1, held
            m_shiftRegReset = m_is6581 ? SHIFT_REGISTER_RESET_6581 : SHIFT_REGISTER_RESET_8580;
        } else {
            shiftPhase2(waveformPrev, m_waveform);   // released: phase 2
        }
    }
}

void SidWave::waveBitfade() {
    m_waveOut &= m_waveOut >> 1;
    m_osc3 = m_waveOut;
    if (m_waveOut != 0)
        m_floatingTtl = m_is6581 ? FLOATING_OUTPUT_FADE_6581 : FLOATING_OUTPUT_FADE_8580;
}

void SidWave::shiftregBitfade() {
    m_shiftReg |= m_shiftReg >> 1;
    m_shiftReg |= 0x400000;
    if (m_shiftReg != 0x7FFFFF)
        m_shiftRegReset = m_is6581 ? SHIFT_REGISTER_FADE_6581 : SHIFT_REGISTER_FADE_8580;
}

void SidWave::reset() {
    // The accumulator is not changed by a reset
    m_freq = 0;
    m_pw = 0;
    m_msbRising = false;
    m_waveform = 0;
    m_osc3 = 0;
    m_test = false;
    m_sync = false;
    m_ringMsbMask = 0;
    m_noNoise = 0xFFF;
    m_noPulse = 0xFFF;
    m_pulseOut = 0xFFF;
    m_shiftRegReset = 0;
    m_shiftReg = 0x7FFFFF;
    // Releasing reset clocks the register once, clearing its low bit
    m_testOrReset = true;
    m_shiftLatch = m_shiftReg;
    shiftPhase2(0, 0);
    m_shiftPipeline = 0;
    m_waveOut = 0;
    m_floatingTtl = 0;
}

uint32_t SidWave::lfsr() const {
    uint32_t r = 0;
    for (int i = 0; i < 23; ++i)
        if (m_shiftReg & (1u << i)) r |= 1u << (22 - i);
    return r;
}

// ---------------------------------------------------------------------------
// Envelope generator
// ---------------------------------------------------------------------------

namespace {
// Rate LFSR values that end a period, for the 4-bit A/D/R values (kevtris)
const uint16_t ADSR_TABLE[16] = {
    0x007F, 0x3000, 0x1E00, 0x0660, 0x0182, 0x5573, 0x000E, 0x3805,
    0x2424, 0x2220, 0x090C, 0x0ECD, 0x010E, 0x23F7, 0x5237, 0x64A8,
};
}  // namespace

void SidEnvelope::clock() {
    m_env3 = m_counter;
    if (m_newExpPeriod > 0) { m_expPeriod = m_newExpPeriod; m_newExpPeriod = 0; }
    if (m_statePipeline) stateChange();

    if (m_envPipeline != 0 && --m_envPipeline == 0) {
        if (m_counterEnabled) {
            if (m_state == State::ATTACK) {
                if (++m_counter == 0xFF) { m_nextState = State::DECAY_SUSTAIN; m_statePipeline = 3; }
            } else if (--m_counter == 0x00) {
                m_counterEnabled = false;
            }
            setExponentialCounter();
        }
    } else if (m_expPipeline != 0 && --m_expPipeline == 0) {
        m_expCounter = 0;
        // The counter can flip from $00 to $FF going attack -> release, then
        // keeps counting down
        if ((m_state == State::DECAY_SUSTAIN && m_counter != m_sustain) || m_state == State::RELEASE)
            m_envPipeline = 1;
    } else if (m_resetLfsr) {
        m_lfsr = 0x7FFF;
        m_resetLfsr = false;
        if (m_state == State::ATTACK) {
            // The first attack step also resets the exponential counter; a
            // counter that flipped $FF -> $00 (release, then attack) stays frozen
            m_expCounter = 0;
            m_envPipeline = 2;
        } else if (m_counterEnabled && ++m_expCounter == m_expPeriod) {
            m_expPipeline = m_expPeriod != 1 ? 2 : 1;
        }
    }

    // ADSR delay bug: a rate below the counter's value waits for it to wrap
    if (m_lfsr != m_rate) {
        const unsigned feedback = ((m_lfsr << 14) ^ (m_lfsr << 13)) & 0x4000;
        m_lfsr = (uint16_t)((m_lfsr >> 1) | feedback);
    } else {
        m_resetLfsr = true;
    }
}

// The state switches go through a pipeline (die analysis): attack enables the
// decay rate for a cycle first; release from attack takes a cycle longer.
void SidEnvelope::stateChange() {
    --m_statePipeline;
    switch (m_nextState) {
        case State::ATTACK:
            if (m_statePipeline == 1) {
                m_rate = ADSR_TABLE[m_decay];
            } else if (m_statePipeline == 0) {
                m_state = State::ATTACK;
                m_rate = ADSR_TABLE[m_attack];
                m_counterEnabled = true;
            }
            break;
        case State::DECAY_SUSTAIN:
            if (m_statePipeline == 0) { m_state = State::DECAY_SUSTAIN; m_rate = ADSR_TABLE[m_decay]; }
            break;
        case State::RELEASE:
            if ((m_state == State::ATTACK && m_statePipeline == 0) ||
                (m_state == State::DECAY_SUSTAIN && m_statePipeline == 1)) {
                m_state = State::RELEASE;
                m_rate = ADSR_TABLE[m_release];
            }
            break;
    }
}

void SidEnvelope::setExponentialCounter() {
    switch (m_counter) {
        case 0xFF: case 0x00: m_newExpPeriod = 1; break;
        case 0x5D: m_newExpPeriod = 2; break;
        case 0x36: m_newExpPeriod = 4; break;
        case 0x1A: m_newExpPeriod = 8; break;
        case 0x0E: m_newExpPeriod = 16; break;
        case 0x06: m_newExpPeriod = 30; break;
        default: break;
    }
}

void SidEnvelope::reset() {
    // The counter is not changed by a reset
    m_envPipeline = 0;
    m_statePipeline = 0;
    m_attack = m_decay = m_sustain = m_release = 0;
    m_gate = false;
    m_resetLfsr = true;
    m_expCounter = 0;
    m_expPeriod = 1;
    m_newExpPeriod = 0;
    m_state = State::RELEASE;
    m_counterEnabled = true;
    m_rate = ADSR_TABLE[m_release];
}

void SidEnvelope::writeControl(uint8_t control) {
    const bool gateNext = (control & 0x01) != 0;
    if (gateNext == m_gate) return;
    m_gate = gateNext;
    // The rate counter is never reset: a delay before the first step
    if (gateNext) {
        m_nextState = State::ATTACK;
        m_statePipeline = 2;
        if (m_resetLfsr || m_expPipeline == 2)
            m_envPipeline = (m_expPeriod == 1 || m_expPipeline == 2) ? 2 : 4;
        else if (m_expPipeline == 1)
            m_statePipeline = 3;
    } else {
        m_nextState = State::RELEASE;
        m_statePipeline = m_envPipeline > 0 ? 3 : 2;
    }
}

void SidEnvelope::writeAttackDecay(uint8_t v) {
    m_attack = (v >> 4) & 0x0F;
    m_decay = v & 0x0F;
    if (m_state == State::ATTACK) m_rate = ADSR_TABLE[m_attack];
    else if (m_state == State::DECAY_SUSTAIN) m_rate = ADSR_TABLE[m_decay];
}

void SidEnvelope::writeSustainRelease(uint8_t v) {
    // Both nibbles of the counter are compared with the sustain value
    m_sustain = (uint8_t)((v & 0xF0) | ((v >> 4) & 0x0F));
    m_release = v & 0x0F;
    if (m_state == State::RELEASE) m_rate = ADSR_TABLE[m_release];
}
