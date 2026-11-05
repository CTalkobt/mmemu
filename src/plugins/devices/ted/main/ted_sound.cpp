// TED sound: two square-wave voices, noise on voice 2, DA mode.
//
// Ported from VICE (src/plus4/ted-sound.c, the non-float path, by Andreas
// Boose, Tibor Biczo and Marco van den Heuvel; its logic follows SDL-YAPE,
// github.com/calmopyrin/yapesdl). GPL-2.0-or-later.
//
// VICE generates one sample per host sample: the oscillators advance by
// oscStep = (clock / 8) / rate in 12-bit fixed point and flip when they
// reach $400. Register writes take effect at the next sample here too.

#include "ted.h"

namespace {
constexpr uint8_t CTRL_VOICE0_ENABLE  = 0x10;
constexpr uint8_t CTRL_VOICE1_ENABLE  = 0x20;
constexpr uint8_t CTRL_NOISE_ENABLE   = 0x40;
constexpr uint8_t CTRL_DIGITAL_ENABLE = 0x80;
constexpr int PRECISION = 12;
constexpr uint32_t OSCRELOADVAL = 0x400u << PRECISION;

// sdl-yape: bit 5 voice 1 output, bit 4 voice 0 output, bits 3-0 volume (8-F alike)
const int16_t VOLUME_TABLE[4 * 16] = {
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x024a, 0x064a, 0x0a4a, 0x0e4a, 0x124a, 0x164a, 0x1a4a,
    0x1e4a, 0x1e4a, 0x1e4a, 0x1e4a, 0x1e4a, 0x1e4a, 0x1e4a, 0x1e4a,
    0x0000, 0x024a, 0x064a, 0x0a4a, 0x0e4a, 0x124a, 0x164a, 0x1a4a,
    0x1e4a, 0x1e4a, 0x1e4a, 0x1e4a, 0x1e4a, 0x1e4a, 0x1e4a, 0x1e4a,
    0x0000, 0x0494, 0x0cd4, 0x1596, 0x1f30, 0x29a2, 0x34ec, 0x410e,
    0x4e08, 0x4e08, 0x4e08, 0x4e08, 0x4e08, 0x4e08, 0x4e08, 0x4e08
};
}  // namespace

void Ted::setSampleRate(int hz) {
    if (hz <= 0) return;
    m_sampleRate = hz;
    m_audioAcc = 0;
    soundInit();
}

void Ted::soundInit() {
    // ted_sound_machine_init(): only the oscillator step depends on the rates
    const double clock = (double)clockHz(m_std);
    m_snd.oscStep = (uint32_t)(((clock / 8) * (double)(1 << PRECISION)) / (double)m_sampleRate + 0.5);
}

void Ted::soundClockShift() {
    uint8_t r = m_snd.noiseShiftRegister;
    m_snd.noiseShiftRegister = (uint8_t)((r << 1) | (((r >> 7) ^ (r >> 5) ^ (r >> 4) ^ (r >> 1)) & 1));
}

uint16_t Ted::voiceFreq(int v) const {
    return v == 0 ? (uint16_t)(m_snd.data[0] | (m_snd.data[4] << 8))
                  : (uint16_t)(m_snd.data[1] | (m_snd.data[2] << 8));
}

