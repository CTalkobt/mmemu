#include "sid6581.h"
#include "filter_curve.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>

// ---------------------------------------------------------------------------
// Soft-clipping distortion for high-resonance filtering
// ---------------------------------------------------------------------------

/**
 * Simplified soft-clipping function using tanh-like approximation.
 * More efficient than full tanh() call; provides smooth saturation.
 *
 * Input: x in range [-threshold..+threshold]
 * Output: soft-clipped value in range ~[-1..+1]
 *
 * Approximation: tanh(x) ≈ x / (1 + |x|) for low CPU cost.
 * This is computationally cheaper than std::tanh() and works well
 * for distortion in audio processing.
 */
static inline float softClip(float x, float threshold) {
    if (threshold <= 0.0f) return x;  // no clipping

    // Normalize by threshold
    float normalized = x / threshold;

    // Apply soft-clip using economical approximation
    // Avoids the full tanh which costs ~50 cycles; this costs ~10
    if (normalized >  1.5f) return  threshold;  // hard limit at high distortion
    if (normalized < -1.5f) return -threshold;

    // Smooth saturation in [-1.5..+1.5]: uses rational approximation
    // tanh(x) ≈ x * (27.0 + x*x) / (27.0 + 9.0*x*x)
    float x2 = normalized * normalized;
    float numerator = normalized * (27.0f + x2);
    float denominator = 27.0f + 9.0f * x2;
    return (numerator / denominator) * threshold;
}

/**
 * Calculate saturation threshold based on resonance.
 * High resonance (Q > 1) causes analog circuitry to clip.
 *
 * Resonance parameter: 0-15 (from $D417 bits 4-7)
 * Q value: roughly 0.22 (Q_min) to 2.0 (Q_max)
 *
 * Saturation threshold is inversely related to Q:
 * - Low Q (0-7): threshold ≈ 1.5-2.0 (little clipping)
 * - High Q (8-15): threshold ≈ 0.2-0.8 (heavy clipping, adds harmonics)
 *
 * For the 6581, the analog filter can saturate quite aggressively.
 * For the 8580, the distortion is smoother and less pronounced.
 */
