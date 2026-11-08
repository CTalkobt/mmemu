// SN76489 for the BBC Micro (register handling after b-em sn76489.c)

#include "sn76489.h"
#include <cmath>

namespace bbc {

namespace {
// 2 dB a step; 15 is silence
float volume(int atten) {
    static float table[16];
    static bool init = false;
    if (!init) {
        for (int i = 0; i < 15; i++) table[i] = (float)std::pow(10.0, -0.1 * i);
        table[15] = 0;
        init = true;
    }
    return table[atten & 15];
}
}  // namespace

void Sn76489::reset() {
    // The BBC's chip powers up noisy; the MOS silences it at once (b-em sn_init: noise 3)
    for (int i = 0; i < 3; i++) { m_period[i] = 0x3FF; m_count[i] = 0; m_out[i] = 1; }
    m_atten[0] = m_atten[1] = m_atten[2] = m_atten[3] = 15;
    m_noise = 3;
    m_period[3] = m_period[2];
    m_lfsr = 0x4000;
    m_latchedTone = 0;
    m_samples.clear();
}

void Sn76489::write(uint8_t data) {
    auto setNoise = [&](uint8_t v) {
        m_lfsr = 0x4000;
        m_noise = v & 0x7;
        m_count[3] = 0;
    };
    if (data & 0x80) {
        m_latch = data;
        const int reg = (data >> 4) & 7;
        const int ch = reg >> 1;                  // 0-2 tones, 3 noise
        if (reg & 1) {
            m_atten[ch] = data & 0xF;
        } else if (ch == 3) {
            setNoise(data);
        } else {
            m_period[ch] = (uint16_t)((m_period[ch] & 0x3F0) | (data & 0xF));
            m_latchedTone = ch;
        }
    } else {
        if ((m_latch & 0x70) == 0x60) {           // a second noise byte
            setNoise(data);
            return;
        }
        m_period[m_latchedTone] = (uint16_t)((m_period[m_latchedTone] & 0xF) | ((data & 0x3F) << 4));
        m_count[m_latchedTone] = 0;
    }
}

void Sn76489::tick() {
    for (int ch = 0; ch < 3; ch++) {
        if (m_count[ch] == 0) {
            m_count[ch] = m_period[ch] ? m_period[ch] : 1024;
            m_out[ch] ^= 1;
            if (ch == 2 && (m_noise & 3) == 3) {
                // Noise at tone 3's rate: the register shifts on its rising edge
                if (m_out[2]) {
                    const int fb = (m_noise & 4) ? ((m_lfsr ^ (m_lfsr >> 1)) & 1) : (m_lfsr & 1);
                    m_lfsr = (uint16_t)((m_lfsr >> 1) | (fb << 14));
                }
            }
        }
        m_count[ch]--;
    }
    if ((m_noise & 3) != 3) {
        if (m_count[3] == 0) {
            m_count[3] = (uint16_t)(0x10 << (m_noise & 3));
            m_out[3] ^= 1;
            if (m_out[3]) {
                const int fb = (m_noise & 4) ? ((m_lfsr ^ (m_lfsr >> 1)) & 1) : (m_lfsr & 1);
                m_lfsr = (uint16_t)((m_lfsr >> 1) | (fb << 14));
            }
        }
        m_count[3]--;
    }
}

float Sn76489::level() const {
    float s = 0;
    for (int ch = 0; ch < 3; ch++) {
        // Very high tones (N <= 1) hold the output high, as the chip does
        const bool high = m_period[ch] <= 1 || m_out[ch];
        s += (high ? 1.0f : -1.0f) * volume(m_atten[ch]);
    }
    s += ((m_lfsr & 1) ? 1.0f : -1.0f) * volume(m_atten[3]);
    return s * 0.15f;
}

void Sn76489::advance(uint64_t cycles) {
    constexpr double PER_SAMPLE = CLOCK_2MHZ / SAMPLE_RATE;
    while (cycles--) {
        if (++m_div == 8) {                       // 2 MHz / 8 = 250 kHz
            m_div = 0;
            tick();
        }
        m_acc += level();
        ++m_accN;
        m_phase += 1.0;
        if (m_phase >= PER_SAMPLE) {
            m_phase -= PER_SAMPLE;
            m_samples.push_back((float)(m_acc / m_accN));
            if (m_samples.size() > SAMPLE_RATE) m_samples.pop_front();
            m_acc = 0;
            m_accN = 0;
        }
    }
}

int Sn76489::pull(float* buffer, int maxSamples) {
    int n = 0;
    while (n < maxSamples && !m_samples.empty()) {
        buffer[n++] = m_samples.front();
        m_samples.pop_front();
    }
    return n;
}

}  // namespace bbc
