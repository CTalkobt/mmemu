#pragma once

#include <cstdint>

namespace spectrum {

/**
 * The AY-3-8912 of the Spectrum 128K and +2, after Fuse
 * (sound/ay_engine.c, peripherals/sound/ay.c; GPL-2.0-or-later): clocked
 * at the CPU's clock / 2 and stepped every 32 T-states (16 AY clocks):
 * three tone channels, the 17-bit noise generator, the envelope (16 steps,
 * shapes from register 13), the mixer and Matthew Westcott's measured
 * output levels. Mono: the channels are summed.
 *
 * Ports (Fuse's 128K decoding, A15, A14 and A1): $FFFD selects a register
 * (and reads it back), $BFFD writes it. Unused register bits read as 0.
 */
class SpectrumAy {
public:
    SpectrumAy();
    void reset();

    void selectRegister(uint8_t r) { m_current = r & 15; }
    uint8_t current() const { return m_current; }
    void write(uint8_t v);
    uint8_t read() const;
    uint8_t reg(int r) const { return m_regs[r & 15]; }
    /** Set a register (snapshots). */
    void set(int r, uint8_t v) { selectRegister((uint8_t)r); write(v); }

    /** One step (32 T-states): the summed level after it, 0..1. */
    float step();

private:
    void apply(int reg);
    void finishEnvelopeCycle(int shape);
    void advanceEnvelope(int shape);

    uint8_t m_regs[16] = {};
    uint8_t m_current = 0;
    unsigned m_levels[16] = {};
    unsigned m_toneTick[3] = {}, m_toneHigh[3] = {}, m_tonePeriod[3] = {1, 1, 1};
    unsigned m_noisePeriod = 0, m_envPeriod = 0, m_noiseTick = 0, m_toneCycles = 0;
    unsigned m_envInternalTick = 0, m_envTick = 0;
    int m_rng = 1, m_noiseToggle = 0, m_envFirst = 1, m_envReverse = 0, m_envCounter = 15;
};

}  // namespace spectrum
