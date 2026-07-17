#include "unified_test_runner.h"
#include "spdlog/spdlog.h"
#include "nlohmann/json.hpp"
#include <filesystem>
#include <iostream>
#include <algorithm>
#include <fstream>

using json = nlohmann::json;
namespace fs = std::filesystem;

std::string UnifiedTestRunner::backendName(Backend b) {
    switch (b) {
        case Backend::MMEMU:   return "mmsim";
        case Backend::XMEGA65: return "xemu-xmega65";
        case Backend::REAL:    return "real MEGA65";
    }
    return "unknown";
}

UnifiedTestRunner::Config UnifiedTestRunner::parseArgs(int argc, char* argv[]) {
    Config config;
    config.backends.push_back(Backend::MMEMU);  // Default: test on mmsim

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "-mmemu") {
            // Replace default with explicit mmemu
            if (config.backends.size() == 1 && config.backends[0] == Backend::MMEMU) {
                // Already the default
            }
        } else if (arg == "-xmega65") {
            config.backends.push_back(Backend::XMEGA65);
        } else if (arg == "-real") {
            config.backends.push_back(Backend::REAL);
        } else if (arg == "-all") {
            config.backends.clear();
            config.backends = {Backend::MMEMU, Backend::XMEGA65, Backend::REAL};
        } else if (arg == "-machine" && i + 1 < argc) {
            config.machine = argv[++i];
        } else if (arg == "-host" && i + 1 < argc) {
            config.emuHost = argv[++i];
        } else if (arg == "-port" && i + 1 < argc) {
            config.emuPort = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "-serial" && i + 1 < argc) {
            config.serialPort = argv[++i];
        } else if (arg == "-baud" && i + 1 < argc) {
            config.serialBaud = std::stoul(argv[++i]);
        } else if (arg == "-timeout" && i + 1 < argc) {
            config.timeoutMs = std::stoul(argv[++i]);
        } else if (arg == "-json") {
            config.jsonOutput = true;
        } else if (arg == "-verbose") {
            config.verbose = true;
        } else if (arg == "-help") {
            std::cout << "Usage: test-runner [options] [test-file.bin ...]\n\n";
            std::cout << "Options:\n";
            std::cout << "  -mmemu              Test on mmsim emulator\n";
            std::cout << "  -xmega65            Test on xemu-xmega65\n";
            std::cout << "  -real               Test on real MEGA65 hardware\n";
            std::cout << "  -all                Test on all available backends\n";
            std::cout << "  -machine <type>     Machine preset (default: c64)\n";
            std::cout << "  -host <host>        Emulator host (default: 127.0.0.1)\n";
            std::cout << "  -port <port>        Emulator port (default: 6502)\n";
            std::cout << "  -serial <dev>       Serial port for hardware\n";
            std::cout << "  -baud <rate>        Serial baud rate (default: 2000000)\n";
            std::cout << "  -timeout <ms>       Test timeout in ms (default: 5000)\n";
            std::cout << "  -json               JSON output format\n";
            std::cout << "  -verbose            Verbose output\n";
            std::cout << "  -help               Show this help\n\n";
            std::cout << "Examples:\n";
            std::cout << "  test-runner -mmemu tests/45gs02/arithmetic.bin\n";
            std::cout << "  test-runner -all tests/45gs02/*.bin\n";
            std::cout << "  test-runner -mmemu -xmega65 -machine mega65 tests/45gs02/transfers.bin\n";
            exit(0);
        } else if (arg[0] != '-') {
            // Positional argument: test file
            config.testFiles.push_back(arg);
        }
    }

    // If no explicit backend flags given, use default
    if (config.backends.size() == 1 && config.backends[0] == Backend::MMEMU) {
        // Already set
    }

    return config;
}

std::vector<std::string> UnifiedTestRunner::discoverTests(const std::string& dir) {
    std::vector<std::string> tests;

    if (!fs::exists(dir)) {
        spdlog::warn("Test directory not found: {}", dir);
        return tests;
    }

    for (const auto& entry : fs::recursive_directory_iterator(dir)) {
        if (entry.path().extension() == ".bin") {
            tests.push_back(entry.path().string());
        }
    }

    std::sort(tests.begin(), tests.end());
    return tests;
}

UnifiedTestRunner::UnifiedTestRunner(const Config& config)
    : m_config(config), m_runner(createRunner()) {
}

