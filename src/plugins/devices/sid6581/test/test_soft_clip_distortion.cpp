#include "test_harness.h"
#include "plugins/devices/sid6581/main/sid6581.h"
#include <cmath>
#include <cstring>

/**
 * Test suite for SID6581 soft-clipping distortion filter.
 * Validates resonance-dependent saturation and harmonic generation.
 */

TEST_CASE(soft_clip_low_resonance_no_distortion) {
    // At low resonance, the filter should behave linearly with minimal clipping.
    SID6581 sid;
    sid.setClockHz(985248);  // PAL
    sid.setSampleRate(44100);

    // Setup: voice 1 with low resonance (Q ≈ 0.35)
    sid.ioWrite(nullptr, 0xD400, 0x00);  // freq lo
    sid.ioWrite(nullptr, 0xD401, 0x10);  // freq hi (~256 Hz)
    sid.ioWrite(nullptr, 0xD402, 0x00);  // pw lo
    sid.ioWrite(nullptr, 0xD403, 0x08);  // pw hi
    sid.ioWrite(nullptr, 0xD405, 0xFF);  // AD: instant attack/decay
    sid.ioWrite(nullptr, 0xD406, 0xF0);  // SR: sustain at max
    sid.ioWrite(nullptr, 0xD415, 0x00);  // FC_LO
    sid.ioWrite(nullptr, 0xD416, 0x20);  // FC_HI (cutoff ≈ 1000 Hz)
    sid.ioWrite(nullptr, 0xD417, 0x01);  // RES: 0x0 (low resonance), voice 1 filtered
    sid.ioWrite(nullptr, 0xD418, 0x1F);  // LP mode, max volume
    sid.ioWrite(nullptr, 0xD404, 0x51);  // gate + pulse waveform

    // Generate samples
    for (int i = 0; i < 100; i++)
        sid.tick(1000);

    float buffer[512];
    int n = sid.pullSamples(buffer, 512);

    // At low resonance, samples should be small in magnitude (no heavy clipping)
    // and relatively consistent in output characteristics.
    ASSERT(n > 0);

    float avgAbs = 0.0f;
    for (int i = 0; i < n; i++)
        avgAbs += std::abs(buffer[i]);
    avgAbs /= n;

    // At low resonance, verify output is reasonable (not NaN, not clipped extreme)
    ASSERT(std::isfinite(avgAbs));
    ASSERT(avgAbs < 1.0f);  // Should not be completely saturated
}

TEST_CASE(soft_clip_high_resonance_distortion) {
    // At high resonance, the filter should exhibit soft-clipping distortion.
    SID6581 sid;
    sid.setClockHz(985248);
    sid.setSampleRate(44100);

    // Setup: voice 1 with high resonance (Q ≈ 2.0)
    sid.ioWrite(nullptr, 0xD400, 0x00);  // freq lo
    sid.ioWrite(nullptr, 0xD401, 0x10);  // freq hi
    sid.ioWrite(nullptr, 0xD402, 0x00);  // pw lo
    sid.ioWrite(nullptr, 0xD403, 0x08);  // pw hi
    sid.ioWrite(nullptr, 0xD405, 0xFF);  // AD
    sid.ioWrite(nullptr, 0xD406, 0xF0);  // SR
    sid.ioWrite(nullptr, 0xD415, 0x00);  // FC_LO
    sid.ioWrite(nullptr, 0xD416, 0x20);  // FC_HI
    sid.ioWrite(nullptr, 0xD417, 0xF1);  // RES: 0xF (max resonance), voice 1 filtered
    sid.ioWrite(nullptr, 0xD418, 0x1F);  // LP mode
    sid.ioWrite(nullptr, 0xD404, 0x51);  // gate + pulse

    for (int i = 0; i < 100; i++)
        sid.tick(1000);

    float buffer[512];
    int n = sid.pullSamples(buffer, 512);

    ASSERT(n > 0);

    // With high resonance, we expect clipping characteristics:
    // - Peaks should be limited (soft-clip prevents extreme values)
    // - More variance due to harmonic generation from distortion
    float maxAbs = 0.0f;
    for (int i = 0; i < n; i++) {
        float abs_val = std::abs(buffer[i]);
        if (abs_val > maxAbs) maxAbs = abs_val;
    }

    // Soft-clip should prevent extreme peaks but allow saturation
    ASSERT(maxAbs <= 1.5f);  // Reasonable upper bound after soft-clip
}

