#include <filesystem>
#include <string>
#include <cstdlib>
#include <unistd.h>
#include "test_harness.h"
#include "include/mmemu_plugin_api.h"
#include "include/util/logging.h"
#include <string>
#include <cstring>

int main(int argc, char* argv[]) {
    LogRegistry::instance().init();
    // Machines that keep settings (the IIgs's battery RAM) keep them in a
    // scratch folder, not the user's
    {
        static std::string dir = (std::filesystem::temp_directory_path() / ("mmemu-test-data-" + std::to_string(::getpid()))).string();
        std::filesystem::create_directories(dir);
        ::setenv("MMEMU_DATA_DIR", dir.c_str(), 1);
    }

    // -performance enables tracking; other arguments are test name globs ("*vic20*")
    bool enablePerformance = false;
    std::vector<std::string> patterns;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "-performance") == 0) enablePerformance = true;
        else patterns.push_back(argv[i]);
    }

    return runAllTests(enablePerformance, patterns);
}
