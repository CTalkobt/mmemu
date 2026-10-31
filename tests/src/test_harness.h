#pragma once

#include <iostream>
#include <vector>
#include <string>
#include <functional>
#include <memory>
#include <fnmatch.h>
#include <stdexcept>

// Simple hand-rolled test harness for mmemu
// Documentation: Add TEST_CASE("name") { ... } and use ASSERT(expr)

struct TestCase {
    std::string name;
    std::function<void()> func;
};

inline std::vector<TestCase>& getTestCases() {
    static std::vector<TestCase> cases;
    return cases;
}

struct TestRegistrar {
    TestRegistrar(const std::string& name, std::function<void()> func) {
        getTestCases().push_back({name, func});
    }
};

#define TEST_CASE(name) \
    void test_##name(); \
    static TestRegistrar registrar_##name(#name, test_##name); \
    void test_##name()

// Thrown by a failed assertion; the runner reports the test as failed and moves on
struct TestFailure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

#define ASSERT(expr) \
    if (!(expr)) { \
        std::cerr << "Assertion failed: " << #expr << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
        throw TestFailure("assertion failed"); \
    }

#define ASSERT_EQ(a, b) \
    if (!((a) == (b))) { \
        std::cerr << "Assertion failed: " << #a << " == " << #b \
                  << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
        throw TestFailure("assertion failed"); \
    }

#define ASSERT_NE(a, b) \
    if (!((a) != (b))) { \
        std::cerr << "Assertion failed: " << #a << " != " << #b \
                  << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
        throw TestFailure("assertion failed"); \
    }

// EXPECT_* mirrors ASSERT_* (both end the test on failure in this harness)
#define EXPECT_TRUE(expr)   ASSERT(expr)
#define EXPECT_EQ(a, b)     ASSERT_EQ(a, b)

#define EXPECT_NEAR(a, b, delta) \
    if (std::abs((a) - (b)) > (delta)) { \
        std::cerr << "Assertion failed: abs(" << #a << " - " << #b << ") <= " << #delta \
                  << " (val=" << (a) << ", expected=" << (b) << ")" \
                  << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
        throw TestFailure("assertion failed"); \
    }

// Forward declare performance tracker functions (avoiding TestPerformanceTracker inclusion)
extern void* createTestPerformanceTracker();
extern void deleteTestPerformanceTracker(void* tracker);
extern uint64_t startTestTracking(void* tracker, const std::string& testName);
extern void endTestTracking(void* tracker, uint64_t handle);

// True if no patterns were given, or the name matches any glob pattern (e.g. "*vic20*")
inline bool testSelected(const std::string& name, const std::vector<std::string>& patterns) {
    if (patterns.empty()) return true;
    for (const auto& p : patterns)
        if (fnmatch(p.c_str(), name.c_str(), 0) == 0) return true;
    return false;
}

inline int runAllTests(bool enablePerformance = false, const std::vector<std::string>& patterns = {}) {
    int passed = 0;
    auto& cases = getTestCases();
    std::vector<std::string> failed;
    size_t selected = 0;
    for (auto& tc : cases) if (testSelected(tc.name, patterns)) selected++;
    std::cout << "Running " << selected << " tests..." << std::endl;

    // Conditionally enable performance tracking
    void* tracker = nullptr;
    if (enablePerformance) {
        tracker = createTestPerformanceTracker();
    }

    for (auto& tc : cases) {
        if (!testSelected(tc.name, patterns)) continue;
        std::cout << "  [ RUN      ] " << tc.name << std::endl;

        uint64_t perfHandle = 0;
        if (tracker) {
            perfHandle = startTestTracking(tracker, tc.name);
        }

        bool ok = true;
        try {
            tc.func();
        } catch (const TestFailure&) {
            ok = false;
        } catch (const std::exception& e) {
            std::cerr << "Unexpected exception: " << e.what() << std::endl;
            ok = false;
        }

        if (tracker) {
            endTestTracking(tracker, perfHandle);
        }

        if (ok) {
            std::cout << "  [       OK ] " << tc.name << std::endl;
            passed++;
        } else {
            std::cout << "  [  FAILED  ] " << tc.name << std::endl;
            failed.push_back(tc.name);
        }
    }

    if (tracker) {
        std::cout << "\nPerformance data saved to: ~/.local/share/mmemu/test-performance.json" << std::endl;
        deleteTestPerformanceTracker(tracker);
    }

    std::cout << "Passed " << passed << " / " << selected << " tests." << std::endl;
    if (!failed.empty()) {
        std::cout << "Failed " << failed.size() << ":" << std::endl;
        for (const auto& name : failed) std::cout << "  " << name << std::endl;
        return 1;
    }
    return 0;
}

inline int runTest(const std::string& name) {
    auto& cases = getTestCases();
    for (auto& tc : cases) {
        if (tc.name == name) {
            std::cout << "Running test: " << name << std::endl;
            try {
                tc.func();
            } catch (const TestFailure&) {
                std::cout << "Test failed." << std::endl;
                return 1;
            }
            std::cout << "Test passed." << std::endl;
            return 0;
        }
    }
    std::cerr << "Test not found: " << name << std::endl;
    return 1;
}
