#pragma once

#include <string>
#include <vector>
#include <cstdint>

/**
 * SID Test Program Generator
 *
 * Generates minimal 6502 assembly programs for SID filter validation.
 * Programs exercise specific filter characteristics:
 * - Resonance sweep (0→15)
 * - Cutoff frequency sweep (100Hz→12kHz)
 * - Combined waveforms
 * - High-resonance saturation
 */

class SIDTestProgramGenerator {
public:
    /// Test program types
    enum class TestType {
        ResonanceSweep6581,      // Resonance 0→15 at fixed cutoff
        ResonanceSweep8580,      // Same, but for 8580 variant
        CutoffSweep6581,         // Cutoff sweep at fixed resonance
        CutoffSweep8580,         // Same, but for 8580 variant
        CombinedWaveforms,       // Test triangle+pulse, saw+pulse, etc.
        HighResonanceSaturation, // Max resonance distortion
    };

    /// Generate assembly source code for a test program
    static std::string generateAssembly(TestType type);

    /// Generate binary from assembly (requires ca65 assembler)
    /// Returns empty vector if ca65 not available or assembly fails
    static std::vector<uint8_t> generateBinary(TestType type, const std::string& ca65Path = "ca65");

    /// Get expected result memory address where test writes data
    static uint32_t getResultAddress(TestType type);

    /// Get expected result size in bytes
    static uint32_t getResultSize(TestType type);

private:
    // Assembly code generators for each test type
    static std::string generateResonanceSweepAssembly(bool is8580);
    static std::string generateCutoffSweepAssembly(bool is8580);
    static std::string generateCombinedWaveformsAssembly();
    static std::string generateHighResonanceSaturationAssembly();

    // Helper: Generate SID register write instructions
    static std::string genSIDWrite(uint8_t reg, uint8_t value);

    // Helper: Generate frequency sweep loop
    static std::string genFrequencySweep(uint16_t startHz, uint16_t endHz, uint16_t steps);

    // Helper: Capture output samples at memory address
    static std::string genCaptureOutput(uint32_t addr, uint16_t sampleCount);
};
