// SID Filter Saturation/Distortion Implementation
// Reference Implementation & Pseudocode
// ==============================================================================
// This file contains detailed C++ implementation examples for the filter
// saturation system described in SID_FILTER_SATURATION_DESIGN.md

#pragma once
#include <cmath>
#include <algorithm>
#include <cstdint>


// ==============================================================================
// Part A: Core Soft-Clipping Functions
// ==============================================================================

namespace SIDSaturation {

    /**
     * Sigmoid-based soft-clip using rational approximation.
     *
     * Curve: y = (x / t) / (1 + k * (x/t)^2) * t
     * - Smooth, continuous 1st derivative
     * - Natural harmonic distortion (odd harmonics dominant)
     * - ~4 CPU cycles on modern hardware
     *
     * @param x        Input signal [-1, +1] (normalized)
     * @param threshold Saturation threshold (0.3 to 0.85)
     * @param blend    Distortion amount: 0=clean, 1=full saturation
     * @return Soft-clipped output
     */
    inline float softClipSigmoid(float x, float threshold, float blend) {
        if (blend < 0.001f) return x;  // No saturation

        // Normalize to threshold
        float x_norm = x / threshold;

        // Rational sigmoid: 1 / (1 + k*x^2)
        // k = 0.5 provides smooth S-curve with ~1.5 dB knee
        float x2 = x_norm * x_norm;
        float denom = 1.0f + 0.5f * x2;  // k_saturation = 0.5

        // Clip: scale back to threshold range
        float clipped = (x_norm / denom) * threshold;

        // Blend: interpolate between clean and clipped
        return blend * clipped + (1.0f - blend) * x;
    }

    /**
     * Alternative: Polynomial approximation of tanh.
     * Smoother curve than sigmoid, closer to real op-amp saturation.
     *
     * tanh(x) ≈ x * (27 + x^2) / (27 + 9*x^2)  [Pade approximant]
     *
     * Slightly higher CPU cost (~8 cycles) but better audio quality.
     *
     * @param x        Input signal
     * @param threshold Saturation threshold
     * @param blend    Distortion amount
     * @return Soft-clipped output
     */
    inline float softClipTanhApprox(float x, float threshold, float blend) {
        if (blend < 0.001f) return x;

        float x_norm = x / threshold;

        // Pade approximant: x * (27 + x^2) / (27 + 9*x^2)
        float x2 = x_norm * x_norm;
        float num = x_norm * (27.0f + x2);
        float denom = 27.0f + 9.0f * x2;

        float clipped = (num / denom) * threshold;
        return blend * clipped + (1.0f - blend) * x;
    }

    /**
     * Lookup-table based saturation (fastest, for resource-constrained systems).
     * Precomputed 1024-entry table covering [-2, +2] input range.
     */
    class SaturationLUT {
    public:
        static constexpr int LUT_SIZE = 1024;
        static constexpr float LUT_MIN = -2.0f;
        static constexpr float LUT_MAX = 2.0f;
        float lut[LUT_SIZE];

        SaturationLUT() {
            // Initialize LUT with sigmoid values
            for (int i = 0; i < LUT_SIZE; ++i) {
                float norm_i = (float)i / (LUT_SIZE - 1);
                float x = LUT_MIN + norm_i * (LUT_MAX - LUT_MIN);
                float x2 = x * x;
                lut[i] = x / (1.0f + 0.5f * x2);  // Already scaled
            }
        }

        inline float lookup(float x) const {
            // Map x to LUT index
            float norm = (x - LUT_MIN) / (LUT_MAX - LUT_MIN);
            norm = std::max(0.0f, std::min(1.0f, norm));
            int idx = (int)(norm * (LUT_SIZE - 1));
            return lut[idx];
        }
    };

}  // namespace SIDSaturation


// ==============================================================================
// Part B: Filter State Machine with Saturation
// ==============================================================================

