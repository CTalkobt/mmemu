// The AY-3-8912, after Fuse sound/ay_engine.c and peripherals/sound/ay.c
// (Russell Marks, Matan Ziv-Av, Philip Kendall, Fredrick Meunier, Patrik
// Rak; GPL-2.0-or-later).

#include "spectrum_ay.h"

namespace spectrum {

namespace {
constexpr int ENV_CONT = 8, ENV_ATTACK = 4, ENV_ALT = 2, ENV_HOLD = 1;
constexpr int CLOCK_DIVISOR = 16;
constexpr unsigned AMPL_TONE = 24 * 256;        // Fuse AMPL_AY_TONE
// Unused register bits are zero (ay.c mask[])
const uint8_t MASK[16] = {0xFF, 0x0F, 0xFF, 0x0F, 0xFF, 0x0F, 0x1F, 0xFF,
                          0x1F, 0x1F, 0x1F, 0xFF, 0xFF, 0x0F, 0xFF, 0xFF};
}  // namespace

SpectrumAy::SpectrumAy() {
    // Matthew Westcott's measurements (comp.sys.sinclair, December 2001),
    // with Russell Marks's adjustment for the AY in a Spectrum
    static const unsigned MEASURED[16] = {0x0000, 0x0385, 0x053D, 0x0770, 0x0AD7, 0x0FD5, 0x15B0, 0x230C,
                                          0x2B4C, 0x43C1, 0x5A4B, 0x732F, 0x9204, 0xAFF1, 0xD921, 0xFFFF};
    for (int i = 0; i < 16; ++i) m_levels[i] = (MEASURED[i] * AMPL_TONE + 0x8000) / 0xFFFF;
    reset();
}

void SpectrumAy::reset() {
    for (auto& r : m_regs) r = 0;
    m_current = 0;
    m_noiseTick = m_noisePeriod = 0;
    m_envInternalTick = m_envTick = m_envPeriod = 0;
    m_toneCycles = 0;
    for (int i = 0; i < 3; ++i) { m_toneTick[i] = m_toneHigh[i] = 0; m_tonePeriod[i] = 1; }
    m_rng = 1;
    m_noiseToggle = 0;
    m_envFirst = 1;
    m_envReverse = 0;
    m_envCounter = 15;
}

void SpectrumAy::write(uint8_t v) {
    m_regs[m_current] = v & MASK[m_current];
    apply(m_current);
}

// ay_registerport_read: R14 is the I/O port (input $BF, ANDed with the
// register in output mode); R15 reads $FF in input mode (the 8912 has no port B)
uint8_t SpectrumAy::read() const {
    const uint8_t input = 0xBF;
    if (m_current == 14) return (m_regs[7] & 0x40) ? (uint8_t)(input & m_regs[14]) : input;
    if (m_current == 15 && !(m_regs[7] & 0x80)) return 0xFF;
    return m_regs[m_current] & MASK[m_current];
}

void SpectrumAy::apply(int reg) {
    if (reg <= 5) {
        const int ch = reg >> 1;
        m_tonePeriod[ch] = m_regs[reg & ~1] | (m_regs[reg | 1] & 15) << 8;
        if (!m_tonePeriod[ch]) m_tonePeriod[ch] = 1;
        if (m_toneTick[ch] >= m_tonePeriod[ch] * 2) m_toneTick[ch] %= m_tonePeriod[ch] * 2;
    } else if (reg == 6) {
        m_noiseTick = 0;
        m_noisePeriod = m_regs[6] & 31;
    } else if (reg == 11 || reg == 12) {
        m_envPeriod = m_regs[11] | (m_regs[12] << 8);
    } else if (reg == 13) {
        m_envInternalTick = m_envTick = 0;
        m_envFirst = 1;
        m_envReverse = 0;
        m_envCounter = (m_regs[13] & ENV_ATTACK) ? 0 : 15;
    }
}

void SpectrumAy::finishEnvelopeCycle(int shape) {
    if (!(shape & ENV_CONT)) m_envCounter = 0;
    else if (shape & ENV_HOLD) { if (m_envFirst && (shape & ENV_ALT)) m_envCounter = m_envCounter ? 0 : 15; }
    else if (shape & ENV_ALT) m_envReverse = !m_envReverse;
    else m_envCounter = (shape & ENV_ATTACK) ? 0 : 15;
    m_envFirst = 0;
}

void SpectrumAy::advanceEnvelope(int shape) {
    if (m_envFirst || ((shape & ENV_CONT) && !(shape & ENV_HOLD))) {
        m_envCounter += m_envReverse ? ((shape & ENV_ATTACK) ? -1 : 1) : ((shape & ENV_ATTACK) ? 1 : -1);
        if (m_envCounter < 0) m_envCounter = 0;
        if (m_envCounter > 15) m_envCounter = 15;
    }
    if (++m_envInternalTick >= 16) {
        m_envInternalTick -= 16;
        finishEnvelopeCycle(shape);
    }
}

// ay_engine_render's loop body: 16 AY clocks
float SpectrumAy::step() {
    const int shape = m_regs[13];
    const unsigned envLevel = m_levels[m_envCounter];
    // The envelope (and the noise's clock)
    m_envTick++;
    while (m_envTick >= m_envPeriod) {
        m_envTick -= m_envPeriod;
        advanceEnvelope(shape);
        if (!m_envPeriod) break;
    }
    m_toneCycles += CLOCK_DIVISOR;
    const unsigned toneCount = m_toneCycles >> 3;
    m_toneCycles &= 7;
    unsigned sum = 0;
    const int mixer = m_regs[7];
    for (int ch = 0; ch < 3; ++ch) {
        const int vol = m_regs[8 + ch];
        unsigned out = (vol & 16) ? envLevel : m_levels[vol & 15];
        if (!(mixer & (1 << ch))) {
            m_toneTick[ch] += toneCount;
            if (m_toneTick[ch] >= m_tonePeriod[ch]) {
                m_toneTick[ch] -= m_tonePeriod[ch];
                m_toneHigh[ch] = !m_toneHigh[ch];
            }
            if (!m_toneHigh[ch]) out = 0;
        }
        if (!(mixer & (0x08 << ch)) && m_noiseToggle) out = 0;
        sum += out;
    }
    // The noise: a 17-bit LFSR (taps 14 and 17)
    m_noiseTick += 1;
    while (m_noiseTick >= m_noisePeriod) {
        m_noiseTick -= m_noisePeriod;
        m_noiseToggle ^= (m_rng ^ (m_rng >> 1)) & 1;
        m_rng = (m_rng >> 1) ^ (0x12000 & -(m_rng & 1));
        if (!m_noisePeriod) break;
    }
    return (float)sum / (float)(50 * 256) * 0.5f;      // on the beeper's scale (older Fuse: AMPL_BEEPER 50 * 256)
}

}  // namespace spectrum
