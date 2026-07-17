#include "test_report_generator.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <iomanip>
#include <cmath>

TestReportGenerator::TestReportGenerator(const ReportConfig& config)
    : m_config(config) {
}

void TestReportGenerator::generateReport(
    const std::vector<UnifiedTestRunner::TestResult>& results,
    const ReportMetadata& metadata) {

    std::string reportContent;

    if (m_config.markdownOutput) {
        reportContent = generateMarkdown(results, metadata);
    } else if (m_config.htmlOutput) {
        reportContent = generateHtml(results, metadata);
    }

    // Output to file or stdout
    if (!m_config.outputFile.empty()) {
        std::ofstream file(m_config.outputFile);
        if (file) {
            file << reportContent;
            file.close();
            std::cout << "✅ Report written to: " << m_config.outputFile << "\n";
        } else {
            std::cerr << "❌ Failed to write report to: " << m_config.outputFile << "\n";
        }
    } else {
        std::cout << reportContent;
    }
}

std::string TestReportGenerator::generateMarkdown(
    const std::vector<UnifiedTestRunner::TestResult>& results,
    const ReportMetadata& metadata) {

    std::ostringstream oss;

    // Header
    oss << "# Cross-Validation Test Report\n\n";
    oss << formatTestSummary(metadata);
    oss << "\n\n";
    oss << formatTestDetails(results);

    return oss.str();
}

std::string TestReportGenerator::generateHtml(
    const std::vector<UnifiedTestRunner::TestResult>& results,
    const ReportMetadata& metadata) {

    std::ostringstream oss;

    oss << "<!DOCTYPE html>\n"
        << "<html>\n"
        << "<head>\n"
        << "  <meta charset=\"UTF-8\">\n"
        << "  <title>Cross-Validation Test Report</title>\n"
        << "  <style>\n"
        << "    body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; "
        << "margin: 20px; background: #f5f5f5; }\n"
        << "    .container { max-width: 1200px; margin: 0 auto; background: white; padding: 20px; "
        << "border-radius: 8px; box-shadow: 0 2px 4px rgba(0,0,0,0.1); }\n"
        << "    h1 { color: #333; border-bottom: 3px solid #0066cc; padding-bottom: 10px; }\n"
        << "    h2 { color: #555; margin-top: 30px; }\n"
        << "    .summary { display: grid; grid-template-columns: repeat(auto-fit, minmax(200px, 1fr)); "
        << "gap: 15px; margin: 20px 0; }\n"
        << "    .summary-box { background: #f9f9f9; padding: 15px; border-radius: 6px; "
        << "border-left: 4px solid #0066cc; }\n"
        << "    .summary-box h3 { margin: 0 0 10px 0; color: #666; }\n"
        << "    .summary-box .value { font-size: 24px; font-weight: bold; color: #0066cc; }\n"
        << "    .pass { color: #28a745; }\n"
        << "    .fail { color: #dc3545; }\n"
        << "    .warn { color: #ffc107; }\n"
        << "    table { width: 100%; border-collapse: collapse; margin: 15px 0; }\n"
        << "    th, td { padding: 12px; text-align: left; border-bottom: 1px solid #ddd; }\n"
        << "    th { background: #f5f5f5; font-weight: 600; }\n"
        << "    tr:hover { background: #fafafa; }\n"
        << "    .status-pass { color: #28a745; font-weight: 600; }\n"
        << "    .status-fail { color: #dc3545; font-weight: 600; }\n"
        << "    .memory-dump { background: #f5f5f5; padding: 10px; border-radius: 4px; "
        << "font-family: monospace; font-size: 12px; overflow-x: auto; margin: 10px 0; }\n"
        << "    .backend-result { margin-left: 20px; padding: 10px; background: #fafafa; "
        << "border-radius: 4px; margin-bottom: 10px; }\n"
        << "  </style>\n"
        << "</head>\n"
        << "<body>\n"
        << "<div class=\"container\">\n"
        << "  <h1>🧪 Cross-Validation Test Report</h1>\n";

    // Summary section
    oss << "  <h2>Summary</h2>\n"
        << "  <div class=\"summary\">\n"
        << "    <div class=\"summary-box\">\n"
        << "      <h3>Total Tests</h3>\n"
        << "      <div class=\"value\">" << metadata.totalTests << "</div>\n"
        << "    </div>\n"
        << "    <div class=\"summary-box\">\n"
        << "      <h3>Passed</h3>\n"
        << "      <div class=\"value pass\">" << metadata.passedTests << "</div>\n"
        << "    </div>\n"
        << "    <div class=\"summary-box\">\n"
        << "      <h3>Consistent</h3>\n"
        << "      <div class=\"value\">" << metadata.consistentTests << "</div>\n"
        << "    </div>\n"
        << "    <div class=\"summary-box\">\n"
        << "      <h3>Execution Time</h3>\n"
        << "      <div class=\"value\">" << std::fixed << std::setprecision(1)
        << metadata.executionTimeMs << "ms</div>\n"
        << "    </div>\n"
        << "  </div>\n";

    // Metadata
    oss << "  <p><strong>Machine:</strong> " << metadata.machine << " | "
        << "<strong>Backends:</strong> ";
    for (size_t i = 0; i < metadata.backends.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << metadata.backends[i];
    }
    oss << " | <strong>Timestamp:</strong> " << metadata.timestamp << "</p>\n";

    // Test details
    oss << "  <h2>Test Results</h2>\n"
        << "  <table>\n"
        << "    <tr>\n"
        << "      <th>Test Name</th>\n"
        << "      <th>Status</th>\n";
    for (const auto& backend : metadata.backends) {
        oss << "      <th>" << backend << "</th>\n";
    }
    oss << "      <th>Consistent</th>\n"
        << "    </tr>\n";

    for (const auto& result : results) {
        oss << "    <tr>\n"
            << "      <td><strong>" << result.testName << "</strong></td>\n"
            << "      <td>";

        if (result.overallPass()) {
            oss << "<span class=\"status-pass\">✅ PASS</span>";
        } else {
            oss << "<span class=\"status-fail\">❌ FAIL</span>";
        }

        oss << "</td>\n";

        for (const auto& br : result.results) {
            oss << "      <td>";
            if (br.passed) {
                oss << "<span class=\"status-pass\">✅</span>";
            } else {
                oss << "<span class=\"status-fail\">❌</span>";
                if (!br.error.empty()) {
                    oss << " <small>" << br.error << "</small>";
                }
            }
            oss << "</td>\n";
        }

        oss << "      <td>";
        if (result.resultsConsistent()) {
            oss << "<span class=\"status-pass\">✓</span>";
        } else {
            oss << "<span class=\"status-fail\">✗ Diverge</span>";
        }
        oss << "</td>\n"
            << "    </tr>\n";

        // Memory comparison (if available)
        if (result.results.size() > 1 && !result.results[0].memory.empty()) {
            oss << "    <tr><td colspan=\"" << (3 + metadata.backends.size()) << "\">\n";
            oss << formatMemoryComparison(result);
            oss << "    </td></tr>\n";
        }
    }

    oss << "  </table>\n";

    // Footer
    oss << "  <hr>\n"
        << "  <p><small>Generated by mmemu Cross-Validation Test Suite</small></p>\n"
        << "</div>\n"
        << "</body>\n"
        << "</html>\n";

    return oss.str();
}

