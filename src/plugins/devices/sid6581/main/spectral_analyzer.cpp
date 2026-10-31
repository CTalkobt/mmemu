#include "spectral_analyzer.h"
#include <cmath>
#include <complex>
#include <algorithm>
#include <cstring>
#include <iomanip>
#include <sstream>

SpectralAnalyzer::SpectralAnalyzer(float sampleRate)
    : m_sampleRate(sampleRate) {
}

SpectralAnalyzer::AnalysisResult SpectralAnalyzer::analyze(
    const std::vector<float>& samples, int fftSize) {
    AnalysisResult result;

    if (samples.empty() || fftSize <= 0) {
        result.errorMsg = "Invalid input: empty samples or zero FFT size";
        return result;
    }

    // Ensure FFT size is a power of 2 that covers every sample; a shorter
    // transform would wrap the excess samples around in time
    int minSize = std::max(fftSize, (int)samples.size());
    int actualFFTSize = 1;
    while (actualFFTSize < minSize) {
        actualFFTSize *= 2;
    }

    // Apply window function
    std::vector<float> windowed = applyWindow(samples);

    // Pad to FFT size if needed
    if (windowed.size() < (size_t)actualFFTSize) {
        windowed.resize(actualFFTSize, 0.0f);
    }

    // Compute magnitude spectrum
    std::vector<float> spectrum = computeFFT(windowed, actualFFTSize);

    // Detect peaks
    result.peaks = detectPeaks(spectrum, 0.05f);  // 5% magnitude threshold

    if (!result.peaks.empty()) {
        result.fundamentalFreq = result.peaks[0].frequency;
        result.resonancePeak = result.peaks[0].magnitude;
        result.peakFrequency = result.peaks[0].frequency;

        // Identify harmonics
        identifyHarmonics(result.peaks, result.fundamentalFreq);

        // Calculate harmonic ratios for soft-clipping detection
        for (const auto& peak : result.peaks) {
            if (peak.harmonicNumber == 2 && result.harmonicRatio == 0.0f)
                result.harmonicRatio = peak.magnitude / result.peaks[0].magnitude;
            else if (peak.harmonicNumber == 3 && result.thirdHarmonicRatio == 0.0f)
                result.thirdHarmonicRatio = peak.magnitude / result.peaks[0].magnitude;
        }
    }

    // Calculate total energy
    for (float mag : spectrum) {
        result.totalEnergy += mag;
    }

    // Analyze filter characteristics
    analyzeFilterCharacteristics(spectrum, result);

    result.success = true;
    return result;
}

SpectralAnalyzer::ComparisonResult SpectralAnalyzer::compare(
    const AnalysisResult& mmsim,
    const AnalysisResult& xemu,
    float frequencyTolerance,
    float magnitudeTolerance) {
    ComparisonResult result;

    if (!mmsim.success || !xemu.success) {
        result.spectraMatch = false;
        result.notes = "Analysis failed for one or both inputs";
        return result;
    }

    // Compare fundamental frequencies
    if (std::abs(mmsim.fundamentalFreq - xemu.fundamentalFreq) < frequencyTolerance) {
        result.peakFreqError = std::abs(mmsim.fundamentalFreq - xemu.fundamentalFreq);
    } else {
        result.spectraMatch = false;
        result.notes = "Fundamental frequency mismatch";
        return result;
    }

    // Compare resonance peak magnitudes (in dB)
    float mmsimPeakDb = 20.0f * std::log10(mmsim.resonancePeak + 1e-6f);
    float xemuPeakDb = 20.0f * std::log10(xemu.resonancePeak + 1e-6f);
    float peakErrorDb = std::abs(mmsimPeakDb - xemuPeakDb);

    if (peakErrorDb < magnitudeTolerance * 20.0f) {
        result.resonancePeakError = peakErrorDb;
    } else {
        result.spectraMatch = false;
        result.notes = "Resonance peak magnitude mismatch";
        return result;
    }

    // Calculate overall spectral distance
    float sumSquaredError = 0.0f;
    int peakCount = std::min(mmsim.peaks.size(), xemu.peaks.size());

    for (int i = 0; i < peakCount; ++i) {
        float freqError = std::abs(mmsim.peaks[i].frequency - xemu.peaks[i].frequency);
        float magError = std::abs(mmsim.peaks[i].magnitude - xemu.peaks[i].magnitude);
        sumSquaredError += freqError * freqError / (frequencyTolerance * frequencyTolerance) +
                          magError * magError / (magnitudeTolerance * magnitudeTolerance);
    }

    result.spectralDistance = std::sqrt(sumSquaredError / std::max(peakCount, 1));
    result.spectraMatch = (result.spectralDistance < 1.0f);

    return result;
}

