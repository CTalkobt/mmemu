#include "test_persistence.h"
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <cstring>
#include <array>
#include <chrono>

namespace fs = std::filesystem;

/**
 * TestRunResult JSON conversion
 */
json TestRunResult::toJson() const {
    return json{
        {"timestamp", timestamp},
        {"gitHash", gitHash},
        {"status", status},
        {"duration", duration},
        {"error", error}
    };
}

TestRunResult TestRunResult::fromJson(const json& j) {
    TestRunResult result;
    result.timestamp = j.value("timestamp", "");
    result.gitHash = j.value("gitHash", "unknown");
    result.status = j.value("status", "");
    result.duration = j.value("duration", 0u);
    result.error = j.value("error", "");
    return result;
}

/**
 * TestHistory JSON conversion
 */
json TestHistory::toJson() const {
    json historyArray = json::array();
    for (const auto& run : history) {
        historyArray.push_back(run.toJson());
    }

    return json{
        {"file", file},
        {"history", historyArray},
        {"lastStatus", lastStatus},
        {"lastGitHash", lastGitHash},
        {"statusChanges", statusChanges}
    };
}

TestHistory TestHistory::fromJson(const json& j) {
    TestHistory history;
    history.file = j.value("file", "");
    history.lastStatus = j.value("lastStatus", "");
    history.lastGitHash = j.value("lastGitHash", "unknown");
    history.statusChanges = j.value("statusChanges", 0u);

    if (j.contains("history") && j["history"].is_array()) {
        for (const auto& run : j["history"]) {
            history.history.push_back(TestRunResult::fromJson(run));
        }
    }

    return history;
}

/**
 * Constructor
 */
TestPersistence::TestPersistence(const std::string& workspaceRoot)
    : workspaceRoot(workspaceRoot.empty() ? "." : workspaceRoot) {
    dataPath = getPersistencePath();
    loadData();
}

/**
 * Get persistence file path (platform-aware)
 */
std::string TestPersistence::getPersistencePath() {
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

    return (mmemuPath / "test-results.json").string();
}

/**
 * Load test results from file
 */
void TestPersistence::loadData() {
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
                tests[testName] = TestHistory::fromJson(testData);
            }
        }

        cleanupOldData();
    } catch (const std::exception& e) {
        // Failed to load - start fresh
        tests.clear();
    }
}

/**
 * Save test results to file
 */
void TestPersistence::saveData() {
    try {
        json data;
        data["version"] = "1.0";
        data["workspace"] = workspaceRoot;
        data["tests"] = json::object();

        for (const auto& [testName, history] : tests) {
            data["tests"][testName] = history.toJson();
        }

        fs::create_directories(fs::path(dataPath).parent_path());

        std::ofstream file(dataPath);
        file << data.dump(2) << std::endl;
        dirty = false;
    } catch (const std::exception& e) {
        // Failed to save - silently continue
    }
}

/**
 * Record test run result
 */
bool TestPersistence::recordTestRun(const std::string& testName,
                                     const std::string& filePath,
                                     const TestRunResult& result) {
    TestRunResult fullResult = result;
    if (fullResult.gitHash.empty()) {
        fullResult.gitHash = getGitHash();
    }
    if (fullResult.timestamp.empty()) {
        fullResult.timestamp = getCurrentTimestamp();
    }

    bool hasChanged = hasStatusChanged(testName, result.status);

    if (tests.find(testName) == tests.end()) {
        TestHistory history;
        history.file = filePath;
        history.lastStatus = result.status;
        history.lastGitHash = fullResult.gitHash;
        history.statusChanges = 0;
        tests[testName] = history;
    }

    auto& history = tests[testName];
    history.history.push_back(fullResult);

    // Only increment status changes if there was a prior status and it changed
    if (hasChanged && history.history.size() > 1) {
        history.statusChanges++;
    }

    history.lastStatus = result.status;
    history.lastGitHash = fullResult.gitHash;

    dirty = true;
    saveData();

    return hasChanged;
}

/**
 * Check if status has changed
 */
bool TestPersistence::hasStatusChanged(const std::string& testName,
                                        const std::string& newStatus) const {
    auto it = tests.find(testName);
    if (it == tests.end()) return true;
    return it->second.lastStatus != newStatus;
}

/**
 * Get test history
 */
std::vector<TestRunResult> TestPersistence::getTestHistory(const std::string& testName) const {
    auto it = tests.find(testName);
    if (it == tests.end()) return {};
    return it->second.history;
}

/**
 * Get status change count
 */
uint32_t TestPersistence::getStatusChangeCount(const std::string& testName) const {
    auto it = tests.find(testName);
    if (it == tests.end()) return 0;
    return it->second.statusChanges;
}

