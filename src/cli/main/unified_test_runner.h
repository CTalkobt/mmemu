#pragma once

#include "cross_validation_runner.h"
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <optional>
#include <algorithm>

/**
 * Unified Test Runner - Dynamic multi-backend test orchestration
 *
 * Supports running test programs against a mix of:
 * - mmsim (emulator via TCP)
 * - xemu-xmega65 (alternative emulator via subprocess)
 * - Real MEGA65 hardware (via serial port)
 *
 * Command-line usage:
 *   test-runner [options] [test-file.bin ...]
 *
 * Options:
 *   -mmemu              Test on mmsim emulator
 *   -xmega65            Test on xemu-xmega65
 *   -real               Test on real MEGA65 hardware
 *   -all                Test on all available backends (default)
 *   -machine <type>     Machine preset (c64, vic20, rawMega65, etc.)
 *   -host <host>        Emulator host (default: 127.0.0.1)
 *   -port <port>        Emulator port (default: 6502)
 *   -serial <dev>       Serial port for hardware (default: /dev/ttyUSB0)
 *   -baud <rate>        Serial baud rate (default: 2000000)
 *   -timeout <ms>       Test timeout in ms (default: 5000)
 *   -json               JSON output format
 *   -verbose            Verbose output
 *   -help               Show help
 */
class UnifiedTestRunner {
public:
    enum class Backend {
        MMEMU,      // mmsim emulator
        XMEGA65,    // xemu-xmega65
        REAL,       // Real MEGA65 hardware
    };

    struct Config {
        std::vector<Backend> backends;
        std::string machine = "c64";
        std::string emuHost = "127.0.0.1";
        uint16_t emuPort = 6502;
        std::string serialPort = "/dev/ttyUSB0";
        uint32_t serialBaud = 2000000;
        uint32_t timeoutMs = 5000;
        bool jsonOutput = false;
        bool verbose = false;
        std::vector<std::string> testFiles;
    };

    struct TestResult {
        std::string testName;
        std::string testFile;

        // Per-backend results
        struct BackendResult {
            Backend backend;
            std::string backendName;
            bool passed = false;
            std::string error;
            std::vector<uint8_t> memory;
            std::string output;
        };

        std::vector<BackendResult> results;

        // Comparison results
        bool allMatch = false;
        std::string divergenceReport;

        // Summary
        bool overallPass() const {
            if (results.empty()) return false;
            return std::all_of(results.begin(), results.end(),
                             [](const BackendResult& r) { return r.passed; });
        }

        bool resultsConsistent() const {
            if (results.size() < 2) return true;
            const auto& first = results[0].memory;
            for (size_t i = 1; i < results.size(); ++i) {
                if (results[i].memory != first) return false;
            }
            return true;
        }
    };

    /**
     * Parse command-line arguments
     */
    static Config parseArgs(int argc, char* argv[]);

    /**
     * Discover test programs in a directory
     */
    static std::vector<std::string> discoverTests(const std::string& dir);

    /**
     * Get human-readable backend name
     */
    static std::string backendName(Backend b);

    /**
     * Run tests on specified backends
     */
    std::vector<TestResult> runTests(const std::vector<std::string>& testFiles);

    /**
     * Run single test on specified backends
     */
    TestResult runTest(const std::string& testFile);

    /**
     * Print results in human-readable format
     */
    static void printResults(const std::vector<TestResult>& results, bool verbose = false);

    /**
     * Print results in JSON format
     */
    static std::string toJSON(const std::vector<TestResult>& results);

    /**
     * Create runner from configuration
     */
    explicit UnifiedTestRunner(const Config& config);

private:
    Config m_config;
    std::unique_ptr<CrossValidationRunner> m_runner;

    /**
     * Create cross-validation runner from config
     */
    std::unique_ptr<CrossValidationRunner> createRunner();

    /**
     * Test a single file against all configured backends
     */
    TestResult runTestInternal(const std::string& testFile);
};
