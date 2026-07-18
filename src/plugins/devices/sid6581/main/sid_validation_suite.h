#pragma once

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <cstdint>

class XemuBridge;
class SpectralAnalyzer;

/**
 * SIDValidationSuite: Complete xemu vs mmsim SID filter validation
 *
 * Orchestrates the full validation pipeline:
 * 1. Generate test programs (resonance sweep, cutoff sweep, etc.)
 * 2. Load into both mmsim and xemu
 * 3. Capture output and compare
 * 4. Report spectral matching results
 *
 * Three test intensity levels:
 * - QUICK_SMOKE: 2 basic resonance/cutoff tests (~1 minute)
 * - STANDARD: 4 tests covering all variants (~5 minutes)
 * - COMPREHENSIVE: 6 tests + multiple runs (~15 minutes)
 */

class SIDValidationSuite {
public:
    /// Test intensity levels
    enum class ValidationLevel {
        QUICK_SMOKE,      // Basic sanity check (2 tests)
        STANDARD,         // Full basic coverage (4 tests)
        COMPREHENSIVE     // Exhaustive validation (6 tests)
    };

    /// Result for a single validation test
    struct TestResult {
        std::string testName;
        bool passed = false;
        float spectralError = 1.0f;       // 0-1, lower is better
        float peakFreqError = 0.0f;       // Hz
        float resonancePeakError = 0.0f;  // dB
        std::string notes;
        uint32_t executionTimeMs = 0;
    };

    /// Overall validation report
    struct ValidationReport {
        bool allTestsPassed = false;
        int testsPassed = 0;
        int testsFailed = 0;
        std::vector<TestResult> results;
        float averageSpectralError = 0.0f;
        uint32_t totalTimeMs = 0;
        std::string summary;
    };

    /// Create validation suite with optional xemu path
    explicit SIDValidationSuite(const std::string& xemuPath = "/usr/local/bin/xemu-xmega65");

    /// Run validation suite at specified intensity
    /// Note: Requires functioning xemu, mmsim binary, and ca65 assembler
    ValidationReport runValidation(ValidationLevel level);

    /// Run specific test (for debugging)
    TestResult runSingleTest(const std::string& testName);

    /// Generate validation report as formatted string
    static std::string formatReport(const ValidationReport& report);

private:
    std::string m_xemuPath;
    std::unique_ptr<XemuBridge> m_xemuBridge;
    std::unique_ptr<SpectralAnalyzer> m_analyzer;

    /// Generate test configuration for given test type
    struct TestConfig {
        std::string testName;
        std::string programName;
        uint32_t programAddr = 0x0800;  // C64 load address
        uint32_t resultAddr = 0x2000;   // Result buffer address
        uint32_t resultSize = 256;      // Expected result size in bytes
        uint32_t timeoutMs = 10000;     // Max execution time
    };

    /// Get test configuration by name
    TestConfig getTestConfig(const std::string& testName);

    /// Run a single validation test
    TestResult executeTest(const TestConfig& config);

    /// Load program into both emulators
    bool loadProgram(const TestConfig& config, std::vector<uint8_t>& mmsimOutput,
                     std::vector<uint8_t>& xemuOutput);

    /// Get all tests for validation level
    std::vector<std::string> getTestsForLevel(ValidationLevel level);

    /// Format result for display
    static std::string formatTestResult(const TestResult& result);
};
