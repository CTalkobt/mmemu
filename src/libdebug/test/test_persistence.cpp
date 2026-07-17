#include "test_harness.h"
#include "libdebug/main/test_persistence.h"
#include <filesystem>
#include <fstream>
#include <algorithm>

namespace fs = std::filesystem;

TEST_CASE(test_persistence_record_and_retrieve) {
    TestPersistence persistence;
    persistence.clearTestHistory("pers_test_1");

    TestRunResult result1;
    result1.status = "pass";
    result1.duration = 42;
    result1.gitHash = "abc1234";
    result1.timestamp = "2026-07-16T10:00:00Z";

    bool changed = persistence.recordTestRun("pers_test_1", "test.lua", result1);
    ASSERT(changed == true);

    auto history = persistence.getTestHistory("pers_test_1");
    ASSERT(history.size() == 1);
    ASSERT(history[0].status == "pass");
    ASSERT(history[0].duration == 42);
}

TEST_CASE(test_persistence_track_status_changes) {
    TestPersistence persistence;
    persistence.clearTestHistory("pers_test_2");

    TestRunResult pass;
    pass.status = "pass";
    pass.duration = 40;
    pass.gitHash = "abc1111";
    pass.timestamp = "2026-07-16T10:00:00Z";

    TestRunResult fail;
    fail.status = "fail";
    fail.duration = 120;
    fail.error = "Assertion failed";
    fail.gitHash = "abc2222";
    fail.timestamp = "2026-07-16T10:01:00Z";

    bool changed1 = persistence.recordTestRun("pers_test_2", "test.lua", pass);
    ASSERT(changed1 == true);
    ASSERT(persistence.getStatusChangeCount("pers_test_2") == 0);

    bool changed2 = persistence.recordTestRun("pers_test_2", "test.lua", fail);
    ASSERT(changed2 == true);
    ASSERT(persistence.getStatusChangeCount("pers_test_2") == 1);

    bool changed3 = persistence.recordTestRun("pers_test_2", "test.lua", fail);
    ASSERT(changed3 == false);
    ASSERT(persistence.getStatusChangeCount("pers_test_2") == 1);

    bool changed4 = persistence.recordTestRun("pers_test_2", "test.lua", pass);
    ASSERT(changed4 == true);
    ASSERT(persistence.getStatusChangeCount("pers_test_2") == 2);
}

TEST_CASE(test_persistence_get_status_info) {
    TestPersistence persistence;
    persistence.clearTestHistory("pers_test_3");

    TestRunResult result;
    result.status = "pass";
    result.duration = 42;
    result.gitHash = "a6e0156";
    result.timestamp = "2026-07-16T10:00:00Z";

    persistence.recordTestRun("pers_test_3", "test.lua", result);

    ASSERT(persistence.getLastStatus("pers_test_3") == "pass");
    ASSERT(persistence.getLastGitHash("pers_test_3") == "a6e0156");
    ASSERT(persistence.getStatusChangeCount("pers_test_3") == 0);
}

TEST_CASE(test_persistence_csv_export) {
    TestPersistence persistence;
    persistence.clearTestHistory("pers_test_4");

    TestRunResult result1;
    result1.status = "pass";
    result1.duration = 42;
    result1.gitHash = "a6e0156";
    result1.timestamp = "2026-07-16T10:00:00Z";
    result1.error = "";

    TestRunResult result2;
    result2.status = "fail";
    result2.duration = 120;
    result2.gitHash = "a6e0157";
    result2.timestamp = "2026-07-16T10:01:00Z";
    result2.error = "Assertion failed";

    persistence.recordTestRun("pers_test_4", "test.lua", result1);
    persistence.recordTestRun("pers_test_4", "test.lua", result2);

    std::string csv = persistence.exportAsCSV("pers_test_4");
    ASSERT(csv.find("timestamp,git_hash,status,duration_ms,error") != std::string::npos);
    ASSERT(csv.find("pass") != std::string::npos);
    ASSERT(csv.find("fail") != std::string::npos);
    ASSERT(csv.find("a6e0156") != std::string::npos);
    ASSERT(csv.find("a6e0157") != std::string::npos);
}

TEST_CASE(test_persistence_multiple_tests) {
    TestPersistence persistence;
    persistence.clearTestHistory("pers_test_mem");
    persistence.clearTestHistory("pers_test_reg");

    TestRunResult result1;
    result1.status = "pass";
    result1.duration = 42;
    result1.gitHash = "abc1111";
    result1.timestamp = "2026-07-16T10:00:00Z";

    TestRunResult result2;
    result2.status = "pass";
    result2.duration = 58;
    result2.gitHash = "abc1111";
    result2.timestamp = "2026-07-16T10:00:00Z";

    persistence.recordTestRun("pers_test_mem", "test.lua", result1);
    persistence.recordTestRun("pers_test_reg", "test.lua", result2);

    auto names = persistence.getTestNames();
    ASSERT(std::find(names.begin(), names.end(), std::string("pers_test_mem")) != names.end());
    ASSERT(std::find(names.begin(), names.end(), std::string("pers_test_reg")) != names.end());

    std::string allCsv = persistence.exportAllAsCSV();
    ASSERT(allCsv.find("pers_test_mem") != std::string::npos);
    ASSERT(allCsv.find("pers_test_reg") != std::string::npos);
}

TEST_CASE(test_persistence_file_path) {
    std::string path = TestPersistence::getPersistencePath();
    ASSERT(!path.empty());
    ASSERT(path.find("mmemu") != std::string::npos);
    ASSERT(path.find("test-results.json") != std::string::npos);
}

TEST_CASE(test_persistence_clear_history) {
    TestPersistence persistence;

    TestRunResult result;
    result.status = "pass";
    result.duration = 42;
    result.gitHash = "abc1234";
    result.timestamp = "2026-07-16T10:00:00Z";

    persistence.recordTestRun("pers_test_clear", "test.lua", result);
    ASSERT(persistence.getTestHistory("pers_test_clear").size() == 1);

    persistence.clearTestHistory("pers_test_clear");
    ASSERT(persistence.getTestHistory("pers_test_clear").size() == 0);
}
