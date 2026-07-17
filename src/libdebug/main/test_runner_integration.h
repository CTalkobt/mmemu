#pragma once

#include "test_persistence.h"
#include <string>
#include <chrono>
#include <memory>

/**
 * Test Runner Integration
 * Provides utilities for integrating TestPersistence with catch2 test runner
 */
class TestRunnerIntegration {
public:
    /**
     * Initialize test persistence
     * @param workspaceRoot Optional workspace path
     */
    static void initialize(const std::string& workspaceRoot = "");

    /**
     * Record a test result
     * @param testName Name of the test
     * @param testFile File containing the test
     * @param passed Whether test passed
     * @param duration Duration in milliseconds
     * @param error Error message if failed
     * @return true if status changed
     */
    static bool recordTestResult(const std::string& testName,
                                 const std::string& testFile,
                                 bool passed,
                                 uint32_t duration,
                                 const std::string& error = "");

    /**
     * Get persistence instance
     * @return Shared pointer to TestPersistence
     */
    static std::shared_ptr<TestPersistence> getPersistence();

    /**
     * Print test summary with persistence data
     */
    static void printSummary();

    /**
     * Enable/disable persistence recording
     * @param enabled Whether to record results
     */
    static void setEnabled(bool enabled);

    /**
     * Check if persistence is enabled
     * @return true if recording is enabled
     */
    static bool isEnabled();

private:
    static std::shared_ptr<TestPersistence> persistence;
    static bool enabled;
};
