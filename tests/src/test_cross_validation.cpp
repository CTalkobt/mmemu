#include "../src/test_harness.h"
#include "cli/main/cross_validation_runner.h"
#include <iostream>

TEST_CASE(cross_validation_runner_create_emulator) {
    // Create a runner connecting to emulator
    auto runner = CrossValidationRunner::withEmulator("127.0.0.1", 6502);
    // Note: This will fail if no emulator is running, but verifies the API
    // In CI/testing, we only test the API structure, not actual connections
}

TEST_CASE(cross_validation_runner_create_hardware) {
    // Create a runner connecting to hardware (won't actually connect without device)
    // This verifies the API is available
    auto runner = CrossValidationRunner::withHardware("/dev/ttyUSB0", 2000000);
}

TEST_CASE(cross_validation_runner_create_both) {
    // Create a runner for cross-validation
    // Will connect to emulator if available, hardware port is optional
    auto runner = CrossValidationRunner::withBoth();
    // This verifies the factory methods exist
}

TEST_CASE(cross_validation_test_case_structure) {
    // Verify the TestCase structure can be created
    CrossValidationRunner::TestCase test;
    test.name = "arithmetic_test";
    test.programPath = "test_binary.bin";
    test.programAddr = 0x0800;
    test.resultAddr = 0x2000;
    test.resultSize = 256;
    test.timeoutMs = 5000;

    ASSERT_EQ(test.name, "arithmetic_test");
    ASSERT_EQ(test.programAddr, 0x0800);
    ASSERT_EQ(test.resultAddr, 0x2000);
    ASSERT_EQ(test.resultSize, 256);
}

TEST_CASE(cross_validation_comparison_result_structure) {
    // Verify ComparisonResult structure
    CrossValidationRunner::ComparisonResult result;
    result.testName = "test1";
    result.emulatorPass = true;
    result.hardwarePass = true;
    result.resultsMatch = true;

    ASSERT(result.overallPass());
}

TEST_CASE(cross_validation_comparison_partial_fail) {
    CrossValidationRunner::ComparisonResult result;
    result.testName = "test2";
    result.emulatorPass = true;
    result.hardwarePass = false;
    result.resultsMatch = false;

    ASSERT(!result.overallPass());
}

TEST_CASE(cross_validation_comparison_mismatch) {
    CrossValidationRunner::ComparisonResult result;
    result.testName = "test3";
    result.emulatorPass = true;
    result.hardwarePass = true;
    result.resultsMatch = false;

    ASSERT(!result.overallPass());
}
