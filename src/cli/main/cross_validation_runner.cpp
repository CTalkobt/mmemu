#include "cross_validation_runner.h"
#include <spdlog/spdlog.h>
#include <sstream>
#include <iomanip>
#include <algorithm>

CrossValidationRunner::CrossValidationRunner() {}

CrossValidationRunner::~CrossValidationRunner() {}

std::unique_ptr<CrossValidationRunner> CrossValidationRunner::withEmulator(
    const std::string& host, uint16_t port) {
    auto runner = std::unique_ptr<CrossValidationRunner>(new CrossValidationRunner());
    runner->m_emulator = HardwareTestBridge::connectEmulator(host, port);
    if (!runner->m_emulator) {
        spdlog::error("Failed to connect to emulator at {}:{}", host, port);
        return nullptr;
    }
    return runner;
}

std::unique_ptr<CrossValidationRunner> CrossValidationRunner::withHardware(
    const std::string& portPath, uint32_t baudRate) {
    auto runner = std::unique_ptr<CrossValidationRunner>(new CrossValidationRunner());
    runner->m_hardware = HardwareTestBridge::connectHardware(portPath, baudRate);
    if (!runner->m_hardware) {
        spdlog::error("Failed to connect to hardware at {}", portPath);
        return nullptr;
    }
    return runner;
}

std::unique_ptr<CrossValidationRunner> CrossValidationRunner::withBoth(
    const std::string& emuHost, uint16_t emuPort,
    const std::string& hwPort, uint32_t hwBaudRate) {
    auto runner = std::unique_ptr<CrossValidationRunner>(new CrossValidationRunner());

    // Connect to emulator
    runner->m_emulator = HardwareTestBridge::connectEmulator(emuHost, emuPort);
    if (!runner->m_emulator) {
        spdlog::warn("Failed to connect to emulator at {}:{}", emuHost, emuPort);
    } else {
        spdlog::info("Connected to emulator");
    }

    // Connect to hardware (if port specified)
    if (!hwPort.empty()) {
        runner->m_hardware = HardwareTestBridge::connectHardware(hwPort, hwBaudRate);
        if (!runner->m_hardware) {
            spdlog::warn("Failed to connect to hardware at {}", hwPort);
        } else {
            spdlog::info("Connected to hardware");
        }
    }

    if (!runner->m_emulator && !runner->m_hardware) {
        spdlog::error("Failed to connect to any target");
        return nullptr;
    }

    return runner;
}

CrossValidationRunner::ComparisonResult CrossValidationRunner::runTest(
    const TestCase& test) {

    ComparisonResult result;
    result.testName = test.name;

    // Run on emulator if available
    if (m_emulator) {
        spdlog::info("Running test '{}' on emulator...", test.name);

        if (!m_emulator->loadMemoryFile(test.programAddr, test.programPath)) {
            result.emulatorError = "Failed to load program";
            result.emulatorPass = false;
        } else {
            auto testResult = m_emulator->runTest(
                test.programAddr,
                test.resultAddr,
                test.resultSize,
                test.timeoutMs
            );

            result.emulatorPass = testResult.success;
            result.emulatorError = testResult.error;
            result.emulatorMemory = testResult.memorySnapshot;
            result.emulatorOutput = testResult.output;

            if (testResult.success) {
                spdlog::info("Emulator test passed ({}ms, {} cycles)",
                             test.timeoutMs, testResult.executionCycles);
            } else {
                spdlog::error("Emulator test failed: {}", testResult.error);
            }
        }
    }

    // Run on hardware if available
    if (m_hardware) {
        spdlog::info("Running test '{}' on hardware...", test.name);

        if (!m_hardware->loadMemoryFile(test.programAddr, test.programPath)) {
            result.hardwareError = "Failed to load program";
            result.hardwarePass = false;
        } else {
            auto testResult = m_hardware->runTest(
                test.programAddr,
                test.resultAddr,
                test.resultSize,
                test.timeoutMs
            );

            result.hardwarePass = testResult.success;
            result.hardwareError = testResult.error;
            result.hardwareMemory = testResult.memorySnapshot;
            result.hardwareOutput = testResult.output;

            if (testResult.success) {
                spdlog::info("Hardware test passed");
            } else {
                spdlog::error("Hardware test failed: {}", testResult.error);
            }
        }
    }

    // Compare results if both succeeded
    if (result.emulatorPass && result.hardwarePass) {
        std::string diffReport;
        result.resultsMatch = compareMemory(
            result.emulatorMemory,
            result.hardwareMemory,
            diffReport
        );

        if (result.resultsMatch) {
            spdlog::info("Results match! ✓");
        } else {
            spdlog::warn("Results differ:\n{}", diffReport);
        }
    }

    return result;
}

