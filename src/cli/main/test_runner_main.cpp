#include "unified_test_runner.h"
#include "test_report_generator.h"
#include "spdlog/spdlog.h"
#include "spdlog/sinks/stdout_color_sinks.h"
#include <iostream>
#include <filesystem>
#include <chrono>
#include <ctime>

namespace fs = std::filesystem;

std::string getCurrentTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    char buf[100];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&time));
    return std::string(buf);
}

int main(int argc, char* argv[]) {
    // Setup logging
    auto console = spdlog::stdout_color_mt("console");
    spdlog::set_default_logger(console);

    try {
        // Parse arguments
        auto config = UnifiedTestRunner::parseArgs(argc, argv);

        // Check for report options
        std::string reportFile;
        bool generateReport = false;
        bool htmlReport = false;

        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "-report") {
                generateReport = true;
            } else if (arg == "-report-html") {
                generateReport = true;
                htmlReport = true;
            } else if (arg == "-report-file" && i + 1 < argc) {
                generateReport = true;
                reportFile = argv[++i];
            }
        }

        // If no test files specified, discover them in default location
        if (config.testFiles.empty()) {
            std::cout << "📁 Discovering test programs...\n";
            auto discovered = UnifiedTestRunner::discoverTests("tests/45gs02");

            if (!discovered.empty()) {
                std::cout << "Found " << discovered.size() << " test(s):\n";
                for (const auto& test : discovered) {
                    std::cout << "  - " << fs::path(test).stem().string() << "\n";
                }
                config.testFiles = discovered;
            } else {
                std::cerr << "No test files found in tests/45gs02\n";
                return 1;
            }
        }

        // Create and run test runner
        UnifiedTestRunner runner(config);

        if (config.verbose) {
            std::cout << "Configuration:\n";
            std::cout << "  Machine: " << config.machine << "\n";
            std::cout << "  Backends: ";
            for (const auto& b : config.backends) {
                std::cout << UnifiedTestRunner::backendName(b) << " ";
            }
            std::cout << "\n";
            std::cout << "  Tests: " << config.testFiles.size() << "\n";
            std::cout << "\n";
        }

        // Record start time
        auto startTime = std::chrono::high_resolution_clock::now();

        // Run tests
        auto results = runner.runTests(config.testFiles);

        // Record end time
        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

        // Calculate statistics
        int passedTests = 0;
        int consistentTests = 0;
        for (const auto& result : results) {
            if (result.overallPass()) {
                passedTests++;
            }
            if (result.resultsConsistent()) {
                consistentTests++;
            }
        }

        // Print results (unless report mode is enabled)
        if (!generateReport) {
            if (config.jsonOutput) {
                std::cout << UnifiedTestRunner::toJSON(results) << "\n";
            } else {
                UnifiedTestRunner::printResults(results, config.verbose);
            }
        }

        // Generate report if requested
        if (generateReport) {
            TestReportGenerator::ReportConfig reportConfig;
            reportConfig.markdownOutput = !htmlReport;
            reportConfig.htmlOutput = htmlReport;
            reportConfig.outputFile = reportFile;

            TestReportGenerator::ReportMetadata metadata;
            metadata.timestamp = getCurrentTimestamp();
            metadata.machine = config.machine;
            for (const auto& b : config.backends) {
                metadata.backends.push_back(UnifiedTestRunner::backendName(b));
            }
            metadata.totalTests = results.size();
            metadata.passedTests = passedTests;
            metadata.consistentTests = consistentTests;
            metadata.executionTimeMs = duration.count();

            TestReportGenerator generator(reportConfig);
            generator.generateReport(results, metadata);
        }

        // Exit with appropriate code
        bool anyFailed = false;
        for (const auto& result : results) {
            if (!result.overallPass()) {
                anyFailed = true;
                break;
            }
        }

        return anyFailed ? 1 : 0;

    } catch (const std::exception& e) {
        std::cerr << "❌ Error: " << e.what() << "\n";
        return 1;
    }
}
