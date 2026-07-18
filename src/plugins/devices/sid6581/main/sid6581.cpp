#include "sid6581.h"
#include <cstring>
#include <cmath>
#include <algorithm>

SID6581::SID6581(const std::string& name, uint32_t baseAddr)
    : m_name(name), m_baseAddr(baseAddr)
{
    SID6581::reset();
}

// ---------------------------------------------------------------------------
// ADSR rate tables
// ---------------------------------------------------------------------------
// Cycles per +1 envelope step at 1 MHz (standard SID documented values).
// Scale to the configured clock in atkCycles()/decRelCycles().
static constexpr uint32_t s_atkBase[16] = {
    9, 32, 63, 95, 149, 220, 267, 313, 392, 977, 1954, 3126, 3907, 11720, 19531, 31250
};
// Decay/Release take 3× as many cycles per step as attack.
static constexpr uint32_t s_drBase[16] = {
    27, 96, 189, 285, 447, 660, 801, 939, 1176, 2931, 5862, 9378, 11721, 35160, 58593, 93750
};

uint32_t SID6581::atkCycles(uint8_t nibble, uint32_t clockHz) {
    // s_atkBase is calibrated for 1 MHz; scale to actual clock.
    uint64_t v = (uint64_t)s_atkBase[nibble & 0x0F] * clockHz / 1000000u;
    return v ? (uint32_t)v : 1;
}
uint32_t SID6581::decRelCycles(uint8_t nibble, uint32_t clockHz) {
    uint64_t v = (uint64_t)s_drBase[nibble & 0x0F] * clockHz / 1000000u;
    return v ? (uint32_t)v : 1;
}

// ---------------------------------------------------------------------------
// Reset
// ---------------------------------------------------------------------------

void SID6581::reset() {
    std::memset(m_regs, 0, sizeof(m_regs));
    for (auto& v : m_voices) {
        v.phase     = 0;
        v.lfsr      = 0x7FFFFF;
        v.prevBit19 = 0;
        v.envPhase  = Voice::Env::IDLE;
        v.envLevel  = 0;
        v.envAccum  = 0;
        v.freq  = 0;
        v.pw    = 0;
        v.cr    = 0;
        v.ad    = 0;
        v.sr    = 0;
        v.prevGate  = false;
    }
    m_filter.lp = 0.0f;
    m_filter.bp = 0.0f;
    m_sampleFrac    = 0;
    m_audioBufRead  = 0;
    m_audioBufWrite = 0;
    m_audioBufCount = 0;
}

// ---------------------------------------------------------------------------
// IOHandler: ioRead
// ---------------------------------------------------------------------------

bool SID6581::ioRead(IBus* /*bus*/, uint32_t addr, uint8_t* val) {
    if ((addr & ~addrMask()) != m_baseAddr) return false;
    uint8_t reg = addr & 0x1F;

    switch (reg) {
        case POTX:
        case POTY:
            *val = 0xFF; // paddle inputs not implemented
            break;
        case OSC3:
            // Upper 8 bits of voice 3 phase accumulator.
            *val = (uint8_t)(m_voices[2].phase >> 16);
            break;
        case ENV3:
            *val = m_voices[2].envLevel;
            break;
        default:
            // Write-only registers return the last written value (open-bus on real chip,
            // but returning the stored value is the common emulator approximation).
            if (reg <= 0x1C)
                *val = m_regs[reg];
            else
                *val = 0xFF;
            break;
    }
    return true;
}

// ---------------------------------------------------------------------------
// IOHandler: ioWrite
// ---------------------------------------------------------------------------