std::map<std::string, CrossValidationRunner::ComparisonResult>
CrossValidationRunner::runTests(const std::vector<TestCase>& tests) {
    std::map<std::string, ComparisonResult> results;

    spdlog::info("Running {} test cases...", tests.size());
    int passed = 0;
    int failed = 0;

    for (const auto& test : tests) {
        auto result = runTest(test);
        results[test.name] = result;

        if (result.overallPass()) {
            passed++;
        } else {
            failed++;
        }
    }

    spdlog::info("Test summary: {} passed, {} failed", passed, failed);
    return results;
}

bool CrossValidationRunner::compareMemory(
    const std::vector<uint8_t>& expected,
    const std::vector<uint8_t>& actual,
    std::string& diffReport) {

    if (expected.size() != actual.size()) {
        std::ostringstream oss;
        oss << "Memory size mismatch: expected " << expected.size()
            << " bytes, got " << actual.size() << " bytes";
        diffReport = oss.str();
        return false;
    }

    std::vector<uint32_t> diffAddresses;
    for (size_t i = 0; i < expected.size(); ++i) {
        if (expected[i] != actual[i]) {
            diffAddresses.push_back(i);
        }
    }

    if (diffAddresses.empty()) {
        return true;
    }

    // Format difference report
    std::ostringstream oss;
    oss << "Found " << diffAddresses.size() << " byte(s) that differ:\n";
    oss << std::hex << std::setfill('0');

    for (size_t i = 0; i < std::min(diffAddresses.size(), size_t(16)); ++i) {
        uint32_t addr = diffAddresses[i];
        oss << "  0x" << std::setw(4) << addr << ": expected 0x"
            << std::setw(2) << (int)expected[addr]
            << ", got 0x" << std::setw(2) << (int)actual[addr] << "\n";
    }

    if (diffAddresses.size() > 16) {
        oss << "  ... and " << (diffAddresses.size() - 16) << " more\n";
    }

    diffReport = oss.str();
    return false;
}

std::string CrossValidationRunner::formatResult(const ComparisonResult& result) {
    std::ostringstream oss;

    oss << "\n=== Test: " << result.testName << " ===\n";

    if (result.emulatorPass) {
        oss << "Emulator: ✓ PASS\n";
    } else {
        oss << "Emulator: ✗ FAIL (" << result.emulatorError << ")\n";
    }

    if (!result.hardwareError.empty()) {
        if (result.hardwarePass) {
            oss << "Hardware: ✓ PASS\n";
        } else {
            oss << "Hardware: ✗ FAIL (" << result.hardwareError << ")\n";
        }
    }

    if (result.emulatorPass && result.hardwarePass) {
        if (result.resultsMatch) {
            oss << "Results: ✓ MATCH\n";
        } else {
            oss << "Results: ✗ DIFFER\n";
            oss << "  Emulator memory: " << result.emulatorMemory.size() << " bytes\n";
            oss << "  Hardware memory: " << result.hardwareMemory.size() << " bytes\n";
        }
    }

    return oss.str();
}
