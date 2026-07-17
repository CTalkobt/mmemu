#pragma once

#include <string>
#include <vector>
#include <map>
#include <cstdint>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

/**
 * Single test run result
 */
struct TestRunResult {
    std::string timestamp;  // ISO 8601
    std::string gitHash;    // 7-char abbreviated hash
    std::string status;     // "pass", "fail", "skip"
    uint32_t duration;      // milliseconds
    std::string error;      // error message if failed

    json toJson() const;
    static TestRunResult fromJson(const json& j);
};

/**
 * Test history entry
 */
struct TestHistory {
    std::string file;
    std::vector<TestRunResult> history;
    std::string lastStatus;
    std::string lastGitHash;
    uint32_t statusChanges = 0;

    json toJson() const;
    static TestHistory fromJson(const json& j);
};

/**
 * Test Run Persistence Manager
 * Tracks test results with git hash recording on status changes
 */
class TestPersistence {
public:
    /**
     * Constructor
     * @param workspaceRoot Optional workspace path (uses current dir if empty)
     */
    explicit TestPersistence(const std::string& workspaceRoot = "");

    /**
     * Record a test run result
     * @param testName Name of the test
     * @param filePath Path to test file
     * @param result Test result data
     * @return true if test status changed
     */
    bool recordTestRun(const std::string& testName, const std::string& filePath,
                       const TestRunResult& result);

    /**
     * Get test history
     * @param testName Name of the test
     * @return Vector of all runs for this test
     */
    std::vector<TestRunResult> getTestHistory(const std::string& testName) const;

    /**
     * Get status change count
     * @param testName Name of the test
     * @return Number of status transitions
     */
    uint32_t getStatusChangeCount(const std::string& testName) const;

    /**
     * Get last status for test
     * @param testName Name of the test
     * @return Last status or empty string if not found
     */
    std::string getLastStatus(const std::string& testName) const;

    /**
     * Get last git hash for test
     * @param testName Name of the test
     * @return Last git hash or "unknown"
     */
    std::string getLastGitHash(const std::string& testName) const;

    /**
     * Get status description (e.g., "✓ pass (5m ago, a6e0156)")
     * @param testName Name of the test
     * @return Formatted status string
     */
    std::string getStatusDescription(const std::string& testName) const;

    /**
     * Export history as CSV for a single test
     * @param testName Name of the test
     * @return CSV string with header row
     */
    std::string exportAsCSV(const std::string& testName) const;

    /**
     * Export all history as CSV
     * @return CSV string with all tests
     */
    std::string exportAllAsCSV() const;

    /**
     * Clear all test history
     */
    void clearAllHistory();

    /**
     * Clear history for specific test
     * @param testName Name of the test
     */
    void clearTestHistory(const std::string& testName);

    /**
     * Get all test names
     * @return Vector of test names
     */
    std::vector<std::string> getTestNames() const;

    /**
     * Get persistence file path
     * @return Full path to test-results.json
     */
    static std::string getPersistencePath();

    /**
     * Get current timestamp (ISO 8601)
     * @return Current time as ISO 8601 string
     */
    static std::string getCurrentTimestamp();

    /**
     * Get current git commit hash
     * @return 7-char abbreviated hash or "unknown"
     */
    static std::string getGitHash();

private:
    std::string dataPath;
    std::string workspaceRoot;
    std::map<std::string, TestHistory> tests;
    bool dirty = false;

    /**
     * Load test results from file
     */
    void loadData();

    /**
     * Save test results to file
     */
    void saveData();

    /**
     * Clean up old data (>30 days, keep minimum 10)
     */
    void cleanupOldData();

    /**
     * Check if test status has changed
     * @param testName Name of the test
     * @param newStatus New status to check
     * @return true if status is different from last recorded
     */
    bool hasStatusChanged(const std::string& testName, const std::string& newStatus) const;
};
