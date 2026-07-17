#include "test_performance_tracker.h"
#include <filesystem>
#include <fstream>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <array>

namespace fs = std::filesystem;

/**
 * FunctionTiming JSON conversion
 */
json FunctionTiming::toJson() const {
    return json{
        {"name", name},
        {"callCount", callCount},
        {"totalMicroseconds", totalMicroseconds},
        {"minMicroseconds", minMicroseconds},
        {"maxMicroseconds", maxMicroseconds},
        {"avgMicroseconds", callCount > 0 ? (totalMicroseconds / callCount) : 0}
    };
}

FunctionTiming FunctionTiming::fromJson(const json& j) {
    FunctionTiming timing;
    timing.name = j.value("name", "");
    timing.callCount = j.value("callCount", 0u);
    timing.totalMicroseconds = j.value("totalMicroseconds", 0ull);
    timing.minMicroseconds = j.value("minMicroseconds", UINT64_MAX);
    timing.maxMicroseconds = j.value("maxMicroseconds", 0ull);
    return timing;
}

/**
 * TestPerformanceData JSON conversion
 */
json TestPerformanceData::toJson() const {
    json functionsArray = json::array();
    for (const auto& fn : functions) {
        functionsArray.push_back(fn.toJson());
    }

    return json{
        {"testName", testName},
        {"timestamp", timestamp},
        {"gitHash", gitHash},
        {"totalMicroseconds", totalMicroseconds},
        {"totalMilliseconds", totalMicroseconds / 1000.0},
        {"functions", functionsArray}
    };
}

TestPerformanceData TestPerformanceData::fromJson(const json& j) {
    TestPerformanceData data;
    data.testName = j.value("testName", "");
    data.timestamp = j.value("timestamp", "");
    data.gitHash = j.value("gitHash", "unknown");
    data.totalMicroseconds = j.value("totalMicroseconds", 0ull);

    if (j.contains("functions") && j["functions"].is_array()) {
        for (const auto& fn : j["functions"]) {
            data.functions.push_back(FunctionTiming::fromJson(fn));
        }
    }

    return data;
}

/**
 * Constructor
 */
TestPerformanceTracker::TestPerformanceTracker(const std::string& workspaceRoot)
    : workspaceRoot(workspaceRoot.empty() ? "." : workspaceRoot) {
    dataPath = getPersistencePath();
    loadData();
}

/**
 * Get persistence file path
 */
std::string TestPerformanceTracker::getPersistencePath() {
    std::string basePath;

#ifdef _WIN32
    const char* appData = std::getenv("APPDATA");
    basePath = appData ? appData : ".";
#elif __APPLE__
    const char* home = std::getenv("HOME");
    basePath = std::string(home ? home : ".") + "/Library/Application Support";
#else
    const char* home = std::getenv("HOME");
    basePath = std::string(home ? home : ".") + "/.local/share";
#endif

    fs::path mmemuPath = fs::path(basePath) / "mmemu";
    fs::create_directories(mmemuPath);

    return (mmemuPath / "test-performance.json").string();
}

/**
 * Load performance data from file
 */
void TestPerformanceTracker::loadData() {
    try {
        if (!fs::exists(dataPath)) {
            return;
        }

        std::ifstream file(dataPath);
        if (!file.is_open()) {
            return;
        }

        json data;
        file >> data;

        if (data.contains("tests") && data["tests"].is_object()) {
            for (auto& [testName, testData] : data["tests"].items()) {
                if (testData.is_array()) {
                    for (const auto& run : testData) {
                        testHistory[testName].push_back(TestPerformanceData::fromJson(run));
                    }
                }
            }
        }

        cleanupOldData();
    } catch (const std::exception& e) {
        // Failed to load - start fresh
        testHistory.clear();
    }
}

/**
 * Save performance data to file
 */
void TestPerformanceTracker::saveData() {
    try {
        json data;
        data["version"] = "1.0";
        data["workspace"] = workspaceRoot;
        data["tests"] = json::object();

        for (const auto& [testName, runs] : testHistory) {
            json runsArray = json::array();
            for (const auto& run : runs) {
                runsArray.push_back(run.toJson());
            }
            data["tests"][testName] = runsArray;
        }

        fs::create_directories(fs::path(dataPath).parent_path());

        std::ofstream file(dataPath);
        file << data.dump(2) << std::endl;
    } catch (const std::exception& e) {
        // Failed to save - silently continue
    }
}

/**
 * Start timing a test
 */
uint64_t TestPerformanceTracker::startTest(const std::string& testName) {
    uint64_t handle = nextSessionHandle++;

    TestTimingSession session;
    session.testName = testName;
    session.gitHash = getGitHash();
    session.startMicroseconds = getCurrentMicroseconds();

    activeSessions[handle] = session;
    return handle;
}

/**
 * Record a function call timing
 */