bool SID6581::ioWrite(IBus* /*bus*/, uint32_t addr, uint8_t val) {
    if ((addr & ~addrMask()) != m_baseAddr) return false;
    uint8_t reg = addr & 0x1F;

    if (reg > 0x18) return true; // $D419–$D41F are read-only or unused

    m_regs[reg] = val;

    // Update voice mirrors from raw registers.
    for (int i = 0; i < 3; ++i) {
        int base = i * 7;
        Voice& v = m_voices[i];
        v.freq = (uint16_t)m_regs[base + 0] | ((uint16_t)m_regs[base + 1] << 8);
        v.pw   = (uint16_t)m_regs[base + 2] | ((uint16_t)(m_regs[base + 3] & 0x0F) << 8);
        v.cr   = m_regs[base + 4];
        v.ad   = m_regs[base + 5];
        v.sr   = m_regs[base + 6];

        // Envelope gate transitions.
        bool gate = (v.cr & CR_GATE) != 0;
        if (gate && !v.prevGate) {
            // Rising gate: restart attack from current level.
            v.envPhase = Voice::Env::ATTACK;
            v.envAccum = 0;
        } else if (!gate && v.prevGate) {
            // Falling gate: start release.
            v.envPhase = Voice::Env::RELEASE;
            v.envAccum = 0;
        }
        v.prevGate = gate;

        // Test bit: freeze oscillator, load LFSR with all-1s.
        if (v.cr & CR_TEST) {
            v.phase = 0;
            v.lfsr  = 0x7FFFFF;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// IOHandler: tick — advance synthesis and push samples
// ---------------------------------------------------------------------------

void SID6581::tick(uint64_t cycles) {
    synthesize(cycles);
}

// ---------------------------------------------------------------------------
// Synthesis
// ---------------------------------------------------------------------------

void SID6581::synthesize(uint64_t cycles) {
    // Advance all three voice oscillators and envelopes.
    // Voice sync source: v0 syncs to v2, v1 syncs to v0, v2 syncs to v1
    // (standard C64 wiring: voice N is synced/ring-mod by voice N-1, wrapping).
    tickVoice(m_voices[0], m_voices[2], cycles, m_clockHz);
    tickVoice(m_voices[1], m_voices[0], cycles, m_clockHz);
    tickVoice(m_voices[2], m_voices[1], cycles, m_clockHz);

    // Build filter coefficient from cutoff register.
    uint16_t cutoff = (uint16_t)(m_regs[FC_LO] & 0x07) | ((uint16_t)m_regs[FC_HI] << 3);
    // Map 0–2047 to ~30–12000 Hz (approximately logarithmic).
    float fcHz = 30.0f * std::pow(400.0f, (float)cutoff / 2047.0f);
    // Chamberlin filter coefficient: f = 2*sin(pi*fc/sr)  (keep small for stability)
    float f = 2.0f * std::sin(3.14159265f * fcHz / (float)m_sampleRate);
    f = std::min(f, 0.95f);
    // Resonance: Q from RES nibble (0=low, 15=high).
    uint8_t resMask = (m_regs[RES_FILT] >> 4) & 0x0F;
    float q = 1.0f / (0.5f + (float)resMask / 15.0f * 3.5f); // Q range ~0.22–2.0
    uint8_t modeVol = m_regs[MODE_VOL];
    float vol = (float)(modeVol & 0x0F) / 15.0f;

    // Determine how many output samples this tick spans.
    m_sampleFrac += (uint64_t)cycles * (uint32_t)m_sampleRate;
    uint32_t newSamples = (uint32_t)(m_sampleFrac / m_clockHz);
    m_sampleFrac %= m_clockHz;

    for (uint32_t s = 0; s < newSamples; ++s) {
        float mix = 0.0f;

        for (int i = 0; i < 3; ++i) {
            // Voice 3 can be disconnected from audio output (used as LFO/modulator).
            if (i == 2 && (modeVol & MV_3OFF)) continue;

            float vout = voiceOutput(m_voices[i]);
            bool filtered = (m_regs[RES_FILT] >> i) & 1;

            if (filtered) {
                vout = m_filter.process(vout, f, q, modeVol);
            } else {
                // Un-filtered voices bypass LP/BP but still pass through HP path.
                // Real SID: un-filtered voices are summed directly to output.
            }
            mix += vout;
        }

        // Clamp and scale by master volume.
        mix = std::max(-1.0f, std::min(1.0f, mix / 3.0f)) * vol;
        pushSample(mix);
    }
}

// ---------------------------------------------------------------------------
// Per-voice oscillator and envelope advance
// ---------------------------------------------------------------------------

void SID6581::tickVoice(Voice& v, Voice& syncSrc, uint64_t cycles, uint32_t clockHz) {
    if (v.cr & CR_TEST) return; // test bit freezes oscillator

    uint32_t prevPhase = v.phase;
    v.phase = (v.phase + (uint32_t)v.freq * (uint32_t)cycles) & 0xFFFFFF;

    // Hard sync: if sync source overflowed, reset our phase to 0.
    if ((v.cr & CR_SYNC) && (syncSrc.phase < (uint32_t)syncSrc.freq)) {
        // Approximate: sync source overflowed if its current phase < freq
        // (it just wrapped around). This is not cycle-exact but functional.
        v.phase = 0;
    }

    // Noise LFSR: clocked on rising edge of phase bit 19.
    uint32_t bit19 = (v.phase >> 19) & 1;
    if (bit19 && !v.prevBit19) {
        // Step LFSR: feedback from bits 22 and 17 (0-indexed).
        uint32_t newBit = ((v.lfsr >> 22) ^ (v.lfsr >> 17)) & 1u;
        v.lfsr = ((v.lfsr << 1) | newBit) & 0x7FFFFFu;
    }
    v.prevBit19 = bit19;
    (void)prevPhase;

    // ---------------------------------------------------------------------------
    // Envelope generator
    // ---------------------------------------------------------------------------
    if (cycles == 0) return;

    v.envAccum += cycles;

    switch (v.envPhase) {
        case Voice::Env::IDLE:
            break;

        case Voice::Env::ATTACK: {
            uint32_t rate = atkCycles((v.ad >> 4) & 0x0F, clockHz);
            while (v.envAccum >= rate && v.envPhase == Voice::Env::ATTACK) {
                v.envAccum -= rate;
                if (v.envLevel < 255) {
                    ++v.envLevel;
                } else {
                    v.envPhase = Voice::Env::DECAY;
                    v.envAccum = 0;
                }
            }
            break;
        }

        case Voice::Env::DECAY: {
            uint8_t  sustain = (v.sr >> 4) & 0x0F;
            uint8_t  sustLvl = (uint8_t)((sustain << 4) | sustain); // 4-bit → 8-bit
            uint32_t rate    = decRelCycles(v.ad & 0x0F, clockHz);
            while (v.envAccum >= rate && v.envPhase == Voice::Env::DECAY) {
                v.envAccum -= rate;
                if (v.envLevel > sustLvl) {
                    --v.envLevel;
                } else {
                    v.envLevel = sustLvl;
                    v.envPhase = Voice::Env::SUSTAIN;
                }
            }
            break;
        }

        case Voice::Env::SUSTAIN:
            // Hold at sustain level; envelope re-checked on gate change.
            v.envAccum = 0;
            break;

        case Voice::Env::RELEASE: {
            uint32_t rate = decRelCycles(v.sr & 0x0F, clockHz);
            while (v.envAccum >= rate && v.envPhase == Voice::Env::RELEASE) {
                v.envAccum -= rate;
                if (v.envLevel > 0) {
                    --v.envLevel;
                } else {
                    v.envPhase = Voice::Env::IDLE;
                }
            }
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Waveform output — Voice struct methods
// ---------------------------------------------------------------------------

uint16_t SID6581::Voice::noiseOutput() const {
    // Build 12-bit noise from LFSR bits {20,18,14,11,9,5,3,0}.
    return (uint16_t)(
        (((lfsr >> 20) & 1) << 11) |
        (((lfsr >> 18) & 1) << 10) |
        (((lfsr >> 14) & 1) <<  9) |
        (((lfsr >> 11) & 1) <<  8) |
        (((lfsr >>  9) & 1) <<  7) |
        (((lfsr >>  5) & 1) <<  6) |
        (((lfsr >>  3) & 1) <<  5) |
        (((lfsr >>  0) & 1) <<  4)
    );
}

uint16_t SID6581::Voice::waveOutput(bool syncReset, bool ringMsb) const {
    // Effective phase: hard sync resets to 0 (affects waveform output).
    uint32_t p = syncReset ? 0 : phase;

    // Combined waveforms: use empirically-derived lookup tables instead of AND operation.
    // Real 6581 hardware uses open-collector NMOS DAC with asymmetric driving strength,
    // producing complex harmonic patterns (30-40% amplitude reduction vs AND operation).
    // Table data derived from reSIDfp reverse-engineered measurements.
    uint8_t  wavSel = cr & 0xF0u;
    uint16_t out    = CombinedWaveformTable::getCombinedOutput(p, wavSel, pw, lfsr, ringMsb);

    return out;
}

float SID6581::voiceOutput(const Voice& v) const {
    // Sync / ring source is the previous voice (wiring: v0←v2, v1←v0, v2←v1).
    // We only need syncReset and ringMsb for the waveform call, approximated here.
    bool syncReset = false; // sync already applied in tickVoice
    bool ringMsb   = false; // approximate: ring mod not fully cycle-exact here

    // Find source voice index (v0's source is v2, etc.)
    int srcIdx = -1;
    for (int i = 0; i < 3; ++i) {
        if (&m_voices[i] == &v) { srcIdx = (i + 2) % 3; break; }
    }
    if (srcIdx >= 0) {
        ringMsb = (m_voices[srcIdx].phase & 0x800000u) != 0u;
    }

    uint16_t raw = v.waveOutput(syncReset, ringMsb);

    // Apply envelope (multiply 12-bit waveform × 8-bit envelope → scale to float).
    float sample = ((float)raw / 2048.0f) - 1.0f; // 12-bit centred: 0..4095 → -1..+1
    sample *= (float)v.envLevel / 255.0f;
    return sample;
}

// ---------------------------------------------------------------------------
// Filter (Chamberlin state-variable)
// ---------------------------------------------------------------------------

float SID6581::Filter::process(float in, float f, float q, uint8_t mode) {
    // Chamberlin SVF: one step per sample.
    float lp_new = lp + f * bp;
    float hp_new = in - lp_new - q * bp;
    float bp_new = f * hp_new + bp;
    lp = lp_new;
    bp = bp_new;

    float out = 0.0f;
    if (mode & MV_LP) out += lp;
    if (mode & MV_BP) out += bp;
    if (mode & MV_HP) out += hp_new;
    return out;
}

// ---------------------------------------------------------------------------
// Ring buffer
// ---------------------------------------------------------------------------

void SID6581::pushSample(float s) {
    if (m_audioBufCount >= AUDIO_BUF) return; // drop if full
    m_audioBuf[m_audioBufWrite] = s;
    m_audioBufWrite = (m_audioBufWrite + 1) & (AUDIO_BUF - 1);
    ++m_audioBufCount;
}

int SID6581::pullSamples(float* buffer, int maxSamples) {
    int n = (maxSamples < m_audioBufCount) ? maxSamples : m_audioBufCount;
    for (int i = 0; i < n; ++i) {
        buffer[i] = m_audioBuf[m_audioBufRead];
        m_audioBufRead = (m_audioBufRead + 1) & (AUDIO_BUF - 1);
    }
    m_audioBufCount -= n;
    return n;
}

void SID6581::getDeviceInfo(DeviceInfo& out) const {
    out.name = m_name;
    out.baseAddr = m_baseAddr;
    out.addrMask = addrMask();

    auto addReg = [&](const std::string& name, int reg, const std::string& desc = "") {
        out.registers.push_back({name, (uint32_t)reg, m_regs[reg], desc});
    };

    for (int i = 0; i < 3; ++i) {
        std::string v = "V" + std::to_string(i + 1) + "_";
        addReg(v + "FREQ_LO", i * 7 + 0);
        addReg(v + "FREQ_HI", i * 7 + 1);
        addReg(v + "PW_LO",   i * 7 + 2);
        addReg(v + "PW_HI",   i * 7 + 3);
        addReg(v + "CR",      i * 7 + 4);
        addReg(v + "AD",      i * 7 + 5);
        addReg(v + "SR",      i * 7 + 6);
    }
    addReg("FC_LO", 0x15);
    addReg("FC_HI", 0x16);
    addReg("RES_FILT", 0x17);
    addReg("MODE_VOL", 0x18);
    addReg("POTX", 0x19);
    addReg("POTY", 0x1A);
    addReg("OSC3", 0x1B);
    addReg("ENV3", 0x1C);

    for (int i = 0; i < 3; ++i) {
        const char* ph = "UNKNOWN";
        switch (m_voices[i].envPhase) {
            case Voice::Env::IDLE: ph = "IDLE"; break;
            case Voice::Env::ATTACK: ph = "ATTACK"; break;
            case Voice::Env::DECAY: ph = "DECAY"; break;
            case Voice::Env::SUSTAIN: ph = "SUSTAIN"; break;
            case Voice::Env::RELEASE: ph = "RELEASE"; break;
        }
        std::string prefix = "Voice " + std::to_string(i+1);
        out.state.push_back({prefix + " Env Phase", ph});
        out.state.push_back({prefix + " Env Level", std::to_string(m_voices[i].envLevel)});
    }

    uint16_t fc = ((m_regs[FC_HI] << 3) | (m_regs[FC_LO] & 0x07));
    out.state.push_back({"Filter Cutoff", std::to_string(fc)});
    out.state.push_back({"Filter Res", std::to_string(m_regs[RES_FILT] >> 4)});
    out.state.push_back({"Main Volume", std::to_string(m_regs[MODE_VOL] & 0x0F)});
}
