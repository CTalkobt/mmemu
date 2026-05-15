#include "rom_discovery.h"
#include <filesystem>
#include <cstdlib>

namespace fs = std::filesystem;

namespace mega65_importer {

std::vector<RomFileSpec> romFilesFor(const std::string& machineId) {
    if (machineId == "mega65") {
        return {
            // Standard full ROM image used by xemu and real hardware
            { "MEGA65.ROM", "mega65.rom", 786432 }
        };
    }
    return {};
}

std::vector<RomSource> discoverSourcesInPaths(const std::string& machineId,
                                               const std::vector<std::string>& searchPaths) {
    std::vector<RomSource> found;
    auto specs = romFilesFor(machineId);
    if (specs.empty()) return found;

    for (const auto& path : searchPaths) {
        if (!fs::exists(path)) continue;

        bool allPresent = true;
        for (const auto& spec : specs) {
            fs::path fullPath = fs::path(path) / spec.srcRelPath;
            if (!fs::exists(fullPath) || fs::file_size(fullPath) != spec.expectedSize) {
                // Try lowercase if uppercase fails
                std::string lowerRel = spec.srcRelPath;
                for (char &c : lowerRel) c = std::tolower(c);
                fullPath = fs::path(path) / lowerRel;
                
                if (!fs::exists(fullPath) || fs::file_size(fullPath) != spec.expectedSize) {
                    allPresent = false;
                    break;
                }
            }
        }

        if (allPresent) {
            std::string label = "Source at " + path;
            if (path.find("xemu") != std::string::npos) label = "xemu installation at " + path;
            found.push_back({ label, path });
        }
    }

    return found;
}

std::vector<RomSource> discoverSources(const std::string& machineId) {
    std::vector<std::string> candidates = {
        ".",
        "roms/mega65"
    };

    const char* home = std::getenv("HOME");
    if (home) {
        std::string h(home);
        candidates.push_back(h + "/.local/share/xemu/mega65");
        candidates.push_back(h + "/Library/Application Support/xemu/mega65");
    }

    const char* appData = std::getenv("APPDATA");
    if (appData) {
        candidates.push_back(std::string(appData) + "\\xemu\\mega65");
    }

    return discoverSourcesInPaths(machineId, candidates);
}

} // namespace mega65_importer
