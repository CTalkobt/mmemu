#pragma once

#include <cstdint>
#include <deque>

namespace bbc {

/**
 * The SN76489 sound chip as the BBC Micro has it: a 4 MHz clock divided
 * by 16 (250 kHz), three square-wave tones (output frequency 125 kHz / N,
 * N of 0 counting as 1024), a noise channel with the BBC's 15-bit shift
 * register (white: bits 0 and 1 fed back; periodic: bit 0), at 250 kHz /
 * 16, 32 or 64, or at tone 3's rate, and 2 dB volume steps.
 *
 * Writes as b-em takes them (sn76489.c): a byte with bit 7 set latches a
 * register and sets its low bits; a byte without sets the high six bits of
 * the last tone register written. The output is mixed to 44.1 kHz mono.
 */
class Sn76489 {
public:
    static constexpr int SAMPLE_RATE = 44100;
    static constexpr double CLOCK_2MHZ = 2000000.0;

    void reset();
    void write(uint8_t data);
    /** Run `cycles` 2 MHz cycles. */
    void advance(uint64_t cycles);
    int pull(float* buffer, int maxSamples);

    uint16_t tone(int ch) const { return m_period[ch & 3]; }
    uint8_t attenuation(int ch) const { return m_atten[ch & 3]; }
    uint8_t noiseControl() const { return m_noise; }

private:
    void tick();            // one 250 kHz clock
    float level() const;

    uint16_t m_period[4] = {0x3FF, 0x3FF, 0x3FF, 0x10};   // tones 0-2; [3]: the noise period
    uint8_t m_atten[4] = {15, 15, 15, 15};                 // 15 = off
    uint16_t m_count[4] = {};
    int m_out[4] = {1, 1, 1, 1};
    uint8_t m_noise = 0;
    uint16_t m_lfsr = 0x4000;
    int m_latchedTone = 0;
    uint8_t m_latch = 0;
    int m_div = 0;
    double m_phase = 0, m_acc = 0;
    int m_accN = 0;
    std::deque<float> m_samples;
};

}  // namespace bbc