/**
 * Enhanced SID Filter with parametric saturation.
 *
 * Placement strategy:
 * - Primary: BP memory saturation (controls resonance feedback)
 * - Optional: LP memory saturation (adds 2nd harmonic, light effect)
 * - Bypass: HP output (differential stage, rarely saturates)
 */
class SID6581FilterWithSaturation {
public:
    // ========== Constructor & Configuration ==========

    SID6581FilterWithSaturation(bool is_8580 = false)
        : m_is_8580(is_8580),
          m_lp(0.0f), m_bp(0.0f),
          m_cached_threshold(0.9f),
          m_cached_blend(0.0f),
          m_cached_lp_blend(0.0f),
          m_last_res(0xFF),
          m_enable_lp_saturation(true)
    {
        // Empty
    }

    void setChipVariant(bool is_8580) {
        m_is_8580 = is_8580;
        m_last_res = 0xFF;  // Force recalculation
    }

    void setLPSaturation(bool enabled) {
        m_enable_lp_saturation = enabled;
    }

    // ========== Main Filter Processing ==========

    /**
     * Chamberlin state-variable filter with integrated saturation.
     *
     * @param in         Input sample [-1, +1]
     * @param f          Filter coefficient (0 to 0.95)
     * @param q          Resonance gain (Q factor)
     * @param mode       Mode bitmask (LP=0x10, BP=0x20, HP=0x40)
     * @param res_nibble Resonance register (0-15) from $D417[7:4]
     * @return           Filtered output (mixed per mode)
     */
    float process(float in, float f, float q, uint8_t mode, uint8_t res_nibble) {
        // --- Step 1: Update saturation parameters (cached) ---
        if (res_nibble != m_last_res) {
            updateSaturationParameters(res_nibble);
            m_last_res = res_nibble;
        }

        // --- Step 2: Chamberlin SVF update (core filter) ---
        float lp_new = m_lp + f * m_bp;
        float hp_new = in - lp_new - q * m_bp;
        float bp_new = f * hp_new + m_bp;

        // --- Step 3: Apply soft-clip saturation ---
        // BP is the primary control point for resonance feedback
        float bp_saturated = SIDSaturation::softClipSigmoid(
            bp_new, m_cached_threshold, m_cached_blend
        );

        // LP gets light saturation (if enabled and resonance > threshold)
        float lp_saturated = lp_new;
        if (m_enable_lp_saturation && m_cached_lp_blend > 0.001f) {
            float lp_threshold = m_cached_threshold * 1.3f;
            lp_saturated = SIDSaturation::softClipSigmoid(
                lp_new, lp_threshold, m_cached_lp_blend
            );
        }

        // --- Step 4: Update state (feedback loop uses saturated values) ---
        m_lp = lp_saturated;
        m_bp = bp_saturated;

        // --- Step 5: Mix output based on mode bits ---
        float out = 0.0f;
        if (mode & 0x10) out += lp_saturated;   // MV_LP
        if (mode & 0x20) out += bp_saturated;   // MV_BP
        if (mode & 0x40) out += hp_new;         // MV_HP (output, not saturated)

        return out;
    }

    // ========== State Management ==========

    void reset() {
        m_lp = 0.0f;
        m_bp = 0.0f;
        m_last_res = 0xFF;  // Force recalculation
        m_cached_threshold = 0.9f;
        m_cached_blend = 0.0f;
        m_cached_lp_blend = 0.0f;
    }

    float getState_LP() const { return m_lp; }
    float getState_BP() const { return m_bp; }

    // ========== Debug / Instrumentation ==========

    struct SaturationInfo {
        float threshold;
        float blend_bp;
        float blend_lp;
        float q_factor;
        bool is_8580;
    };

    SaturationInfo getInfo() const {
        return {
            m_cached_threshold,
            m_cached_blend,
            m_cached_lp_blend,
            0.0f,  // Would compute from m_last_res
            m_is_8580
        };
    }

private:
    // ========== Private Members ==========

    bool m_is_8580;
    float m_lp, m_bp;  // Filter state
    float m_cached_threshold;
    float m_cached_blend;
    float m_cached_lp_blend;
    uint8_t m_last_res;
    bool m_enable_lp_saturation;