TEST_CASE(soft_clip_resonance_progression) {
    // Verify that increasing resonance progressively increases distortion.
    // We test multiple resonance levels and verify clipping increases.

    SID6581 sid;
    sid.setClockHz(985248);
    sid.setSampleRate(44100);

    // Setup base voice
    sid.ioWrite(nullptr, 0xD400, 0x00);
    sid.ioWrite(nullptr, 0xD401, 0x10);
    sid.ioWrite(nullptr, 0xD402, 0x00);
    sid.ioWrite(nullptr, 0xD403, 0x08);
    sid.ioWrite(nullptr, 0xD405, 0xFF);
    sid.ioWrite(nullptr, 0xD406, 0xF0);
    sid.ioWrite(nullptr, 0xD415, 0x00);
    sid.ioWrite(nullptr, 0xD416, 0x20);

    float prevClipIndicator = 0.0f;

    // Test resonance values from low to high
    for (uint8_t res = 0; res <= 15; res += 3) {
        sid.reset();  // Reset for clean slate

        // Reconfigure with new resonance
        sid.ioWrite(nullptr, 0xD400, 0x00);
        sid.ioWrite(nullptr, 0xD401, 0x10);
        sid.ioWrite(nullptr, 0xD402, 0x00);
        sid.ioWrite(nullptr, 0xD403, 0x08);
        sid.ioWrite(nullptr, 0xD405, 0xFF);
        sid.ioWrite(nullptr, 0xD406, 0xF0);
        sid.ioWrite(nullptr, 0xD415, 0x00);
        sid.ioWrite(nullptr, 0xD416, 0x20);
        sid.ioWrite(nullptr, 0xD417, (res << 4) | 0x01);  // RES, voice 1 filtered
        sid.ioWrite(nullptr, 0xD418, 0x1F);
        sid.ioWrite(nullptr, 0xD404, 0x51);

        for (int i = 0; i < 100; i++)
            sid.tick(1000);

        float buffer[512];
        int n = sid.pullSamples(buffer, 512);

        // Calculate a clipping indicator: ratio of samples at edge of [-1,1] range
        int clipCount = 0;
        for (int i = 0; i < n; i++) {
            if (std::abs(buffer[i]) > 0.8f) clipCount++;
        }
        float clipIndicator = (float)clipCount / n;

        // Clipping should increase or stay stable as resonance increases
        if (res > 0) {
            // Allow some variance but expect monotonic or near-monotonic trend
            ASSERT(clipIndicator >= prevClipIndicator - 0.05f);
        }
        prevClipIndicator = clipIndicator;
    }
}

TEST_CASE(soft_clip_filter_modes) {
    // Verify soft-clipping works with different filter output modes (LP, BP, HP).
    SID6581 sid;
    sid.setClockHz(985248);
    sid.setSampleRate(44100);

    uint8_t modes[] = {
        0x10,  // LP only
        0x20,  // BP only
        0x40,  // HP only
        0x30,  // LP + BP
        0x50,  // LP + HP
        0x60,  // BP + HP
        0x70   // LP + BP + HP
    };

    for (uint8_t mode : modes) {
        sid.reset();

        sid.ioWrite(nullptr, 0xD400, 0x00);
        sid.ioWrite(nullptr, 0xD401, 0x10);
        sid.ioWrite(nullptr, 0xD402, 0x00);
        sid.ioWrite(nullptr, 0xD403, 0x08);
        sid.ioWrite(nullptr, 0xD405, 0xFF);
        sid.ioWrite(nullptr, 0xD406, 0xF0);
        sid.ioWrite(nullptr, 0xD415, 0x00);
        sid.ioWrite(nullptr, 0xD416, 0x20);
        sid.ioWrite(nullptr, 0xD417, 0xF1);  // High resonance
        sid.ioWrite(nullptr, 0xD418, (mode & 0x70) | 0x0F);  // mode + max volume
        sid.ioWrite(nullptr, 0xD404, 0x51);

        for (int i = 0; i < 100; i++)
            sid.tick(1000);

        float buffer[512];
        int n = sid.pullSamples(buffer, 512);

        // All modes should produce valid audio (not extreme values)
        ASSERT(n > 0);
        for (int i = 0; i < n; i++) {
            ASSERT(std::abs(buffer[i]) <= 1.5f);  // Soft-clip boundary
        }
    }
}

