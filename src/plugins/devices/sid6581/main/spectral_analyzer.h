#pragma once

#include <vector>
#include <cstdint>
#include <string>
#include <map>

/**
 * SpectralAnalyzer: Frequency-domain comparison for SID filter validation
 *
 * Analyzes audio output from emulators and identifies:
 * - Spectral peaks (fundamental frequencies)
 * - Harmonic series
 * - Spectral error metrics
 * - Resonance peak characteristics
 *
 * Used to compare mmsim vs xemu SID filter output without bit-exact matching.
 */

class SpectralAnalyzer {
public:
    /// Spectral peak information
    struct SpectralPeak {
        float frequency;     // Hz (estimated from bin)
        float magnitude;     // Normalized 0-1
        int harmonicNumber;  // 1 for fundamental, 2+ for harmonics
    };

    /// Complete spectral analysis result
    struct AnalysisResult {
        bool success = false;
        std::string errorMsg;

        // Frequency domain info
        std::vector<SpectralPeak> peaks;  // Dominant peaks in order
        float fundamentalFreq = 0.0f;     // Detected fundamental frequency
        float totalEnergy = 0.0f;         // Sum of all frequency bin magnitudes

        // Filter characteristic metrics
        float resonancePeak = 0.0f;       // Magnitude of filter resonance peak
        float peakFrequency = 0.0f;       // Frequency at peak magnitude
        float bandwidth = 0.0f;           // Q-factor derived bandwidth estimate

        // Soft-clipping detection
        float harmonicRatio = 0.0f;       // Ratio of 2nd harmonic to fundamental
        float thirdHarmonicRatio = 0.0f;  // Ratio of 3rd harmonic to fundamental
    };

    /// Comparison result between two spectra
    struct ComparisonResult {
        bool spectraMatch = false;
        float spectralDistance = 0.0f;    // Normalized difference 0-1
        float peakFreqError = 0.0f;       // Hz difference in fundamental
        float resonancePeakError = 0.0f;  // dB difference in filter peak
        std::string notes;
    };

    /// Create analyzer with sample rate
    explicit SpectralAnalyzer(float sampleRate = 44100.0f);

    /// Analyze audio samples (mono or mono-converted)
    AnalysisResult analyze(const std::vector<float>& samples, int fftSize = 2048);

    /// Compare two analysis results
    ComparisonResult compare(const AnalysisResult& mmsim,
                            const AnalysisResult& xemu,
                            float frequencyTolerance = 5.0f,  // Hz
                            float magnitudeTolerance = 0.1f);  // dB

    /// Simple peak magnitude comparison (for exact byte comparison)
    static float calculateSpectralError(const std::vector<uint8_t>& mmsim,
                                       const std::vector<uint8_t>& xemu);

private:
    float m_sampleRate;

    /// Apply Hamming window to samples
    std::vector<float> applyWindow(const std::vector<float>& samples);

    /// Simple FFT using DFT for bin analysis
    std::vector<float> computeFFT(const std::vector<float>& samples, int fftSize);

    /// Find spectral peaks above threshold
    std::vector<SpectralPeak> detectPeaks(const std::vector<float>& spectrum,
                                         float threshold = 0.1f);

    /// Identify harmonics related to fundamental
    void identifyHarmonics(std::vector<SpectralPeak>& peaks, float fundamentalFreq);

    /// Estimate filter characteristics from spectrum
    void analyzeFilterCharacteristics(const std::vector<float>& spectrum,
                                     AnalysisResult& result);

    /// Convert bin index to frequency
    float binToFrequency(int bin, int fftSize) const;

    /// Convert frequency to bin index
    int frequencyToBin(float freq, int fftSize) const;
};