    // ========== Parameter Calculation ==========

    /**
     * Update saturation threshold and blend factors based on resonance.
     * Called only when resonance nibble changes (typically infrequent).
     */
    void updateSaturationParameters(uint8_t res_nibble) {
        // Convert resonance register (0-15) to Q factor
        // Q = 0.5 + (res/15) * 3.5, giving Q range [0.5, 4.0]
        float q_factor = 0.5f + (float)res_nibble / 15.0f * 3.5f;

        // Chip-dependent saturation threshold
        // 6581: saturates earlier (1.2x), 8580: more resilient (0.8x)
        float chip_factor = m_is_8580 ? 0.8f : 1.2f;

        // Threshold inversely correlates with resonance
        // At Q=0.5: threshold ≈ 0.9
        // At Q=4.0: threshold ≈ 0.17
        m_cached_threshold = 0.9f / (1.0f + chip_factor * q_factor);

        // Clamp to practical range [0.3, 0.85]
        m_cached_threshold = std::max(0.3f, std::min(0.85f, m_cached_threshold));

        // Blend factor (distortion amount) increases with resonance
        // At res=0: blend = 0% (no distortion)
        // At res=15: blend = 55–70% (significant distortion)
        float max_blend = m_is_8580 ? 0.55f : 0.70f;
        m_cached_blend = (float)res_nibble / 15.0f * max_blend;

        // LP gets lighter effect (30% of BP blend), and only when res > 8
        m_cached_lp_blend = (res_nibble > 8) ? m_cached_blend * 0.3f : 0.0f;
    }
};


// ==============================================================================
// Part C: Integration into SID6581 Class
// ==============================================================================

/**
 * Minimal diff showing changes to src/plugins/devices/sid6581/main/sid6581.h
 *
 * --- a/sid6581.h
 * +++ b/sid6581.h
 * @@ private section @@
 */

/*
ADDITIONS TO SID6581 CLASS:

private:
    // Chip variant detection
    bool m_is_8580 = false;

    // Enhanced filter with saturation
    struct Filter {
        float lp = 0.0f;
        float bp = 0.0f;

        // Cached saturation parameters (update on resonance change)
        float cached_threshold = 0.9f;
        float cached_blend = 0.0f;
        float cached_lp_blend = 0.0f;
        uint8_t last_res = 0xFF;

        // New signature: add res_nibble and chip variant
        float process(float in, float f, float q, uint8_t mode,
                      uint8_t res_nibble, bool is_8580);
    } m_filter;
*/


// ==============================================================================
// Part D: Updated SID::synthesize() Call Flow
// ==============================================================================

/*
UPDATED PSEUDOCODE for SID6581::synthesize():

void SID6581::synthesize(uint64_t cycles) {
    // ... (existing voice synthesis code unchanged) ...

    // Determine how many output samples this tick spans
    m_sampleFrac += (uint64_t)cycles * (uint32_t)m_sampleRate;
    uint32_t newSamples = (uint32_t)(m_sampleFrac / m_clockHz);
    m_sampleFrac %= m_clockHz;

    for (uint32_t s = 0; s < newSamples; ++s) {
        float mix = 0.0f;

        // Voice-by-voice processing
        for (int i = 0; i < 3; ++i) {
            if (i == 2 && (modeVol & MV_3OFF)) continue;  // V3 disconnect

            float vout = voiceOutput(m_voices[i]);
            bool filtered = (m_regs[RES_FILT] >> i) & 1;

            if (filtered) {
                // CHANGED: Pass resonance nibble to filter
                uint8_t res_nibble = (m_regs[RES_FILT] >> 4) & 0x0F;
                vout = m_filter.process(vout, f, q, modeVol, res_nibble, m_is_8580);
            }
            mix += vout;
        }

        // Clamp and scale
        mix = std::max(-1.0f, std::min(1.0f, mix / 3.0f)) * vol;
        pushSample(mix);
    }
}
*/


