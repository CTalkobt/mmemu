#pragma once

#include "unified_test_runner.h"
#include <string>
#include <vector>
#include <chrono>

class TestReportGenerator {
public:
    struct ReportConfig {
        bool htmlOutput = false;
        bool markdownOutput = true;
        bool jsonOutput = false;
        std::string outputFile;
        bool includeMemoryDumps = false;
    };

    struct ReportMetadata {
        std::string timestamp;
        std::string machine;
        std::vector<std::string> backends;
        int totalTests = 0;
        int passedTests = 0;
        int consistentTests = 0;
        double executionTimeMs = 0;
    };

    explicit TestReportGenerator(const ReportConfig& config);

    void generateReport(const std::vector<UnifiedTestRunner::TestResult>& results,
                       const ReportMetadata& metadata);

    static std::string generateMarkdown(
        const std::vector<UnifiedTestRunner::TestResult>& results,
        const ReportMetadata& metadata);

    static std::string generateHtml(
        const std::vector<UnifiedTestRunner::TestResult>& results,
        const ReportMetadata& metadata);

private:
    ReportConfig m_config;

    static std::string formatTestSummary(const ReportMetadata& metadata);
    static std::string formatTestDetails(const std::vector<UnifiedTestRunner::TestResult>& results);
    static std::string formatMemoryComparison(const UnifiedTestRunner::TestResult& result);
};