std::unique_ptr<CrossValidationRunner> UnifiedTestRunner::createRunner() {
    // Create runner based on configured backends
    if (m_config.backends.empty()) {
        return nullptr;
    }

    bool hasMMemu = std::any_of(m_config.backends.begin(), m_config.backends.end(),
                               [](Backend b) { return b == Backend::MMEMU; });
    bool hasXmega65 = std::any_of(m_config.backends.begin(), m_config.backends.end(),
                                 [](Backend b) { return b == Backend::XMEGA65; });
    bool hasReal = std::any_of(m_config.backends.begin(), m_config.backends.end(),
                              [](Backend b) { return b == Backend::REAL; });

    if (hasMMemu && hasXmega65 && hasReal) {
        return CrossValidationRunner::withAll(
            m_config.emuHost, m_config.emuPort,
            "/usr/local/bin/xemu-xmega65",
            m_config.serialPort, m_config.serialBaud
        );
    } else if (hasMMemu && hasXmega65) {
        return CrossValidationRunner::withXemu(
            m_config.emuHost, m_config.emuPort,
            "/usr/local/bin/xemu-xmega65"
        );
    } else if (hasMMemu && hasReal) {
        return CrossValidationRunner::withBoth(
            m_config.emuHost, m_config.emuPort,
            m_config.serialPort, m_config.serialBaud
        );
    } else if (hasXmega65) {
        return CrossValidationRunner::withXemu(
            "127.0.0.1", 6502,
            "/usr/local/bin/xemu-xmega65"
        );
    } else if (hasReal) {
        return CrossValidationRunner::withHardware(
            m_config.serialPort, m_config.serialBaud
        );
    } else {
        return CrossValidationRunner::withEmulator(
            m_config.emuHost, m_config.emuPort
        );
    }
}

std::vector<UnifiedTestRunner::TestResult> UnifiedTestRunner::runTests(
    const std::vector<std::string>& testFiles) {
    std::vector<TestResult> results;

    for (const auto& testFile : testFiles) {
        results.push_back(runTest(testFile));
    }

    return results;
}

UnifiedTestRunner::TestResult UnifiedTestRunner::runTest(const std::string& testFile) {
    TestResult result;
    result.testFile = testFile;

    // Extract test name from filename
    fs::path p(testFile);
    result.testName = p.stem().string();

    if (!m_runner) {
        TestResult::BackendResult br;
        br.backend = Backend::MMEMU;
        br.backendName = "mmsim";
        br.passed = false;
        br.error = "Failed to create cross-validation runner";
        result.results.push_back(br);
        return result;
    }

    return runTestInternal(testFile);
}

UnifiedTestRunner::TestResult UnifiedTestRunner::runTestInternal(const std::string& testFile) {
    TestResult result;
    result.testFile = testFile;

    fs::path p(testFile);
    result.testName = p.stem().string();

    if (!fs::exists(testFile)) {
        for (const auto& backend : m_config.backends) {
            TestResult::BackendResult br;
            br.backend = backend;
            br.backendName = backendName(backend);
            br.passed = false;
            br.error = "Test file not found: " + testFile;
            result.results.push_back(br);
        }
        return result;
    }

    // Load test program
    std::ifstream file(testFile, std::ios::binary);
    if (!file) {
        for (const auto& backend : m_config.backends) {
            TestResult::BackendResult br;
            br.backend = backend;
            br.backendName = backendName(backend);
            br.passed = false;
            br.error = "Failed to open test file";
            result.results.push_back(br);
        }
        return result;
    }

    std::vector<uint8_t> programData((std::istreambuf_iterator<char>(file)),
                                      std::istreambuf_iterator<char>());
    file.close();

    // Run test on each backend
    for (const auto& backend : m_config.backends) {
        TestResult::BackendResult br;
        br.backend = backend;
        br.backendName = backendName(backend);

        switch (backend) {
            case Backend::MMEMU: {
                if (m_runner && m_runner->hasEmulator()) {
                    CrossValidationRunner::TestCase testCase;
                    testCase.name = result.testName;
                    testCase.programPath = testFile;
                    testCase.programAddr = 0x2000;
                    testCase.resultAddr = 0x0400;
                    testCase.resultSize = 16;
                    testCase.timeoutMs = m_config.timeoutMs;

                    auto cvResult = m_runner->runTest(testCase);
                    br.passed = cvResult.emulatorPass;
                    br.error = cvResult.emulatorError;
                    br.memory = cvResult.emulatorMemory;
                    br.output = cvResult.emulatorOutput;
                } else {
                    br.error = "mmsim not available";
                }
                break;
            }

            case Backend::XMEGA65: {
                if (m_runner && m_runner->hasXemu()) {
                    CrossValidationRunner::TestCase testCase;
                    testCase.name = result.testName;
                    testCase.programPath = testFile;
                    testCase.programAddr = 0x2000;
                    testCase.resultAddr = 0x0400;
                    testCase.resultSize = 16;
                    testCase.timeoutMs = m_config.timeoutMs;

                    auto cvResult = m_runner->runTest(testCase);
                    br.passed = cvResult.xemuPass;
                    br.error = cvResult.xemuError;
                    br.memory = cvResult.xemuMemory;
                    br.output = cvResult.xemuOutput;
                } else {
                    br.error = "xemu-xmega65 not available";
                }
                break;
            }

            case Backend::REAL: {
                if (m_runner && m_runner->hasHardware()) {
                    CrossValidationRunner::TestCase testCase;
                    testCase.name = result.testName;
                    testCase.programPath = testFile;
                    testCase.programAddr = 0x2000;
                    testCase.resultAddr = 0x0400;
                    testCase.resultSize = 16;
                    testCase.timeoutMs = m_config.timeoutMs;

                    auto cvResult = m_runner->runTest(testCase);
                    br.passed = cvResult.hardwarePass;
                    br.error = cvResult.hardwareError;
                    br.memory = cvResult.hardwareMemory;
                    br.output = cvResult.hardwareOutput;
                } else {
                    br.error = "Real MEGA65 hardware not available";
                }
                break;
            }
        }

        result.results.push_back(br);
    }

    // Check consistency
    if (result.results.size() > 1) {
        result.allMatch = result.resultsConsistent();
    }

    return result;
}

