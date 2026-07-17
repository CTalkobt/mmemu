#include "unified_test_runner.h"
#include "spdlog/spdlog.h"
#include "spdlog/sinks/stdout_color_sinks.h"
#include <iostream>
#include <filesystem>

namespace fs = std::filesystem;

int main(int argc, char* argv[]) {
    // Setup logging
    auto console = spdlog::stdout_color_mt("console");
    spdlog::set_default_logger(console);

    try {
        // Parse arguments
        auto config = UnifiedTestRunner::parseArgs(argc, argv);

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

        // Run tests
        auto results = runner.runTests(config.testFiles);

        // Print results
        if (config.jsonOutput) {
            std::cout << UnifiedTestRunner::toJSON(results) << "\n";
        } else {
            UnifiedTestRunner::printResults(results, config.verbose);
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
