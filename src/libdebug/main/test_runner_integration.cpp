#include "test_runner_integration.h"
#include "test_performance_tracker.h"
#include <iostream>
#include <chrono>
#include <iomanip>
#include <sstream>

std::shared_ptr<TestPersistence> TestRunnerIntegration::persistence = nullptr;
bool TestRunnerIntegration::enabled = true;

void TestRunnerIntegration::initialize(const std::string& workspaceRoot) {
    if (!persistence) {
        persistence = std::make_shared<TestPersistence>(workspaceRoot);
    }
}

bool TestRunnerIntegration::recordTestResult(const std::string& testName,
                                              const std::string& testFile,
                                              bool passed,
                                              uint32_t duration,
                                              const std::string& error) {
    if (!enabled || !persistence) {
        return false;
    }

    TestRunResult result;
    result.status = passed ? "pass" : "fail";
    result.duration = duration;
    result.error = error;
    result.timestamp = TestPersistence::getCurrentTimestamp();

    return persistence->recordTestRun(testName, testFile, result);
}

std::shared_ptr<TestPersistence> TestRunnerIntegration::getPersistence() {
    if (!persistence) {
        persistence = std::make_shared<TestPersistence>();
    }
    return persistence;
}

void TestRunnerIntegration::printSummary() {
    if (!persistence) return;

    const auto& names = persistence->getTestNames();
    if (names.empty()) return;

    std::cout << "\n" << std::string(60, '=') << "\n";
    std::cout << "Test Persistence Summary\n";
    std::cout << std::string(60, '=') << "\n";

    for (const auto& testName : names) {
        const auto& history = persistence->getTestHistory(testName);
        if (history.empty()) continue;

        const auto& lastRun = history.back();
        uint32_t changes = persistence->getStatusChangeCount(testName);

        std::cout << "\n" << testName << "\n";
        std::cout << "  Status: " << lastRun.status << " (git: " << lastRun.gitHash << ")\n";
        std::cout << "  Duration: " << lastRun.duration << "ms\n";
        std::cout << "  Total runs: " << history.size() << "\n";
        std::cout << "  Status changes: " << changes << "\n";

        if (!lastRun.error.empty()) {
            std::cout << "  Error: " << lastRun.error << "\n";
        }
    }

    std::cout << "\nPersistence file: " << TestPersistence::getPersistencePath() << "\n";
    std::cout << std::string(60, '=') << "\n";
}

void TestRunnerIntegration::setEnabled(bool isEnabled) {
    enabled = isEnabled;
}

bool TestRunnerIntegration::isEnabled() {
    return enabled;
}

TestPerformanceTracker* TestRunnerIntegration::createPerformanceTracker() {
    return new TestPerformanceTracker();
}

// Export for test harness (using void* to avoid header inclusion)
void* createTestPerformanceTracker() {
    return TestRunnerIntegration::createPerformanceTracker();
}

void deleteTestPerformanceTracker(void* tracker) {
    delete static_cast<TestPerformanceTracker*>(tracker);
}

uint64_t startTestTracking(void* tracker, const std::string& testName) {
    return static_cast<TestPerformanceTracker*>(tracker)->startTest(testName);
}

void endTestTracking(void* tracker, uint64_t handle) {
    static_cast<TestPerformanceTracker*>(tracker)->endTest(handle);
}