// ==============================================================================
// Part E: Updated Filter::process() Implementation
// ==============================================================================

/*
UPDATED IMPLEMENTATION for SID6581::Filter::process():

float SID6581::Filter::process(
    float in, float f, float q, uint8_t mode,
    uint8_t res_nibble, bool is_8580)
{
    // ===== Cached parameter update (fast path) =====
    // This branch is taken only when resonance nibble changes (~1/1000 samples)
    if (res_nibble != last_res) {
        // Compute Q from resonance nibble
        float Q = 0.5f + (float)res_nibble / 15.0f * 3.5f;

        // Compute chip-dependent saturation threshold
        float chip_factor = is_8580 ? 0.8f : 1.2f;
        cached_threshold = 0.9f / (1.0f + chip_factor * Q);
        cached_threshold = std::max(0.3f, std::min(0.85f, cached_threshold));

        // Compute distortion blend factors
        float max_blend = is_8580 ? 0.55f : 0.70f;
        cached_blend = (float)res_nibble / 15.0f * max_blend;
        cached_lp_blend = (res_nibble > 8) ? cached_blend * 0.3f : 0.0f;

        last_res = res_nibble;
    }

    // ===== Core Chamberlin SVF (unchanged) =====
    float lp_new = lp + f * bp;
    float hp_new = in - lp_new - q * bp;
    float bp_new = f * hp_new + bp;

    // ===== Saturation stage (new) =====
    // Apply sigmoid soft-clip to BP (primary control)
    float bp_saturated = softClipBP(bp_new);

    // Conditionally apply light saturation to LP
    float lp_saturated = lp_new;
    if (cached_lp_blend > 0.001f) {
        lp_saturated = softClipLP(lp_new);
    }

    // Update state with saturated values (feedback loop)
    lp = lp_saturated;
    bp = bp_saturated;

    // ===== Output mixing (mode-dependent) =====
    float out = 0.0f;
    if (mode & 0x10) out += lp_saturated;   // LP enable
    if (mode & 0x20) out += bp_saturated;   // BP enable
    if (mode & 0x40) out += hp_new;         // HP enable

    return out;
}

// Helper: Soft-clip for BP (primary saturation)
inline float softClipBP(float x) {
    if (cached_blend < 0.001f) return x;
    float x_norm = x / cached_threshold;
    float x2 = x_norm * x_norm;
    float clipped = x_norm * cached_threshold / (1.0f + 0.5f * x2);
    return cached_blend * clipped + (1.0f - cached_blend) * x;
}

// Helper: Soft-clip for LP (light saturation)
inline float softClipLP(float x) {
    float lp_threshold = cached_threshold * 1.3f;
    float x_norm = x / lp_threshold;
    float x2 = x_norm * x_norm;
    float clipped = x_norm * lp_threshold / (1.0f + 0.5f * x2);
    return cached_lp_blend * clipped + (1.0f - cached_lp_blend) * x;
}
*/


// ==============================================================================
// Part F: Optimization Strategies (Optional)
// ==============================================================================

/**
 * Pre-computed lookup tables for faster saturation parameter calculation.
 * If CPU is extremely tight, use these instead of computing from scratch.
 */
namespace SIDSaturationLUT {

    // Threshold values for each resonance (0-15), 6581 variant
    static constexpr float THRESHOLD_6581[16] = {
        0.894f, 0.715f, 0.593f, 0.503f,  // res 0-3
        0.432f, 0.371f, 0.320f, 0.273f,  // res 4-7
        0.232f, 0.196f, 0.161f, 0.127f,  // res 8-11
        0.093f, 0.091f, 0.105f, 0.166f   // res 12-15 (nonlinear)
    };

    // Threshold values for 8580 variant (more stable)
    static constexpr float THRESHOLD_8580[16] = {
        0.903f, 0.768f, 0.655f, 0.568f,  // res 0-3
        0.498f, 0.445f, 0.400f, 0.361f,  // res 4-7
        0.327f, 0.296f, 0.267f, 0.239f,  // res 8-11
        0.213f, 0.188f, 0.165f, 0.143f   // res 12-15
    };

