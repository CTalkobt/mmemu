#include "validate_cmd.h"
#include "plugins/devices/sid6581/main/sid_validation_suite.h"
#include "cli/main/xemu_bridge.h"
#include "plugins/devices/sid6581/main/spectral_analyzer.h"
#include <iostream>
#include <iomanip>
#include <cstring>

int ValidateCommand::runValidation(const std::string& level) {
    std::cout << "\n=== SID Filter Cross-Validation (mmsim vs xemu-xmega65) ===\n";
    std::cout << "Validation Level: " << level << "\n";
    std::cout << "Starting tests...\n\n";

    try {
        SIDValidationSuite suite("/usr/local/bin/xemu-xmega65");

        // Convert to enum
        SIDValidationSuite::ValidationLevel validationLevel;
        if (level == "QUICK" || level == "QUICK_SMOKE") {
            validationLevel = SIDValidationSuite::ValidationLevel::QUICK_SMOKE;
        } else if (level == "COMPREHENSIVE") {
            validationLevel = SIDValidationSuite::ValidationLevel::COMPREHENSIVE;
        } else {
            validationLevel = SIDValidationSuite::ValidationLevel::STANDARD;
        }

        auto report = suite.runValidation(validationLevel);

        // Print results
        std::cout << SIDValidationSuite::formatReport(report);

        // Return success/failure
        return report.allTestsPassed ? 0 : 1;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}

void ValidateCommand::printUsage() {
    std::cout << "Validate SID6581 filter implementation against xemu-xmega65\n\n"
              << "Usage:\n"
              << "  mmemu-cli --xemu-validation [LEVEL]\n"
              << "  mmemu-validate [LEVEL]\n\n"
              << "Levels:\n"
              << "  QUICK          - Fast sanity check (2 tests, ~1 min)\n"
              << "  STANDARD       - Full basic validation (4 tests, ~5 min) [DEFAULT]\n"
              << "  COMPREHENSIVE  - Exhaustive testing (6+ tests, ~15 min)\n\n"
              << "Examples:\n"
              << "  mmemu-cli --xemu-validation QUICK\n"
              << "  mmemu-cli --xemu-validation STANDARD\n"
              << "  mmemu-cli --xemu-validation COMPREHENSIVE\n\n"
              << "Requirements:\n"
              << "  - xemu-xmega65 installed and in /usr/local/bin/\n"
              << "  - ca65 assembler (for generating test programs)\n"
              << "  - 10-20 minutes for full suite\n\n"
              << "Test Types:\n"
              << "  - Resonance sweep (R=0→15 at fixed cutoff)\n"
              << "  - Cutoff frequency sweep (FC sweep at fixed resonance)\n"
              << "  - Combined waveforms (tri+pulse, saw+pulse mixing)\n"
              << "  - High-resonance saturation (soft-clipping detection)\n\n";
}

uint8_t ValidateCommand::parseLevel(const std::string& level) {
    if (level == "QUICK" || level == "QUICK_SMOKE") return 0;
    if (level == "STANDARD") return 1;
    if (level == "COMPREHENSIVE") return 2;
    return 1;  // Default to STANDARD
}

void ValidateCommand::printSummary(int passed, int failed, float avgError, uint32_t timeMs) {
    std::cout << "\n=== Summary ===\n";
    std::cout << "Passed: " << passed << "\n";
    std::cout << "Failed: " << failed << "\n";
    std::cout << "Average Spectral Error: " << std::fixed << std::setprecision(3)
              << avgError << "\n";
    std::cout << "Total Time: " << timeMs << " ms\n";
}
