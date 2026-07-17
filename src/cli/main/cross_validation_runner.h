#pragma once

#include "hardware_test_bridge.h"
#include <string>
#include <vector>
#include <map>
#include <memory>

/**
 * Cross-Validation Test Runner
 *
 * Runs identical test programs on both the emulator and real MEGA65 hardware,
 * then compares results to validate emulation accuracy.
 */
class CrossValidationRunner {
public:
    struct TestCase {
        std::string name;
        std::string programPath;   // Path to compiled test binary
        uint32_t programAddr = 0x0800;  // Where to load program
        uint32_t resultAddr = 0x2000;   // Where results are written
        uint32_t resultSize = 256;      // Size of result region
        uint32_t timeoutMs = 5000;      // Test timeout
    };

    struct ComparisonResult {
        std::string testName;
        bool emulatorPass = false;
        bool hardwarePass = false;
        bool resultsMatch = false;

        // Details
        std::string emulatorError;
        std::string hardwareError;
        std::vector<uint8_t> emulatorMemory;
        std::vector<uint8_t> hardwareMemory;
        std::string emulatorOutput;
        std::string hardwareOutput;

        // Summary
        bool overallPass() const {
            return emulatorPass && hardwarePass && resultsMatch;
        }
    };

    /**
     * Create runner with connection to emulator
     * @param host TCP host for SerialMonitorServer
     * @param port TCP port for SerialMonitorServer
     */
    static std::unique_ptr<CrossValidationRunner> withEmulator(
        const std::string& host = "127.0.0.1",
        uint16_t port = 6502
    );

    /**
     * Create runner with connection to hardware
     * @param portPath Serial port path
     * @param baudRate Baud rate
     */
    static std::unique_ptr<CrossValidationRunner> withHardware(
        const std::string& portPath,
        uint32_t baudRate = 2000000
    );

    /**
     * Create runner with both emulator and hardware for cross-validation
     */
    static std::unique_ptr<CrossValidationRunner> withBoth(
        const std::string& emuHost = "127.0.0.1",
        uint16_t emuPort = 6502,
        const std::string& hwPort = "",
        uint32_t hwBaudRate = 2000000
    );

    ~CrossValidationRunner();

    /**
     * Run a single test case on configured targets
     * @return Comparison result
     */
    ComparisonResult runTest(const TestCase& test);

    /**
     * Run multiple test cases
     * @return Map of test name to result
     */
    std::map<std::string, ComparisonResult> runTests(
        const std::vector<TestCase>& tests
    );

    /**
     * Check if both emulator and hardware are configured
     */
    bool canCrossValidate() const {
        return m_emulator && m_hardware;
    }

    /**
     * Check if emulator is available
     */
    bool hasEmulator() const { return m_emulator != nullptr; }

    /**
     * Check if hardware is available
     */
    bool hasHardware() const { return m_hardware != nullptr; }

private:
    CrossValidationRunner();

    std::unique_ptr<HardwareTestBridge> m_emulator;
    std::unique_ptr<HardwareTestBridge> m_hardware;

    /**
     * Compare two memory regions and return true if identical
     */
    static bool compareMemory(
        const std::vector<uint8_t>& expected,
        const std::vector<uint8_t>& actual,
        std::string& diffReport
    );

    /**
     * Format a comparison result as human-readable text
     */
    static std::string formatResult(const ComparisonResult& result);
};