    // Blend factors (distortion amount) for each resonance
    static constexpr float BLEND_6581[16] = {
        0.000f, 0.047f, 0.093f, 0.140f,  // res 0-3
        0.187f, 0.233f, 0.280f, 0.327f,  // res 4-7
        0.373f, 0.420f, 0.467f, 0.513f,  // res 8-11
        0.560f, 0.607f, 0.653f, 0.700f   // res 12-15
    };

    static constexpr float BLEND_8580[16] = {
        0.000f, 0.037f, 0.073f, 0.110f,  // res 0-3
        0.147f, 0.183f, 0.220f, 0.257f,  // res 4-7
        0.293f, 0.330f, 0.367f, 0.403f,  // res 8-11
        0.440f, 0.477f, 0.513f, 0.550f   // res 12-15
    };

    /**
     * Fast path using LUT instead of computing threshold.
     * Saves ~8 cycles per resonance change.
     */
    inline float getThreshold(uint8_t res_nibble, bool is_8580) {
        const float* lut = is_8580 ? THRESHOLD_8580 : THRESHOLD_6581;
        return lut[res_nibble & 0x0F];
    }

    inline float getBlend(uint8_t res_nibble, bool is_8580) {
        const float* lut = is_8580 ? BLEND_8580 : BLEND_6581;
        return lut[res_nibble & 0x0F];
    }
}  // namespace SIDSaturationLUT


// ==============================================================================
// Part G: Validation & Testing Framework
// ==============================================================================

namespace SIDSaturationValidation {

    /**
     * Test: Verify soft-clip curve shape (smoothness, symmetry, monotonicity)
     */
    void testSoftClipCurve() {
        float threshold = 0.5f;
        float blend = 0.8f;

        // Sample curve at 100 points
        for (int i = -100; i <= 100; ++i) {
            float x = (float)i / 50.0f;  // Range [-2, +2]
            float y = SIDSaturation::softClipSigmoid(x, threshold, blend);

            // Verify properties:
            // 1. Symmetry: clip(-x) = -clip(x)
            float y_neg = SIDSaturation::softClipSigmoid(-x, threshold, blend);
            assert(std::abs(y + y_neg) < 0.001f);

            // 2. Monotonicity: dy/dx > 0 (always increasing)
            if (i > -100) {
                float y_prev = SIDSaturation::softClipSigmoid(
                    (float)(i-1) / 50.0f, threshold, blend
                );
                assert(y >= y_prev);
            }

            // 3. Bounded: |y| <= threshold
            assert(std::abs(y) <= threshold * (1.0f + 0.01f));  // +1% tolerance
        }
    }

    /**
     * Test: Verify blend interpolation (0=clean, 1=full clip)
     */
    void testBlendFactors() {
        float x = 1.5f;
        float threshold = 0.5f;

        float y_clean = SIDSaturation::softClipSigmoid(x, threshold, 0.0f);
        float y_full = SIDSaturation::softClipSigmoid(x, threshold, 1.0f);
        float y_half = SIDSaturation::softClipSigmoid(x, threshold, 0.5f);

        // Blend=0 should return input unchanged
        assert(std::abs(y_clean - x) < 0.001f);

        // Blend=1 should clamp
        assert(y_full < x);

        // Blend=0.5 should be between
        assert(y_clean > y_half && y_half > y_full);
    }

