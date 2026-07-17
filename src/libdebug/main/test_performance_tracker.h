#pragma once

#include <string>
#include <vector>
#include <map>
#include <cstdint>
#include <chrono>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

/**
 * Single function timing
 */
struct FunctionTiming {
    std::string name;
    uint64_t callCount = 0;
    uint64_t totalMicroseconds = 0;  // Total time across all calls
    uint64_t minMicroseconds = UINT64_MAX;
    uint64_t maxMicroseconds = 0;

    json toJson() const;
    static FunctionTiming fromJson(const json& j);
};

/**
 * Performance data for a single test run
 */
struct TestPerformanceData {
    std::string testName;
    std::string timestamp;
    std::string gitHash;
    uint64_t totalMicroseconds = 0;
    std::vector<FunctionTiming> functions;

    json toJson() const;
    static TestPerformanceData fromJson(const json& j);
};

/**
 * Test Performance Tracker
 * Records detailed performance metrics for test profiling
 */
class TestPerformanceTracker {
public:
    /**
     * Constructor
     * @param workspaceRoot Optional workspace path
     */
    explicit TestPerformanceTracker(const std::string& workspaceRoot = "");

    /**
     * Start timing a test
     * @param testName Name of the test
     * @return opaque handle for this timing session
     */
    uint64_t startTest(const std::string& testName);

    /**
     * Record a function call timing
     * @param testHandle Handle from startTest
     * @param functionName Name of the function
     * @param microseconds Time taken in microseconds
     */
    void recordFunctionTiming(uint64_t testHandle, const std::string& functionName, uint64_t microseconds);

    /**
     * End timing a test and record results
     * @param testHandle Handle from startTest
     */
    void endTest(uint64_t testHandle);

    /**
     * Get performance data for a test
     * @param testName Name of the test
     * @return Performance data or empty if not found
     */
    std::vector<TestPerformanceData> getTestPerformance(const std::string& testName) const;

    /**
     * Get performance history for a test
     * @param testName Name of the test
     * @return All recorded runs for this test
     */
    std::vector<TestPerformanceData> getTestHistory(const std::string& testName) const;

    /**
     * Export performance data as JSON
     * @return Full performance data as JSON
     */
    json exportAsJson() const;

    /**
     * Export performance data as CSV
     * @param testName Name of the test
     * @return CSV string with header row
     */
    std::string exportAsCSV(const std::string& testName) const;

    /**
     * Clear all performance data
     */
    void clearAllData();

    /**
     * Clear performance data for specific test
     * @param testName Name of the test
     */
    void clearTestData(const std::string& testName);

    /**
     * Get persistence file path
     * @return Full path to test-performance.json
     */
    static std::string getPersistencePath();

    /**
     * Get current git commit hash
     * @return 7-char abbreviated hash or "unknown"
     */
    static std::string getGitHash();

    /**
     * Get current timestamp (ISO 8601)
     * @return Current time as ISO 8601 string
     */
    static std::string getCurrentTimestamp();

private:
    struct TestTimingSession {
        std::string testName;
        std::string gitHash;
        uint64_t startMicroseconds = 0;
        std::map<std::string, std::vector<uint64_t>> functionTimings;  // function name -> list of durations
    };

    std::string dataPath;
    std::string workspaceRoot;
    std::map<std::string, std::vector<TestPerformanceData>> testHistory;
    std::map<uint64_t, TestTimingSession> activeSessions;
    uint64_t nextSessionHandle = 1;

    /**
     * Load performance data from file
     */
    void loadData();

    /**
     * Save performance data to file
     */
    void saveData();

    /**
     * Get current time in microseconds
     */
    static uint64_t getCurrentMicroseconds();

    /**
     * Clean up old data (>90 days)
     */
    void cleanupOldData();
};