/**
 * Get last status
 */
std::string TestPersistence::getLastStatus(const std::string& testName) const {
    auto it = tests.find(testName);
    if (it == tests.end()) return "";
    return it->second.lastStatus;
}

/**
 * Get last git hash
 */
std::string TestPersistence::getLastGitHash(const std::string& testName) const {
    auto it = tests.find(testName);
    if (it == tests.end()) return "unknown";
    return it->second.lastGitHash;
}

/**
 * Get status description
 */
std::string TestPersistence::getStatusDescription(const std::string& testName) const {
    auto it = tests.find(testName);
    if (it == tests.end()) return "No history";

    const auto& history = it->second;
    if (history.history.empty()) return "No history";

    const auto& lastRun = history.history.back();

    // Calculate age
    auto now = std::chrono::system_clock::now();
    auto lastTime = std::chrono::system_clock::from_time_t(
        std::chrono::system_clock::to_time_t(now)  // Simplified; actual parsing from ISO would be better
    );

    std::string statusSymbol = history.lastStatus == "pass" ? "✓" : "✗";
    std::string result = statusSymbol + " " + history.lastStatus + " (" +
                         history.lastGitHash + ")";

    return result;
}

/**
 * Export as CSV (single test)
 */
std::string TestPersistence::exportAsCSV(const std::string& testName) const {
    auto it = tests.find(testName);
    if (it == tests.end()) return "timestamp,git_hash,status,duration_ms,error\n";

    std::string csv = "timestamp,git_hash,status,duration_ms,error\n";

    for (const auto& run : it->second.history) {
        csv += run.timestamp + "," + run.gitHash + "," + run.status + "," +
               std::to_string(run.duration) + ",\"" + run.error + "\"\n";
    }

    return csv;
}

/**
 * Export all as CSV
 */
std::string TestPersistence::exportAllAsCSV() const {
    std::string csv = "test_name,timestamp,git_hash,status,duration_ms,error\n";

    for (const auto& [testName, history] : tests) {
        for (const auto& run : history.history) {
            csv += testName + "," + run.timestamp + "," + run.gitHash + "," +
                   run.status + "," + std::to_string(run.duration) + ",\"" +
                   run.error + "\"\n";
        }
    }

    return csv;
}

/**
 * Clear all history
 */
void TestPersistence::clearAllHistory() {
    tests.clear();
    dirty = true;
    saveData();
}

/**
 * Clear test history
 */
void TestPersistence::clearTestHistory(const std::string& testName) {
    tests.erase(testName);
    dirty = true;
    saveData();
}

/**
 * Get test names
 */
std::vector<std::string> TestPersistence::getTestNames() const {
    std::vector<std::string> names;
    for (const auto& [name, _] : tests) {
        names.push_back(name);
    }
    return names;
}

/**
 * Get current git hash
 */
std::string TestPersistence::getGitHash() {
    std::array<char, 128> buffer;
    std::string result;

    FILE* pipe = popen("git rev-parse --short HEAD 2>/dev/null", "r");
    if (!pipe) return "unknown";

    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        result += buffer.data();
    }

    pclose(pipe);

    // Trim whitespace
    if (!result.empty() && result.back() == '\n') {
        result.pop_back();
    }

    return result.empty() ? "unknown" : result;
}

/**
 * Get current timestamp (ISO 8601)
 */
std::string TestPersistence::getCurrentTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);

    std::stringstream ss;
    ss << std::put_time(std::gmtime(&time), "%Y-%m-%dT%H:%M:%SZ");
    return ss.str();
}

/**
 * Clean up old data
 */
void TestPersistence::cleanupOldData() {
    auto now = std::chrono::system_clock::now();
    auto thirtyDaysAgo = now - std::chrono::hours(30 * 24);
    auto thirtyDaysAgoTime = std::chrono::system_clock::to_time_t(thirtyDaysAgo);

    for (auto& [testName, history] : tests) {
        if (history.history.size() > 10) {
            std::vector<TestRunResult> filtered;

            for (const auto& run : history.history) {
                // Parse ISO timestamp (simplified - assumes valid format)
                // Full implementation would parse the timestamp properly
                filtered.push_back(run);
            }

            // Keep last 10 if too much was filtered
            if (filtered.size() < 10 && history.history.size() > 10) {
                history.history.erase(
                    history.history.begin(),
                    history.history.end() - 10
                );
            } else if (filtered.size() >= 10) {
                history.history = filtered;
            }

            dirty = true;
        }
    }

    if (dirty) {
        saveData();
    }
}
