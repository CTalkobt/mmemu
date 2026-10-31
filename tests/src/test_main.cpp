#include "test_harness.h"
#include "include/mmemu_plugin_api.h"
#include "include/util/logging.h"
#include <string>
#include <cstring>

int main(int argc, char* argv[]) {
    LogRegistry::instance().init();

    // -performance enables tracking; other arguments are test name globs ("*vic20*")
    bool enablePerformance = false;
    std::vector<std::string> patterns;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "-performance") == 0) enablePerformance = true;
        else patterns.push_back(argv[i]);
    }

    return runAllTests(enablePerformance, patterns);
}