void UnifiedTestRunner::printResults(const std::vector<TestResult>& results, bool verbose) {
    std::cout << "\n╔════════════════════════════════════════════════════════╗\n";
    std::cout << "║         Unified Test Runner - Cross-Validation         ║\n";
    std::cout << "╚════════════════════════════════════════════════════════╝\n\n";

    for (const auto& result : results) {
        std::cout << "Test: " << result.testName << " (" << result.testFile << ")\n";
        std::cout << "──────────────────────────────────────────────────────\n";

        // Show per-backend results
        for (const auto& br : result.results) {
            std::cout << "  " << br.backendName << ":  ";
            if (br.passed) {
                std::cout << "✅ PASS\n";
            } else {
                std::cout << "❌ FAIL\n";
                if (!br.error.empty()) {
                    std::cout << "    Error: " << br.error << "\n";
                }
            }

            if (verbose && !br.memory.empty()) {
                std::cout << "    Memory: ";
                for (size_t i = 0; i < std::min(size_t(8), br.memory.size()); ++i) {
                    printf("%02X ", br.memory[i]);
                }
                if (br.memory.size() > 8) {
                    std::cout << "...";
                }
                std::cout << "\n";
            }
        }

        // Show consistency check
        if (result.results.size() > 1) {
            std::cout << "\n  Consistency: ";
            if (result.allMatch) {
                std::cout << "✅ All backends match!\n";
            } else {
                std::cout << "⚠ Results diverge\n";
            }
        }

        std::cout << "\n";
    }

    // Summary
    int totalTests = results.size();
    int passedTests = 0;
    int consistentTests = 0;

    for (const auto& result : results) {
        if (result.overallPass()) {
            passedTests++;
        }
        if (result.allMatch || result.results.size() == 1) {
            consistentTests++;
        }
    }

    std::cout << "╭────────────────────────────────────────────────────────╮\n";
    printf("│ Results: %d/%d passed, %d/%d consistent              │\n",
           passedTests, totalTests, consistentTests, totalTests);
    std::cout << "╰────────────────────────────────────────────────────────╯\n";
}

std::string UnifiedTestRunner::toJSON(const std::vector<TestResult>& results) {
    json root = json::array();

    for (const auto& result : results) {
        json testObj;
        testObj["testName"] = result.testName;
        testObj["testFile"] = result.testFile;
        testObj["overallPass"] = result.overallPass();
        testObj["consistent"] = result.resultsConsistent();

        json backendResults = json::array();
        for (const auto& br : result.results) {
            json brObj;
            brObj["backend"] = backendName(br.backend);
            brObj["passed"] = br.passed;
            brObj["error"] = br.error;

            json memData = json::array();
            for (const auto& byte : br.memory) {
                memData.push_back(byte);
            }
            brObj["memory"] = memData;
            brObj["output"] = br.output;

            backendResults.push_back(brObj);
        }
        testObj["results"] = backendResults;

        root.push_back(testObj);
    }

    return root.dump(2);
}