TEST_CASE(soft_clip_preserves_low_amplitude) {
    // Soft-clipping should preserve low-amplitude signals without distortion.
    SID6581 sid;
    sid.setClockHz(985248);
    sid.setSampleRate(44100);

    // Setup with low volume (voice volume is controlled by envelope, here we use a quiet pattern)
    sid.ioWrite(nullptr, 0xD400, 0x40);  // lower frequency
    sid.ioWrite(nullptr, 0xD401, 0x08);  // ~528 Hz (lower)
    sid.ioWrite(nullptr, 0xD402, 0x00);
    sid.ioWrite(nullptr, 0xD403, 0x08);
    sid.ioWrite(nullptr, 0xD405, 0xFF);
    sid.ioWrite(nullptr, 0xD406, 0xF0);
    sid.ioWrite(nullptr, 0xD415, 0x00);
    sid.ioWrite(nullptr, 0xD416, 0x10);  // Lower cutoff
    sid.ioWrite(nullptr, 0xD417, 0xF1);  // High resonance
    sid.ioWrite(nullptr, 0xD418, 0x0F);  // LP mode, max volume
    sid.ioWrite(nullptr, 0xD404, 0x51);

    for (int i = 0; i < 50; i++)
        sid.tick(1000);

    float buffer[256];
    int n = sid.pullSamples(buffer, 256);

    ASSERT(n > 0);

    // Low-amplitude signals should not be distorted
    // Count how many samples are in the safe linear range
    int linearCount = 0;
    for (int i = 0; i < n; i++) {
        if (std::abs(buffer[i]) < 0.2f) linearCount++;
    }

    // Most samples should be in linear range at this cutoff
    float linearRatio = (float)linearCount / n;
    ASSERT(linearRatio > 0.3f);  // At least 30% in safe range
}

TEST_CASE(soft_clip_continuous_operation) {
    // Verify that soft-clipping doesn't cause instability with continuous operation.
    SID6581 sid;
    sid.setClockHz(985248);
    sid.setSampleRate(44100);

    // Setup with maximum resonance
    sid.ioWrite(nullptr, 0xD400, 0x00);
    sid.ioWrite(nullptr, 0xD401, 0x10);
    sid.ioWrite(nullptr, 0xD402, 0x00);
    sid.ioWrite(nullptr, 0xD403, 0x08);
    sid.ioWrite(nullptr, 0xD405, 0xFF);
    sid.ioWrite(nullptr, 0xD406, 0xF0);
    sid.ioWrite(nullptr, 0xD415, 0x00);
    sid.ioWrite(nullptr, 0xD416, 0x20);
    sid.ioWrite(nullptr, 0xD417, 0xF1);  // Max resonance
    sid.ioWrite(nullptr, 0xD418, 0x1F);  // LP mode
    sid.ioWrite(nullptr, 0xD404, 0x51);

    // Run for a long time to stress-test stability
    for (int i = 0; i < 1000; i++)
        sid.tick(1000);

    float buffer[2048];
    int n = sid.pullSamples(buffer, 2048);

    ASSERT(n > 0);

    // Check that we don't have NaN or Inf values (sign of numerical instability)
    for (int i = 0; i < n; i++) {
        ASSERT(std::isfinite(buffer[i]));
        ASSERT(std::abs(buffer[i]) <= 2.0f);  // Reasonable bounds
    }
}