void TestPerformanceTracker::recordFunctionTiming(uint64_t testHandle,
                                                   const std::string& functionName,
                                                   uint64_t microseconds) {
    auto it = activeSessions.find(testHandle);
    if (it == activeSessions.end()) {
        return;
    }

    it->second.functionTimings[functionName].push_back(microseconds);
}

/**
 * End timing a test
 */
void TestPerformanceTracker::endTest(uint64_t testHandle) {
    auto it = activeSessions.find(testHandle);
    if (it == activeSessions.end()) {
        return;
    }

    auto& session = it->second;
    uint64_t endMicroseconds = getCurrentMicroseconds();
    uint64_t totalMicroseconds = endMicroseconds - session.startMicroseconds;

    TestPerformanceData data;
    data.testName = session.testName;
    data.timestamp = getCurrentTimestamp();
    data.gitHash = session.gitHash;
    data.totalMicroseconds = totalMicroseconds;

    // Aggregate function timings
    for (const auto& [funcName, timings] : session.functionTimings) {
        FunctionTiming funcTiming;
        funcTiming.name = funcName;
        funcTiming.callCount = timings.size();

        for (auto duration : timings) {
            funcTiming.totalMicroseconds += duration;
            funcTiming.minMicroseconds = std::min(funcTiming.minMicroseconds, duration);
            funcTiming.maxMicroseconds = std::max(funcTiming.maxMicroseconds, duration);
        }

        data.functions.push_back(funcTiming);
    }

    testHistory[session.testName].push_back(data);
    activeSessions.erase(it);

    saveData();
}

/**
 * Get performance data for a test
 */
std::vector<TestPerformanceData> TestPerformanceTracker::getTestPerformance(const std::string& testName) const {
    auto it = testHistory.find(testName);
    if (it == testHistory.end()) return {};
    return it->second;
}

/**
 * Get test history
 */
std::vector<TestPerformanceData> TestPerformanceTracker::getTestHistory(const std::string& testName) const {
    return getTestPerformance(testName);
}

/**
 * Export as JSON
 */
json TestPerformanceTracker::exportAsJson() const {
    json result;
    result["version"] = "1.0";
    result["timestamp"] = getCurrentTimestamp();
    result["tests"] = json::object();

    for (const auto& [testName, runs] : testHistory) {
        json runsArray = json::array();
        for (const auto& run : runs) {
            runsArray.push_back(run.toJson());
        }
        result["tests"][testName] = runsArray;
    }

    return result;
}

/**
 * Export as CSV
 */
std::string TestPerformanceTracker::exportAsCSV(const std::string& testName) const {
    auto it = testHistory.find(testName);
    if (it == testHistory.end()) {
        return "test_name,timestamp,git_hash,total_microseconds,total_milliseconds\n";
    }

    std::string csv = "test_name,timestamp,git_hash,total_microseconds,total_milliseconds\n";

    for (const auto& run : it->second) {
        csv += run.testName + "," + run.timestamp + "," + run.gitHash + "," +
               std::to_string(run.totalMicroseconds) + "," +
               std::to_string(run.totalMicroseconds / 1000.0) + "\n";
    }

    return csv;
}

/**
 * Clear all data
 */
void TestPerformanceTracker::clearAllData() {
    testHistory.clear();
    activeSessions.clear();
    saveData();
}

/**
 * Clear test data
 */
void TestPerformanceTracker::clearTestData(const std::string& testName) {
    testHistory.erase(testName);
    saveData();
}

/**
 * Get current git hash
 */
std::string TestPerformanceTracker::getGitHash() {
    std::array<char, 128> buffer;
    std::string result;

    FILE* pipe = popen("git rev-parse --short HEAD 2>/dev/null", "r");
    if (!pipe) return "unknown";

    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        result += buffer.data();
    }

    pclose(pipe);

    if (!result.empty() && result.back() == '\n') {
        result.pop_back();
    }

    return result.empty() ? "unknown" : result;
}

/**
 * Get current timestamp
 */
std::string TestPerformanceTracker::getCurrentTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);

    std::stringstream ss;
    ss << std::put_time(std::gmtime(&time), "%Y-%m-%dT%H:%M:%SZ");
    return ss.str();
}

/**
 * Get current time in microseconds
 */
uint64_t TestPerformanceTracker::getCurrentMicroseconds() {
    auto now = std::chrono::high_resolution_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
}

/**
 * Clean up old data
 */
void TestPerformanceTracker::cleanupOldData() {
    auto ninetyDaysAgo = std::chrono::system_clock::now() - std::chrono::hours(90 * 24);

    for (auto& [testName, runs] : testHistory) {
        auto it = runs.begin();
        while (it != runs.end()) {
            // Parse ISO timestamp (simplified)
            // Keep all data for now - proper timestamp parsing would be needed for filtering
            ++it;
        }
    }
}