void Ted::soundStore(uint8_t reg, uint8_t val) {
    Sound& s = m_snd;
    unsigned freq;
    switch (reg) {
    case 0x0E:   // voice 0 frequency low
    case 0x12:   // voice 0 frequency high (bits 0-1)
        if (reg == 0x0E) s.data[0] = val; else s.data[4] = val & 3;
        freq = s.data[0] | (s.data[4] << 8);
        if (freq == 0x3FE) {
            s.voice0Sign = CTRL_VOICE0_ENABLE;
            s.voice0CachedOutput = s.volume | s.voice0OutputEnabled;
        }
        s.voice0Reload = ((freq + 1) & 0x3FF) << PRECISION;
        break;
    case 0x0F:   // voice 1 frequency low
    case 0x10:   // voice 1 frequency high (bits 0-1)
        if (reg == 0x0F) s.data[1] = val; else s.data[2] = val & 3;
        freq = s.data[1] | (s.data[2] << 8);
        if (freq == 0x3FE) {
            s.voice1Sign = CTRL_VOICE1_ENABLE;
            s.voice1CachedOutput = s.volume | s.voice1OutputEnabled | (s.noise >> 1);
        }
        s.voice1Reload = ((freq + 1) & 0x3FF) << PRECISION;
        break;
    case 0x11:
        // bits 0-3 volume, 4 voice 0, 5 voice 1, 6 noise, 7 DA
        s.volume = val & 0x0F;
        s.voice0OutputEnabled = val & CTRL_VOICE0_ENABLE;
        s.voice1OutputEnabled = val & CTRL_VOICE1_ENABLE;
        s.noise = ((val & 0x60) == CTRL_NOISE_ENABLE) ? CTRL_NOISE_ENABLE : 0;
        s.digital = val & CTRL_DIGITAL_ENABLE;
        if (s.digital) {
            s.voice0Sign = CTRL_VOICE0_ENABLE;
            s.voice0Accu = s.voice0Reload;
            s.voice1Sign = CTRL_VOICE1_ENABLE;
            s.voice1Accu = s.voice1Reload;
            s.noiseShiftRegister = 0xFF;
            s.digitalCachedOutput = (uint16_t)VOLUME_TABLE[val & 0x3F];
        }
        s.voice0CachedOutput = s.volume | (s.voice0Sign & s.voice0OutputEnabled);
        s.voice1CachedOutput = s.volume | (s.voice1Sign & s.voice1OutputEnabled)
                             | ((((s.noiseShiftRegister & 1) ? CTRL_NOISE_ENABLE : 0) & s.noise) >> 1);
        s.data[3] = val;
        break;
    default:
        break;
    }
}

uint8_t Ted::soundRead(uint8_t reg) const {
    switch (reg) {
    case 0x0E: return m_snd.data[0];
    case 0x0F: return m_snd.data[1];
    case 0x10: return (uint8_t)((m_snd.data[2] & 0x7F) | 0x7C);
    case 0x11: return m_snd.data[3];
    case 0x12: return m_snd.data[4];
    default:   return 0;
    }
}

void Ted::renderSample() {
    Sound& s = m_snd;
    int16_t out;
    if (s.digital) {
        out = (int16_t)s.digitalCachedOutput;
    } else {
        // the "state engine" of ted_sound_machine_calculate_samples(): VICE
        // advances it whenever 8 or more cycles have passed since the last
        // sample, which is every sample at audio rates
        if ((s.voice0Reload & (0x3FFu << PRECISION)) != (0x3FFu << PRECISION)) {
            if ((s.voice0Accu += s.oscStep) >= OSCRELOADVAL) {
                s.voice0Sign ^= CTRL_VOICE0_ENABLE;
                s.voice0CachedOutput = s.volume | (s.voice0Sign & s.voice0OutputEnabled);
                s.voice0Accu = s.voice0Reload + (s.voice0Accu - OSCRELOADVAL);
            }
        }
        if ((s.voice1Reload & (0x3FFu << PRECISION)) != (0x3FFu << PRECISION)) {
            if ((s.voice1Accu += s.oscStep) >= OSCRELOADVAL) {
                s.voice1Sign ^= CTRL_VOICE1_ENABLE;
                s.voice1CachedOutput = s.volume | (s.voice1Sign & s.voice1OutputEnabled)
                                     | ((((s.noiseShiftRegister & 1) ? CTRL_NOISE_ENABLE : 0) & s.noise) >> 1);
                soundClockShift();
                s.voice1Accu = s.voice1Reload + (s.voice1Accu - OSCRELOADVAL);
            }
        }
        out = VOLUME_TABLE[(s.voice0CachedOutput | s.voice1CachedOutput) & 0x3F];
    }
    pushSample((float)out / 32768.0f);
}

void Ted::pushSample(float v) {
    if (m_audioCount >= AUDIO_BUF) return;   // full: drop
    m_audioBuf[m_audioWrite] = v;
    m_audioWrite = (m_audioWrite + 1) % AUDIO_BUF;
    ++m_audioCount;
}

int Ted::pullSamples(float* buffer, int maxSamples) {
    int n = maxSamples < m_audioCount ? maxSamples : m_audioCount;
    for (int i = 0; i < n; ++i) {
        buffer[i] = m_audioBuf[m_audioRead];
        m_audioRead = (m_audioRead + 1) % AUDIO_BUF;
    }
    m_audioCount -= n;
    return n;
}
