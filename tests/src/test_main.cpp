#include "test_harness.h"
#include "include/mmemu_plugin_api.h"
#include "include/util/logging.h"
#include <string>
#include <cstring>

int main(int argc, char* argv[]) {
    LogRegistry::instance().init();

    // Check for -performance flag
    bool enablePerformance = false;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "-performance") == 0) {
            enablePerformance = true;
            break;
        }
    }

    return runAllTests(enablePerformance);
}
