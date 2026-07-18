#pragma once

#include <string>
#include <cstdint>

/**
 * Validation command handler
 * Runs xemu-based SID filter validation tests
 */

class ValidateCommand {
public:
    /// Run validation at specified level
    /// Returns 0 on success (all tests pass), 1 on failure
    static int runValidation(const std::string& level = "STANDARD");

    /// Print validation usage and examples
    static void printUsage();

private:
    /// Convert level string to enum
    static uint8_t parseLevel(const std::string& level);

    /// Print validation results summary
    static void printSummary(int passed, int failed, float avgError, uint32_t timeMs);
};