std::string TestReportGenerator::formatTestSummary(const ReportMetadata& metadata) {
    std::ostringstream oss;

    oss << "## Summary\n\n";
    oss << "| Metric | Value |\n";
    oss << "|--------|-------|\n";
    oss << "| **Timestamp** | " << metadata.timestamp << " |\n";
    oss << "| **Machine** | " << metadata.machine << " |\n";
    oss << "| **Backends** | " << "";
    for (size_t i = 0; i < metadata.backends.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << metadata.backends[i];
    }
    oss << " |\n";
    oss << "| **Total Tests** | " << metadata.totalTests << " |\n";
    oss << "| **Passed** | " << metadata.passedTests << "/" << metadata.totalTests
        << " (" << std::fixed << std::setprecision(1)
        << (metadata.totalTests > 0 ? (100.0 * metadata.passedTests / metadata.totalTests) : 0)
        << "%) |\n";
    oss << "| **Consistent** | " << metadata.consistentTests << "/" << metadata.totalTests << " |\n";
    oss << "| **Execution Time** | " << std::fixed << std::setprecision(1)
        << metadata.executionTimeMs << " ms |\n";

    // Pass/fail indicators
    oss << "\n### Result\n\n";
    if (metadata.passedTests == metadata.totalTests && metadata.consistentTests == metadata.totalTests) {
        oss << "✅ **ALL TESTS PASSED** - Emulation is consistent across all backends\n";
    } else if (metadata.passedTests == metadata.totalTests) {
        oss << "⚠️ **TESTS PASSED BUT INCONSISTENT** - Results differ between backends\n";
    } else {
        oss << "❌ **TESTS FAILED** - Some tests did not pass\n";
    }

    return oss.str();
}

