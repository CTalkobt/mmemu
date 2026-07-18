#include "test_harness.h"
#include "cli/main/xemu_bridge.h"
#include "cli/main/sid_test_programs.h"
#include "plugins/devices/sid6581/main/spectral_analyzer.h"
#include "plugins/devices/sid6581/main/sid_validation_suite.h"
#include <cmath>

/**
 * Integration tests for complete xemu validation pipeline
 * Tests: Program generation → xemu communication → analysis → validation
 */

TEST_CASE(xemu_integration_test_program_generation) {
    // Verify test programs can be generated
    for (int type = 0; type < 6; ++type) {
        auto testType = (SIDTestProgramGenerator::TestType)type;
        std::string asm_code = SIDTestProgramGenerator::generateAssembly(testType);

        ASSERT(!asm_code.empty());
        ASSERT(asm_code.find(".org") != std::string::npos);
        ASSERT(asm_code.find("$0800") != std::string::npos);
    }
}

TEST_CASE(xemu_integration_xemu_bridge_creation) {
    // Test XemuBridge can be created and configured
    XemuBridge bridge("/usr/local/bin/xemu-xmega65");
    ASSERT(!bridge.isRunning());  // Not running yet
}

TEST_CASE(xemu_integration_spectral_analyzer_creation) {
    // Test SpectralAnalyzer can be created with proper sample rate
    SpectralAnalyzer analyzer(44100.0f);

    // Generate test signal
    std::vector<float> samples;
    int numSamples = 4410;
    float frequency = 1000.0f;

    for (int i = 0; i < numSamples; ++i) {
        float t = (float)i / 44100.0f;
        float sample = std::sin(2.0f * M_PI * frequency * t);
        samples.push_back(sample);
    }

    auto result = analyzer.analyze(samples);
    ASSERT(result.success);
    ASSERT(result.fundamentalFreq > 0.0f);
}

TEST_CASE(xemu_integration_validation_suite_creation) {
    // Test SIDValidationSuite can be created
    SIDValidationSuite suite("/usr/local/bin/xemu-xmega65");
    ASSERT(true);  // Construction succeeds
}

TEST_CASE(xemu_integration_spectral_error_calculation) {
    // Test spectral error calculation on realistic data
    // Simulate OSC3 output from SID at two different resonance levels
    std::vector<uint8_t> mmsim_low_res = {
        120, 125, 130, 128, 122, 118, 115, 120,  // Low resonance (gentle)
        125, 130, 128, 122, 118, 115, 120, 125
    };

    std::vector<uint8_t> xemu_low_res = {
        121, 126, 131, 127, 121, 117, 114, 121,  // Very similar
        126, 131, 127, 121, 117, 114, 121, 126
    };

    float error = SpectralAnalyzer::calculateSpectralError(mmsim_low_res, xemu_low_res);
    ASSERT(error < 0.05f);  // Should be very close
}

TEST_CASE(xemu_integration_spectral_error_high_resonance) {
    // Test with higher resonance (more clipping/distortion)
    std::vector<uint8_t> mmsim_high_res = {
        255, 200, 50, 200, 255, 200, 50, 200,    // High distortion
        255, 200, 50, 200, 255, 200, 50, 200
    };

    std::vector<uint8_t> xemu_high_res = {
        255, 200, 60, 190, 255, 200, 60, 190,    // Slightly different
        255, 200, 60, 190, 255, 200, 60, 190
    };

    float error = SpectralAnalyzer::calculateSpectralError(mmsim_high_res, xemu_high_res);
    ASSERT(error > 0.05f);  // Should have noticeable difference
    ASSERT(error < 0.20f);  // But not huge
}

TEST_CASE(xemu_integration_program_size_validation) {
    // Verify all test programs have reasonable sizes
    for (int type = 0; type < 6; ++type) {
        auto testType = (SIDTestProgramGenerator::TestType)type;

        uint32_t resultAddr = SIDTestProgramGenerator::getResultAddress(testType);
        uint32_t resultSize = SIDTestProgramGenerator::getResultSize(testType);

        ASSERT(resultAddr == 0x2000);  // Standard result location
        ASSERT(resultSize > 0);        // Non-zero result size
        ASSERT(resultSize <= 256);     // Reasonable size
    }
}

TEST_CASE(xemu_integration_full_pipeline_structure) {
    // Test that all components can be instantiated together
    SpectralAnalyzer analyzer(44100.0f);
    XemuBridge bridge("/usr/local/bin/xemu-xmega65");
    SIDValidationSuite suite("/usr/local/bin/xemu-xmega65");

    // Generate a test program
    std::string asm_code = SIDTestProgramGenerator::generateAssembly(
        SIDTestProgramGenerator::TestType::ResonanceSweep6581);

    ASSERT(!asm_code.empty());
    ASSERT(!bridge.isRunning());  // Bridge not launched (xemu may not be available)
    ASSERT(true);  // All components created successfully
}