static inline float calculateSaturationThreshold(uint8_t resMask, bool is6581) {
    // resMask: 0-15 (4-bit resonance control)
    // Higher resMask = higher Q = more saturation

    if (resMask < 4) return 2.5f;  // Q < 0.5: almost no distortion

    // Q-dependent curve: distortion increases sharply with resonance
    float q_factor = 0.22f + (float)resMask / 15.0f * 1.78f;  // Q in [0.22..2.0]

    // Threshold inversely proportional to Q (higher Q → lower threshold → more clipping)
    float threshold = 2.0f / q_factor;

    // 6581 has more pronounced distortion due to analog limitations
    if (is6581) {
        threshold *= 0.8f;  // ~20% more aggressive clipping
    } else {
        threshold *= 1.1f;  // 8580: smoother, less extreme distortion
    }

    // Clamp to reasonable range [0.15..2.5]
    return std::max(0.15f, std::min(2.5f, threshold));
}

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
    m_scopePos   = 0;
    m_scopeCount = 0;
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
            // Upper 8 bits of voice 3's waveform output (reSID readOSC): the
            // folded triangle, pulse, noise etc., not the raw accumulator.
            *val = (uint8_t)(m_voices[2].waveOutput(false, ringMsbFor(m_voices[2])) >> 4);
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
    // Voice N is synced / ring-modulated by voice N-1, wrapping (v0←v2,
    // v1←v0, v2←v1).
    //
    // 1. Oscillators. As in reSID, all three accumulators advance first and
    //    hard sync is applied afterwards from the sync sources' MSB rising
    //    edges, so the result does not depend on the order voices are updated.
    //    riseAt[i] = cycle (1..cycles) of voice i's last MSB rising edge in
    //    this tick, 0 if none.
    uint64_t riseAt[3] = {0, 0, 0};
    bool anyRise = false;
    if (m_voices[0].freq | m_voices[1].freq | m_voices[2].freq) {   // all silent: nothing moves
        for (int i = 0; i < 3; ++i) {
            Voice& v = m_voices[i];
            if ((v.cr & CR_TEST) || v.freq == 0) continue;   // test bit freezes the oscillator
            uint64_t f = v.freq, p0 = v.phase, end = p0 + f * cycles;
            if (end >= 0x800000) {
                // Last crossing of 0x800000 + k * 2^24 (bit 23 going 0 -> 1)
                uint64_t c = 0x800000 + (((end - 0x800000) >> 24) << 24);
                if (c > p0) { riseAt[i] = (c - p0 + f - 1) / f; anyRise = true; }
            }
            v.phase = (uint32_t)(end & 0xFFFFFF);
        }
    }
    for (int i = 0; anyRise && i < 3; ++i) {
        Voice& v = m_voices[i];
        int src = (i + 2) % 3, srcSrc = (i + 1) % 3;
        if (!(v.cr & CR_SYNC) || (v.cr & CR_TEST) || !riseAt[src]) continue;
        // reSID: no sync when the source is itself being synced in that cycle
        if ((m_voices[src].cr & CR_SYNC) && riseAt[srcSrc] == riseAt[src]) continue;
        // Reset to 0 at the edge, then run for the rest of the tick
        v.phase = (uint32_t)(((uint64_t)v.freq * (cycles - riseAt[src])) & 0xFFFFFF);
    }

    // 2. Noise LFSR and envelopes. A silent idle voice (common: SIDs untouched
    //    during boot) has no LFSR edge and only its idle envelope accumulator
    //    changes: skip the call for it.
    auto lfsrAndEnvelope = [&](Voice& v) {
        if (v.freq == 0 && !(v.cr & CR_TEST) && v.envPhase == Voice::Env::IDLE &&
            ((v.phase >> 19) & 1) == v.prevBit19)
            v.envAccum += cycles;
        else
            tickVoice(v, cycles, m_clockHz);
    };
    lfsrAndEnvelope(m_voices[0]);
    lfsrAndEnvelope(m_voices[1]);
    lfsrAndEnvelope(m_voices[2]);

    // Determine how many output samples this tick spans. Most ticks are one
    // CPU instruction and produce none, so return before any filter maths.
    m_sampleFrac += (uint64_t)cycles * (uint32_t)m_sampleRate;
    uint32_t newSamples = (uint32_t)(m_sampleFrac / m_clockHz);
    m_sampleFrac %= m_clockHz;
    if (newSamples == 0) return;

    uint16_t cutoff = (uint16_t)(m_regs[FC_LO] & 0x07) | ((uint16_t)m_regs[FC_HI] << 3);
    uint8_t resMask = (m_regs[RES_FILT] >> 4) & 0x0F;
    uint8_t modeVol = m_regs[MODE_VOL];
    float vol = (float)(modeVol & 0x0F) / 15.0f;

    // Configure filter with current chip variant
    m_filter.isHardware6581 = (m_filterVariant == 0);  // 0 = 6581, 1 = 8580

    // Filter coefficients depend only on cutoff, resonance, variant and sample
    // rate: recompute them only when one of those changes.
    if (!m_coeffValid || cutoff != m_coeffCutoff || resMask != m_coeffRes ||
        m_filterVariant != m_coeffVariant || m_sampleRate != m_coeffSampleRate) {
        // Map 0–2047 to ~30–12000 Hz (approximately logarithmic).
        float fcHz = 30.0f * std::pow(400.0f, (float)cutoff / 2047.0f);
        // Chamberlin filter coefficient: f = 2*sin(pi*fc/sr)  (keep small for stability)
        float f = 2.0f * std::sin(3.14159265f * fcHz / (float)m_sampleRate);
        f = std::min(f, 0.95f);
        // Resonance: Q from RES nibble (0=low, 15=high).
        float q = 1.0f / (0.5f + (float)resMask / 15.0f * 3.5f); // Q range ~0.22–2.0

        // Apply nonlinear filter curve modeling (6581 vs 8580 variants)
        // The real SID filter exhibits nonlinearity due to component tolerances,
        // transistor gain variation, and op-amp limitations. This adjusts the
        // cutoff frequency coefficient and resonance multiplier based on empirical
        // measurements from real hardware (via reSIDfp reverse-engineering).
        FilterCurve::Variant filterVariant = (m_filterVariant == 0)
            ? FilterCurve::Variant::SID_6581
            : FilterCurve::Variant::SID_8580;
        FilterCurve::FilterCoeffs curved = FilterCurve::applyNonlinearity(
            f, q, cutoff, resMask, filterVariant);
        m_coeffF = curved.f;
        m_coeffQ = curved.q;
        m_coeffCutoff = cutoff;
        m_coeffRes = resMask;
        m_coeffVariant = m_filterVariant;
        m_coeffSampleRate = m_sampleRate;
        m_coeffValid = true;
    }
    const float f = m_coeffF;
    const float q = m_coeffQ;

    for (uint32_t s = 0; s < newSamples; ++s) {
        float mix = 0.0f;

        for (int i = 0; i < 3; ++i) {
            float vout = voiceOutput(m_voices[i]);
            m_scope[i][m_scopePos] = vout;   // scope shows voice 3 even when disconnected

            // Voice 3 can be disconnected from audio output (used as LFO/modulator).
            if (i == 2 && (modeVol & MV_3OFF)) continue;

            bool filtered = (m_regs[RES_FILT] >> i) & 1;

            if (filtered) {
                // Pass voice to filter with soft-clipping distortion
                vout = m_filter.process(vout, f, q, modeVol, resMask);
            } else {
                // Un-filtered voices bypass LP/BP but still pass through HP path.
                // Real SID: un-filtered voices are summed directly to output.
            }
            mix += vout;
        }

        // Clamp and scale by master volume.
        mix = std::max(-1.0f, std::min(1.0f, mix / 3.0f)) * vol;
        m_scope[3][m_scopePos] = mix;
        m_scopePos = (m_scopePos + 1) & (SCOPE_LEN - 1);
        if (m_scopeCount < SCOPE_LEN) ++m_scopeCount;
        pushSample(mix);
    }
}

