#pragma once

#include "combined_waveforms.h"
#include <cstdint>

/**
 * One SID voice's waveform generator and envelope generator, clocked every
 * cycle. Ported from reSIDfp (libsidplayfp, GPL-2.0-or-later: Leandro Nini,
 * Antti Lankila, Dag Lem) - WaveformGenerator and EnvelopeGenerator - which
 * model what sampling OSC3 and ENV3 on real chips showed:
 *
 *  - the noise shift register is clocked in two phases, two and three cycles
 *    after accumulator bit 19 rises; with noise combined with another
 *    waveform the output pulls the register's bits low (writeback);
 *  - the test bit holds the register in its first shift phase and, after a
 *    while, lets its bits fade to 1; releasing it completes the shift;
 *  - with no waveform selected the DAC input floats: the last output stays,
 *    then fades bit by bit;
 *  - the envelope's rate counter is a 15-bit LFSR, and gate changes go
 *    through a short pipeline (ENV3 is the counter of the cycle before).
 *
 * The shift register is held as reSIDfp holds it: bit-reversed (its bit 22 is
 * the noise LFSR's bit 0).
 */
class SidWave {
public:
    using Chip = CombinedWaveformTable::Chip;

    void setChip(Chip c) { m_chip = c; m_is6581 = c == Chip::MOS6581; }
    void setVoices(const SidWave* prev, SidWave* next) { m_prev = prev; m_next = next; }

    void reset();
    void clock();
    /** The 12-bit output this cycle; also updates OSC3 and the writeback. */
    uint32_t output();
    /** Hard sync of the next voice from this one's MSB rising (after output()). */
    void synchronize() const;

    void writeFreqLo(uint8_t v) { m_freq = (m_freq & 0xFF00) | v; }
    void writeFreqHi(uint8_t v) { m_freq = (uint32_t)(v << 8) | (m_freq & 0xFF); }
    void writePwLo(uint8_t v)   { m_pw = (m_pw & 0xF00) | v; }
    void writePwHi(uint8_t v)   { m_pw = (uint32_t)((v & 0x0F) << 8) | (m_pw & 0xFF); }
    void writeControl(uint8_t control);

    uint8_t  readOsc() const { return (uint8_t)(m_osc3 >> 4); }
    uint32_t accumulator() const { return m_acc; }
    /** The noise LFSR in the usual bit order (bit 0 = the newest bit). */
    uint32_t lfsr() const;
    uint32_t lastOutput() const { return m_waveOut; }

private:
    void shiftPhase2(uint8_t waveformOld, uint8_t waveformNew);
    void writeShiftRegister();
    void setNoiseOutput();
    void setNoNoiseOrNoiseOutput() { m_noNoiseOrNoiseOut = m_noNoise | m_noiseOut; }
    void waveBitfade();
    void shiftregBitfade();
    uint32_t wave(uint32_t ix) const;
    uint32_t pulldown(uint32_t v) const;

    const SidWave* m_prev = nullptr;
    SidWave* m_next = nullptr;
    Chip m_chip = Chip::MOS6581;
    bool m_is6581 = true;

    uint32_t m_pw = 0;
    uint32_t m_shiftReg = 0;
    uint32_t m_shiftLatch = 0;
    int      m_shiftPipeline = 0;
    uint32_t m_ringMsbMask = 0;
    uint32_t m_noNoise = 0xFFF;
    uint32_t m_noiseOut = 0;
    uint32_t m_noNoiseOrNoiseOut = 0xFFF;
    uint32_t m_noPulse = 0xFFF;
    uint32_t m_pulseOut = 0xFFF;
    uint32_t m_waveOut = 0;
    uint32_t m_acc = 0x555555;          // the even bits are high at power-up
    uint32_t m_freq = 0;
    uint32_t m_triSawPipeline = 0x555;
    uint32_t m_osc3 = 0;
    unsigned m_shiftRegReset = 0;
    unsigned m_floatingTtl = 0;
    uint8_t  m_waveform = 0;
    bool     m_test = false;
    bool     m_sync = false;
    bool     m_testOrReset = false;
    bool     m_msbRising = false;
};

class SidEnvelope {
public:
    enum class State { ATTACK, DECAY_SUSTAIN, RELEASE };

    void reset();
    void clock();
    uint8_t output() const { return m_counter; }
    /** ENV3: the counter as sampled at the start of the cycle. */
    uint8_t readEnv() const { return m_env3; }
    State state() const { return m_state; }
    uint8_t sustainLevel() const { return m_sustain; }

    void writeControl(uint8_t control);
    void writeAttackDecay(uint8_t v);
    void writeSustainRelease(uint8_t v);

private:
    void setExponentialCounter();
    void stateChange();

    uint16_t m_lfsr = 0x7FFF;
    uint16_t m_rate = 0;
    unsigned m_expCounter = 0;
    unsigned m_expPeriod = 1;
    unsigned m_newExpPeriod = 0;
    unsigned m_statePipeline = 0;
    unsigned m_envPipeline = 0;
    unsigned m_expPipeline = 0;
    State m_state = State::RELEASE;
    State m_nextState = State::RELEASE;
    bool m_counterEnabled = true;
    bool m_gate = false;
    bool m_resetLfsr = false;
    uint8_t m_counter = 0xAA;
    uint8_t m_attack = 0, m_decay = 0, m_sustain = 0, m_release = 0;
    uint8_t m_env3 = 0;
};