std::string TestReportGenerator::formatTestDetails(
    const std::vector<UnifiedTestRunner::TestResult>& results) {

    std::ostringstream oss;

    oss << "## Test Details\n\n";

    for (const auto& result : results) {
        oss << "### " << result.testName << "\n\n";
        oss << "**File:** `" << result.testFile << "`\n\n";

        // Status
        oss << "| Backend | Status | Error |\n";
        oss << "|---------|--------|-------|\n";

        for (const auto& br : result.results) {
            oss << "| " << br.backendName << " | ";
            if (br.passed) {
                oss << "✅ PASS";
            } else {
                oss << "❌ FAIL";
            }
            oss << " | " << (br.error.empty() ? "—" : br.error) << " |\n";
        }

        // Memory comparison
        if (result.results.size() > 1 && !result.results[0].memory.empty()) {
            oss << "\n**Memory Comparison ($0400-$040F):**\n\n";
            oss << "| Address | ";
            for (const auto& br : result.results) {
                oss << br.backendName << " | ";
            }
            oss << "Match |\n";

            oss << "|---------|";
            for (size_t i = 0; i < result.results.size(); ++i) {
                oss << "------|";
            }
            oss << "-------|\n";

            size_t maxSize = 0;
            for (const auto& br : result.results) {
                maxSize = std::max(maxSize, br.memory.size());
            }

            for (size_t i = 0; i < std::min(size_t(16), maxSize); ++i) {
                oss << "| $" << std::hex << std::setw(4) << std::setfill('0')
                    << (0x0400 + i) << std::dec << " | ";

                bool allMatch = true;
                uint8_t firstValue = 0;

                for (size_t j = 0; j < result.results.size(); ++j) {
                    if (i < result.results[j].memory.size()) {
                        if (j == 0) {
                            firstValue = result.results[j].memory[i];
                        } else if (result.results[j].memory[i] != firstValue) {
                            allMatch = false;
                        }
                        oss << std::hex << std::setw(2) << std::setfill('0')
                            << static_cast<int>(result.results[j].memory[i]) << std::dec;
                    } else {
                        oss << "—";
                    }
                    oss << " | ";
                }

                oss << (allMatch ? "✓" : "✗") << " |\n";
            }
        }

        // Consistency check
        oss << "\n**Consistency:** ";
        if (result.resultsConsistent()) {
            oss << "✅ All backends match\n";
        } else {
            oss << "⚠️ Results diverge\n";
        }

        oss << "\n";
    }

    return oss.str();
}

std::string TestReportGenerator::formatMemoryComparison(
    const UnifiedTestRunner::TestResult& result) {

    std::ostringstream oss;

    oss << "<div style=\"margin: 10px 0;\">\n";
    oss << "<strong>Memory Dump:</strong>\n";
    oss << "<table style=\"font-family: monospace; width: 100%;\">\n";
    oss << "<tr><th>Address</th>";

    for (const auto& br : result.results) {
        oss << "<th>" << br.backendName << "</th>";
    }

    oss << "<th>Match</th></tr>\n";

    size_t maxSize = 0;
    for (const auto& br : result.results) {
        maxSize = std::max(maxSize, br.memory.size());
    }

    for (size_t i = 0; i < std::min(size_t(16), maxSize); ++i) {
        oss << "<tr><td>$" << std::hex << std::setw(4) << std::setfill('0')
            << (0x0400 + i) << std::dec << "</td>";

        bool allMatch = true;
        uint8_t firstValue = 0;

        for (size_t j = 0; j < result.results.size(); ++j) {
            if (i < result.results[j].memory.size()) {
                if (j == 0) {
                    firstValue = result.results[j].memory[i];
                } else if (result.results[j].memory[i] != firstValue) {
                    allMatch = false;
                }
                oss << "<td style=\"text-align: center;\">" << std::hex << std::setw(2)
                    << std::setfill('0') << static_cast<int>(result.results[j].memory[i])
                    << std::dec << "</td>";
            } else {
                oss << "<td>—</td>";
            }
        }

        oss << "<td style=\"text-align: center;\">" << (allMatch ? "✓" : "✗") << "</td></tr>\n";
    }

    oss << "</table>\n</div>\n";

    return oss.str();
}