// ---------------------------------------------------------------------------
// Per-voice oscillator and envelope advance
// ---------------------------------------------------------------------------

void SID6581::tickVoice(Voice& v, uint64_t cycles, uint32_t clockHz) {
    // The oscillator phase (and hard sync) is advanced in synthesize()
    if (v.cr & CR_TEST) return; // test bit freezes oscillator

    // Noise LFSR: clocked on rising edge of phase bit 19.
    uint32_t bit19 = (v.phase >> 19) & 1;
    if (bit19 && !v.prevBit19) {
        // Step LFSR: feedback from bits 22 and 17 (0-indexed).
        uint32_t newBit = ((v.lfsr >> 22) ^ (v.lfsr >> 17)) & 1u;
        v.lfsr = ((v.lfsr << 1) | newBit) & 0x7FFFFFu;
    }
    v.prevBit19 = bit19;

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

bool SID6581::ringMsbFor(const Voice& v) const {
    // Ring modulation only when the voice's RING bit is set: the triangle then
    // folds on (own MSB XOR source MSB), as in reSID. Source = previous voice.
    if (!(v.cr & CR_RING)) return false;
    int srcIdx = (int)(&v - m_voices + 2) % 3;
    return (m_voices[srcIdx].phase & 0x800000u) != 0u;
}

float SID6581::voiceOutput(const Voice& v) const {
    // Ring / sync source is the previous voice (wiring: v0←v2, v1←v0, v2←v1).
    bool syncReset = false; // sync already applied in synthesize()

    bool ringMsb = ringMsbFor(v);

    uint16_t raw = v.waveOutput(syncReset, ringMsb);

    // Apply envelope (multiply 12-bit waveform × 8-bit envelope → scale to float).
    float sample = ((float)raw / 2048.0f) - 1.0f; // 12-bit centred: 0..4095 → -1..+1
    sample *= (float)v.envLevel / 255.0f;
    return sample;
}

// ---------------------------------------------------------------------------
// Filter (Chamberlin state-variable with soft-clipping distortion)
// ---------------------------------------------------------------------------

float SID6581::Filter::process(float in, float f, float q, uint8_t mode, uint8_t resMask) {
    // Chamberlin SVF: one step per sample.
    // This is the state-variable filter architecture used in the real 6581.
    // The three filter sections (lowpass, bandpass, highpass) interact through
    // the coefficient f (cutoff) and q (resonance feedback).
    //
    // Circuit equations (continuous approximation):
    //   lp_new = lp + f * bp
    //   hp_new = in - lp_new - q * bp
    //   bp_new = f * hp_new + bp
    float lp_new = lp + f * bp;
    float hp_new = in - lp_new - q * bp;
    float bp_new = f * hp_new + bp;

    // Soft-clipping distortion for high-resonance saturation
    // =====================================================
    // The 6581's analog filter can saturate at high Q values, adding harmonic
    // content that's characteristic of the chip's "warm" and "dirty" sound.
    //
    // The feedback term (q * bp) accumulates energy in the filter at high
    // resonance, and the analog summing nodes cannot reproduce signals beyond
    // their saturation limits. This creates soft nonlinearity.
    //
    // We apply soft-clipping to the band-pass state (bp_new) since:
    //   1. It carries the resonance feedback and is most prone to saturation
    //   2. Clipping here adds harmonics that feed into future samples
    //   3. The distortion is smooth (soft-clip) not harsh (hard-clip)
    //
    // This approach preserves audio quality at low resonance while adding
    // characteristic warmth when the filter is pushed hard.

    float saturation_threshold = calculateSaturationThreshold(resMask, isHardware6581);

    // Apply soft-clipping to the band-pass output
    // The clipped value is fed back into the next sample via state update.
    // This is where analog nonlinearity creates the SID's harmonic richness.
    bp_new = softClip(bp_new, saturation_threshold);

    // Update stored state for next sample
    lp = lp_new;
    bp = bp_new;

    // Output stage: mix the three sections (LP/BP/HP) according to mode bits.
    // Each section can be independently routed to the output.
    float out = 0.0f;
    if (mode & MV_LP) out += lp;
    if (mode & MV_BP) out += bp;   // Note: bp is now soft-clipped
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

// ---------------------------------------------------------------------------
// IAudioInspect (#30)
// ---------------------------------------------------------------------------

std::string SID6581::chipName(int /*chip*/) const {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%s $%04X", m_name.c_str(), (unsigned)m_baseAddr);
    return buf;
}

bool SID6581::voiceInfo(int chip, int voice, VoiceInfo& out) const {
    if (chip != 0 || voice < 0 || voice > 2) return false;
    const Voice& v = m_voices[voice];
    out.freq       = v.freq;
    out.freqHz     = (double)v.freq * (double)m_clockHz / 16777216.0;
    out.pulseWidth = v.pw & 0x0FFF;
    out.control    = v.cr;
    out.attack     = v.ad >> 4;
    out.decay      = v.ad & 0x0F;
    out.sustain    = v.sr >> 4;
    out.release    = v.sr & 0x0F;
    switch (v.envPhase) {
        case Voice::Env::IDLE:    out.envPhase = EnvPhase::Idle;    break;
        case Voice::Env::ATTACK:  out.envPhase = EnvPhase::Attack;  break;
        case Voice::Env::DECAY:   out.envPhase = EnvPhase::Decay;   break;
        case Voice::Env::SUSTAIN: out.envPhase = EnvPhase::Sustain; break;
        case Voice::Env::RELEASE: out.envPhase = EnvPhase::Release; break;
    }
    out.envLevel = v.envLevel;
    out.filtered = (m_regs[RES_FILT] >> voice) & 1;
    out.muted    = voice == 2 && (m_regs[MODE_VOL] & MV_3OFF);
    return true;
}

int SID6581::scopeSamples(int chip, int channel, float* out, int n) const {
    if (chip != 0 || channel < 0 || channel > 3 || n <= 0 || !out) return 0;
    if (n > m_scopeCount) n = m_scopeCount;
    int start = (m_scopePos - n) & (SCOPE_LEN - 1);
    for (int i = 0; i < n; ++i)
        out[i] = m_scope[channel][(start + i) & (SCOPE_LEN - 1)];
    return n;
}