float SpectralAnalyzer::calculateSpectralError(const std::vector<uint8_t>& mmsim,
                                              const std::vector<uint8_t>& xemu) {
    if (mmsim.size() != xemu.size() || mmsim.empty()) {
        return 1.0f;
    }

    float sumSquaredError = 0.0f;
    float maxVal = 0.0f;

    for (size_t i = 0; i < mmsim.size(); ++i) {
        float m = mmsim[i] / 255.0f;
        float x = xemu[i] / 255.0f;
        float error = m - x;

        sumSquaredError += error * error;
        maxVal = std::max(maxVal, x);
    }

    if (maxVal < 0.01f) {
        return 0.0f;  // Both silent
    }

    return std::sqrt(sumSquaredError / mmsim.size()) / maxVal;
}

std::vector<float> SpectralAnalyzer::applyWindow(const std::vector<float>& samples) {
    std::vector<float> windowed = samples;
    int n = windowed.size();

    // Hamming window: w[n] = 0.54 - 0.46*cos(2*pi*n/(N-1))
    for (int i = 0; i < n; ++i) {
        float window = 0.54f - 0.46f * std::cos(2.0f * M_PI * i / (n - 1));
        windowed[i] *= window;
    }

    return windowed;
}

std::vector<float> SpectralAnalyzer::computeFFT(const std::vector<float>& samples, int fftSize) {
    // Iterative radix-2 Cooley-Tukey; fftSize is a power of 2 >= samples.size()
    std::vector<std::complex<double>> data(fftSize);
    for (int n = 0; n < (int)samples.size() && n < fftSize; ++n) {
        data[n] = samples[n];
    }

    for (int i = 1, j = 0; i < fftSize; ++i) {
        int bit = fftSize >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(data[i], data[j]);
    }

    for (int len = 2; len <= fftSize; len <<= 1) {
        std::complex<double> step = std::polar(1.0, -2.0 * M_PI / len);
        for (int start = 0; start < fftSize; start += len) {
            std::complex<double> w(1.0, 0.0);
            for (int k = 0; k < len / 2; ++k) {
                std::complex<double> even = data[start + k];
                std::complex<double> odd = data[start + k + len / 2] * w;
                data[start + k] = even + odd;
                data[start + k + len / 2] = even - odd;
                w *= step;
            }
        }
    }

    std::vector<float> spectrum(fftSize / 2 + 1);
    for (int k = 0; k <= fftSize / 2; ++k) {
        spectrum[k] = (float)(std::abs(data[k]) / fftSize);
    }

    return spectrum;
}