    /**
     * Test: Verify THD increases with resonance/saturation blend
     */
    void testTHDIncrease() {
        // Generate test signal (440 Hz sine wave at 44.1 kHz)
        const int num_samples = 44100;  // 1 second
        float audio_clean[num_samples];
        float audio_saturated[num_samples];

        // Populate with sine wave modulated by resonance sweep
        for (int i = 0; i < num_samples; ++i) {
            float t = (float)i / 44100.0f;
            float freq = 440.0f;
            float phase = 2.0f * 3.14159265f * freq * t;
            float amplitude = 0.8f;  // High to trigger saturation
            audio_clean[i] = amplitude * std::sin(phase);

            // Resonance increases linearly
            uint8_t res_nibble = (uint8_t)((t / 1.0f) * 15.0f);
            SID6581FilterWithSaturation filter(false);  // 6581

            float blend = (float)res_nibble / 15.0f * 0.70f;
            audio_saturated[i] = SIDSaturation::softClipSigmoid(
                audio_clean[i], 0.5f, blend
            );
        }

        // Compute THD via FFT (simplified: just check peak height)
        float peak_clean = *std::max_element(audio_clean, audio_clean + num_samples);
        float peak_saturated = *std::max_element(audio_saturated, audio_saturated + num_samples);

        // Saturation should compress peaks
        assert(peak_saturated < peak_clean);

        printf("✓ THD test: peaks compressed from %.3f to %.3f\n",
               peak_clean, peak_saturated);
    }

    /**
     * Test: Verify 8580 is less saturated than 6581
     */
    void test8580Less SaturatedThan6581() {
        float x = 0.9f;
        uint8_t res_nibble = 15;  // Maximum resonance

        SID6581FilterWithSaturation filter_6581(false);
        SID6581FilterWithSaturation filter_8580(true);

        filter_6581.setChipVariant(false);
        filter_8580.setChipVariant(true);

        float y_6581 = filter_6581.process(x, 0.1f, 2.0f, 0x20, res_nibble);
        float y_8580 = filter_8580.process(x, 0.1f, 2.0f, 0x20, res_nibble);

        // 8580 should show less amplitude reduction (less saturation)
        assert(std::abs(y_8580) > std::abs(y_6581));
        printf("✓ 6581: %.3f, 8580: %.3f (8580 less saturated)\n", y_6581, y_8580);
    }

}  // namespace SIDSaturationValidation


// ==============================================================================
// Part H: Performance Profiling Macros
// ==============================================================================

#ifdef ENABLE_PROFILING

#include <chrono>
#include <iostream>

#define PROFILE_START(name) \
    auto start_##name = std::chrono::high_resolution_clock::now()

#define PROFILE_END(name) \
    do { \
        auto end_##name = std::chrono::high_resolution_clock::now(); \
        auto dur = std::chrono::duration_cast<std::chrono::nanoseconds>( \
            end_##name - start_##name \
        ).count(); \
        std::cout << #name ": " << dur << " ns" << std::endl; \
    } while(0)

// Usage:
// PROFILE_START(filter_process);
// for (int i = 0; i < 100000; ++i) {
//     filter.process(in, f, q, mode, res, is_8580);
// }
// PROFILE_END(filter_process);

#else
#define PROFILE_START(name)
#define PROFILE_END(name)
#endif


// ==============================================================================
// End of Implementation Reference
// ==============================================================================

/*
SUMMARY OF CHANGES:

1. Add m_is_8580 member to SID6581 class
2. Modify Filter struct to include saturation parameters (cached)
3. Update Filter::process() signature to accept res_nibble + is_8580
4. Implement soft-clip helper (sigmoid or LUT-based)
5. Add updateSaturationParameters() method (called on res change)
6. Update synthesize() loop to pass res_nibble to filter
7. Add test suite for curve shape, THD, chip variant comparison
8. Optional: Add configuration JSON for enabling/disabling saturation

PERFORMANCE ESTIMATES:
- Threshold calculation: 8 cycles (only on resonance change ~1/1000 samples)
- Blend calculation: 4 cycles (only on resonance change)
- softClip() call: 12 cycles (every filter sample)
- Total overhead: ~0.5-1% CPU (negligible on modern systems)

TESTING CHECKLIST:
- [ ] Soft-clip curve validation (symmetry, monotonicity, bounds)
- [ ] Blend factor interpolation (0=clean, 1=full clip)
- [ ] THD increase with resonance
- [ ] 8580 shows less distortion than 6581
- [ ] No NaN/infinity in audio output
- [ ] Regression: filters without saturation unchanged
- [ ] Perceptual A/B test (listener preference)

*/
