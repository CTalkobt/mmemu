#include "test_harness.h"
#include "plugins/devices/sid6581/main/spectral_analyzer.h"
#include <cmath>

/**
 * Unit tests for SpectralAnalyzer
 * Tests frequency-domain analysis and comparison functionality
 */

TEST_CASE(spectral_analyzer_creation) {
    SpectralAnalyzer analyzer(44100.0f);
    ASSERT(true);  // Just verify it constructs
}

TEST_CASE(spectral_analyzer_empty_input) {
    SpectralAnalyzer analyzer(44100.0f);
    std::vector<float> empty;

    auto result = analyzer.analyze(empty);
    ASSERT(!result.success);
    ASSERT(!result.errorMsg.empty());
}

TEST_CASE(spectral_analyzer_sine_wave) {
    // Generate a 440 Hz sine wave at 44100 Hz sample rate
    SpectralAnalyzer analyzer(44100.0f);
    std::vector<float> samples;
    int numSamples = 4410;  // 0.1 second
    float frequency = 440.0f;

    for (int i = 0; i < numSamples; ++i) {
        float t = (float)i / 44100.0f;
        float sample = std::sin(2.0f * M_PI * frequency * t);
        samples.push_back(sample);
    }

    auto result = analyzer.analyze(samples);
    ASSERT(result.success);
    ASSERT(!result.peaks.empty());
    ASSERT(result.fundamentalFreq > 0.0f);

    // Fundamental should be close to 440 Hz (within ~20 Hz due to FFT resolution)
    ASSERT(std::abs(result.fundamentalFreq - 440.0f) < 20.0f);
}

TEST_CASE(spectral_analyzer_dc_offset) {
    // Signal with DC offset should still detect AC component
    SpectralAnalyzer analyzer(44100.0f);
    std::vector<float> samples;
    int numSamples = 4410;
    float frequency = 1000.0f;
    float dcOffset = 0.5f;

    for (int i = 0; i < numSamples; ++i) {
        float t = (float)i / 44100.0f;
        float sample = dcOffset + 0.5f * std::sin(2.0f * M_PI * frequency * t);
        samples.push_back(sample);
    }

    auto result = analyzer.analyze(samples);
    ASSERT(result.success);
    ASSERT(!result.peaks.empty());
    ASSERT(result.fundamentalFreq > 0.0f);
}

TEST_CASE(spectral_analyzer_harmonic_detection) {
    // Signal with fundamental + harmonics
    SpectralAnalyzer analyzer(44100.0f);
    std::vector<float> samples;
    int numSamples = 4410;
    float fundamental = 440.0f;

    for (int i = 0; i < numSamples; ++i) {
        float t = (float)i / 44100.0f;
        float sample = std::sin(2.0f * M_PI * fundamental * t) +
                       0.3f * std::sin(2.0f * M_PI * fundamental * 2.0f * t) +
                       0.1f * std::sin(2.0f * M_PI * fundamental * 3.0f * t);
        samples.push_back(sample);
    }

    auto result = analyzer.analyze(samples);
    ASSERT(result.success);
    ASSERT(result.peaks.size() >= 2);  // Should detect at least fundamental and 2nd harmonic

    // First peak should be fundamental
    ASSERT(std::abs(result.peaks[0].frequency - fundamental) < 20.0f);

    // Check harmonic ratios
    ASSERT(result.harmonicRatio > 0.2f && result.harmonicRatio < 0.4f);  // ~0.3
}

TEST_CASE(spectral_analyzer_compare_identical_signals) {
    SpectralAnalyzer analyzer(44100.0f);
    std::vector<float> samples;
    int numSamples = 4410;
    float frequency = 1000.0f;

    for (int i = 0; i < numSamples; ++i) {
        float t = (float)i / 44100.0f;
        float sample = std::sin(2.0f * M_PI * frequency * t);
        samples.push_back(sample);
    }

    auto result1 = analyzer.analyze(samples);
    auto result2 = analyzer.analyze(samples);

    auto comparison = analyzer.compare(result1, result2, 5.0f, 0.1f);
    ASSERT(comparison.spectraMatch);
    ASSERT(comparison.spectralDistance < 0.1f);
}

TEST_CASE(spectral_analyzer_compare_different_frequencies) {
    SpectralAnalyzer analyzer(44100.0f);
    int numSamples = 4410;

    // Signal 1: 440 Hz
    std::vector<float> samples1;
    for (int i = 0; i < numSamples; ++i) {
        float t = (float)i / 44100.0f;
        float sample = std::sin(2.0f * M_PI * 440.0f * t);
        samples1.push_back(sample);
    }

    // Signal 2: 500 Hz (quite different)
    std::vector<float> samples2;
    for (int i = 0; i < numSamples; ++i) {
        float t = (float)i / 44100.0f;
        float sample = std::sin(2.0f * M_PI * 500.0f * t);
        samples2.push_back(sample);
    }

    auto result1 = analyzer.analyze(samples1);
    auto result2 = analyzer.analyze(samples2);

    auto comparison = analyzer.compare(result1, result2, 5.0f, 0.1f);
    ASSERT(!comparison.spectraMatch);  // Should not match
}

TEST_CASE(spectral_analyzer_byte_error_calculation) {
    // Test byte-level spectral error calculation
    std::vector<uint8_t> mmsim = {128, 127, 129, 128};
    std::vector<uint8_t> xemu = {128, 127, 129, 128};

    float error = SpectralAnalyzer::calculateSpectralError(mmsim, xemu);
    ASSERT(error < 0.01f);  // Perfect match should be near zero
}

TEST_CASE(spectral_analyzer_byte_error_with_difference) {
    // Test with some difference
    std::vector<uint8_t> mmsim = {200, 128, 200, 128};
    std::vector<uint8_t> xemu = {210, 128, 190, 128};

    float error = SpectralAnalyzer::calculateSpectralError(mmsim, xemu);
    ASSERT(error > 0.01f);
    ASSERT(error < 0.5f);
}

TEST_CASE(spectral_analyzer_byte_error_empty) {
    // Empty input should return 1.0
    std::vector<uint8_t> empty1, empty2;
    float error = SpectralAnalyzer::calculateSpectralError(empty1, empty2);
    ASSERT(error == 1.0f);
}

TEST_CASE(spectral_analyzer_byte_error_both_silent) {
    // Both all zero should be perfect match
    std::vector<uint8_t> silent(100, 0);
    std::vector<uint8_t> silent2(100, 0);

    float error = SpectralAnalyzer::calculateSpectralError(silent, silent2);
    ASSERT(error == 0.0f);
}

TEST_CASE(spectral_analyzer_resonance_characteristic) {
    // Generate a resonant signal (peaked frequency response)
    SpectralAnalyzer analyzer(44100.0f);
    std::vector<float> samples;
    int numSamples = 8820;  // 0.2 second
    float resonantFreq = 2000.0f;

    for (int i = 0; i < numSamples; ++i) {
        float t = (float)i / 44100.0f;
        // Simulate resonant response using damped oscillation
        float decay = std::exp(-t / 0.1f);
        float sample = decay * std::sin(2.0f * M_PI * resonantFreq * t);
        samples.push_back(sample);
    }

    auto result = analyzer.analyze(samples);
    ASSERT(result.success);
    ASSERT(result.resonancePeak > 0.0f);
    ASSERT(std::abs(result.peakFrequency - resonantFreq) < 100.0f);
}