std::vector<SpectralAnalyzer::SpectralPeak> SpectralAnalyzer::detectPeaks(
    const std::vector<float>& spectrum, float threshold) {
    std::vector<SpectralPeak> peaks;

    int n = spectrum.size();
    int fftSize = (n - 1) * 2;
    float maxMag = *std::max_element(spectrum.begin(), spectrum.end());
    float actualThreshold = std::max(threshold * maxMag, 0.01f);

    // Simple peak detection: local maxima above threshold
    float maxPeak = 0.0f;
    for (int i = 1; i < n - 1; ++i) {
        if (spectrum[i] > actualThreshold &&
            spectrum[i] > spectrum[i - 1] &&
            spectrum[i] > spectrum[i + 1]) {
            // Fit a parabola through the log magnitudes of the peak bin and its
            // neighbours to recover the true frequency and magnitude between bins
            float a = std::log(spectrum[i - 1] + 1e-12f);
            float b = std::log(spectrum[i]);
            float c = std::log(spectrum[i + 1] + 1e-12f);
            float denom = a - 2.0f * b + c;
            float offset = (denom != 0.0f) ? 0.5f * (a - c) / denom : 0.0f;

            SpectralPeak peak;
            peak.frequency = ((float)i + offset) * m_sampleRate / fftSize;
            peak.magnitude = std::exp(b - 0.25f * (a - c) * offset);
            peak.harmonicNumber = 0;  // Will be set by identifyHarmonics
            maxPeak = std::max(maxPeak, peak.magnitude);
            peaks.push_back(peak);
        }
    }

    for (auto& peak : peaks) {
        peak.magnitude /= (maxPeak + 1e-6f);  // Normalize
    }

    // Sort by magnitude (descending)
    std::sort(peaks.begin(), peaks.end(),
              [](const SpectralPeak& a, const SpectralPeak& b) {
                  return a.magnitude > b.magnitude;
              });

    // Keep top peaks only
    if (peaks.size() > 10) {
        peaks.resize(10);
    }

    return peaks;
}

void SpectralAnalyzer::identifyHarmonics(std::vector<SpectralPeak>& peaks,
                                        float fundamentalFreq) {
    if (peaks.empty()) {
        return;
    }

    peaks[0].harmonicNumber = 1;

    // Identify harmonics (2F, 3F, 4F, etc.) within tolerance
    float harmonyTolerance = fundamentalFreq * 0.05f;  // 5% tolerance

    for (size_t i = 1; i < peaks.size(); ++i) {
        for (int h = 2; h <= 10; ++h) {
            float harmonicFreq = fundamentalFreq * h;
            if (std::abs(peaks[i].frequency - harmonicFreq) < harmonyTolerance) {
                peaks[i].harmonicNumber = h;
                break;
            }
        }

        if (peaks[i].harmonicNumber == 0) {
            // Not a harmonic of fundamental
            peaks[i].harmonicNumber = -1;
        }
    }
}

void SpectralAnalyzer::analyzeFilterCharacteristics(const std::vector<float>& spectrum,
                                                   AnalysisResult& result) {
    if (spectrum.empty() || result.fundamentalFreq <= 0.0f) {
        return;
    }

    // Estimate Q-factor from bandwidth
    // Find -3dB points around peak
    float peakMag = result.resonancePeak;
    float halfPowerMag = peakMag / std::sqrt(2.0f);

    int lowBin = frequencyToBin(result.fundamentalFreq / 2.0f, (spectrum.size() - 1) * 2);
    int highBin = frequencyToBin(result.fundamentalFreq * 2.0f, (spectrum.size() - 1) * 2);

    lowBin = std::max(0, lowBin);
    highBin = std::min((int)spectrum.size() - 1, highBin);

    float lowEdge = 0.0f, highEdge = 0.0f;
    for (int i = lowBin; i < (int)spectrum.size(); ++i) {
        if (spectrum[i] >= halfPowerMag) {
            lowEdge = binToFrequency(i, (spectrum.size() - 1) * 2);
            break;
        }
    }

    for (int i = highBin; i >= 0; --i) {
        if (spectrum[i] >= halfPowerMag) {
            highEdge = binToFrequency(i, (spectrum.size() - 1) * 2);
            break;
        }
    }

    if (highEdge > lowEdge && lowEdge > 0.0f) {
        result.bandwidth = highEdge - lowEdge;
        result.resonancePeak = peakMag;  // Already set, but recalculate if needed
    }
}

float SpectralAnalyzer::binToFrequency(int bin, int fftSize) const {
    return (float)bin * m_sampleRate / fftSize;
}

int SpectralAnalyzer::frequencyToBin(float freq, int fftSize) const {
    return (int)(freq * fftSize / m_sampleRate + 0.5f);
}