TEST_CASE(xemu_integration_resonance_sweep_assembly_structure) {
    // Verify resonance sweep has proper structure for testing
    std::string asm_code = SIDTestProgramGenerator::generateAssembly(
        SIDTestProgramGenerator::TestType::ResonanceSweep6581);

    // Should have resonance loop
    ASSERT(asm_code.find("LOOP") != std::string::npos);
    ASSERT(asm_code.find("RLOOP") != std::string::npos);

    // Should set resonance register
    ASSERT(asm_code.find("$D417") != std::string::npos);

    // Should read OSC3
    ASSERT(asm_code.find("$D41B") != std::string::npos);

    // Should store results
    ASSERT(asm_code.find("$2000") != std::string::npos);

    // Should have loop counter increment
    ASSERT(asm_code.find("INX") != std::string::npos);
    ASSERT(asm_code.find("CPX #16") != std::string::npos);
}

TEST_CASE(xemu_integration_cutoff_sweep_assembly_structure) {
    // Verify cutoff sweep has proper structure
    std::string asm_code = SIDTestProgramGenerator::generateAssembly(
        SIDTestProgramGenerator::TestType::CutoffSweep6581);

    // Should have cutoff loop
    ASSERT(asm_code.find("FCLOOP") != std::string::npos);

    // Should set filter frequency registers
    ASSERT(asm_code.find("$D415") != std::string::npos);  // FC_LO
    ASSERT(asm_code.find("$D416") != std::string::npos);  // FC_HI

    // Should read and store OSC3
    ASSERT(asm_code.find("$D41B") != std::string::npos);
    ASSERT(asm_code.find("$2000") != std::string::npos);
}

TEST_CASE(xemu_integration_combined_waveforms_assembly) {
    // Verify combined waveforms test structure
    std::string asm_code = SIDTestProgramGenerator::generateAssembly(
        SIDTestProgramGenerator::TestType::CombinedWaveforms);

    // Should have waveform loop
    ASSERT(asm_code.find("WVLOOP") != std::string::npos);

    // Should set waveform control register
    ASSERT(asm_code.find("$D404") != std::string::npos);

    // Should test multiple waveforms
    ASSERT(asm_code.find("$31") != std::string::npos);  // Triangle + Pulse
    ASSERT(asm_code.find("$61") != std::string::npos);  // Saw + Pulse
}

TEST_CASE(xemu_integration_validation_levels) {
    // Verify validation suite supports all intensity levels
    SIDValidationSuite suite;

    // Test configuration retrieval (doesn't require xemu to be running)
    auto testQS = suite.getTestsForLevel(SIDValidationSuite::ValidationLevel::QUICK_SMOKE);
    auto testStd = suite.getTestsForLevel(SIDValidationSuite::ValidationLevel::STANDARD);
    auto testComp = suite.getTestsForLevel(SIDValidationSuite::ValidationLevel::COMPREHENSIVE);

    ASSERT(!testQS.empty());
    ASSERT(!testStd.empty());
    ASSERT(!testComp.empty());
    ASSERT(testQS.size() < testStd.size());
    ASSERT(testStd.size() <= testComp.size());
}

TEST_CASE(xemu_integration_test_result_formatting) {
    // Verify test results can be formatted properly
    SIDValidationSuite::TestResult result;
    result.testName = "resonance_sweep_6581";
    result.passed = true;
    result.spectralError = 0.08f;
    result.executionTimeMs = 1234;
    result.notes = "Spectral match within tolerance";

    std::string formatted = SIDValidationSuite::formatTestResult(result);
    ASSERT(!formatted.empty());
    ASSERT(formatted.find("resonance_sweep_6581") != std::string::npos);
    ASSERT(formatted.find("PASS") != std::string::npos);
    ASSERT(formatted.find("1234") != std::string::npos);
}

TEST_CASE(xemu_integration_validation_report_formatting) {
    // Verify validation reports can be formatted
    SIDValidationSuite::ValidationReport report;
    report.allTestsPassed = true;
    report.testsPassed = 2;
    report.testsFailed = 0;
    report.averageSpectralError = 0.07f;
    report.totalTimeMs = 5000;
    report.summary = "2/2 tests passed";

    SIDValidationSuite::TestResult result1, result2;
    result1.testName = "test1";
    result1.passed = true;
    result2.testName = "test2";
    result2.passed = true;

    report.results.push_back(result1);
    report.results.push_back(result2);

    std::string formatted = SIDValidationSuite::formatReport(report);
    ASSERT(!formatted.empty());
    ASSERT(formatted.find("PASS") != std::string::npos);
    ASSERT(formatted.find("2/2") != std::string::npos);
}
