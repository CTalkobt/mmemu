#include "rom_discovery.h"
#include <filesystem>
#include <cstdlib>
#include <sstream>

namespace fs = std::filesystem;

namespace mega65_importer {

std::vector<RomFileSpec> romFilesFor(const std::string& machineId) {
    if (machineId == "mega65") {
        return {
            // Standard full ROM image (128 KB for basic/kernal overlay)
            { "MEGA65.ROM", "mega65.rom", 131072 }
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
        candidates.push_back(h + "/.local/share/xemu-lgb/mega65");
        candidates.push_back(h + "/Library/Application Support/xemu/mega65");
    }

    const char* appData = std::getenv("APPDATA");
    if (appData) {
        candidates.push_back(std::string(appData) + "\\xemu\\mega65");
    }

    return discoverSourcesInPaths(machineId, candidates);
}

std::string findXemuBinary() {
    std::vector<std::string> dirs;
    if (const char* path = std::getenv("PATH")) {
        std::stringstream ss(path);
        std::string dir;
        while (std::getline(ss, dir, ':'))
            if (!dir.empty()) dirs.push_back(dir);
    }
    dirs.push_back("/usr/local/bin");
    dirs.push_back("/usr/bin");
    if (const char* home = std::getenv("HOME"))
        dirs.push_back(std::string(home) + "/.local/bin");

    for (const auto& dir : dirs) {
        fs::path candidate = fs::path(dir) / "xemu-xmega65";
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec)) return candidate.string();
    }
    return "";
}

long findHickupImage(const std::vector<uint8_t>& blob) {
    constexpr size_t TRAP_ENTRIES = 64;
    if (blob.size() < HICKUP_SIZE) return -1;

    auto isTrapEntry = [&](size_t i) {
        return blob[i] == 0x4C && blob[i + 2] >= 0x80 && blob[i + 2] <= 0xBF && blob[i + 3] == 0xEA;
    };

    for (size_t pos = 0; pos + HICKUP_SIZE <= blob.size(); ++pos) {
        if (blob[pos] != 0x4C) continue;
        size_t n = 0;
        while (n < TRAP_ENTRIES && isTrapEntry(pos + n * 4)) ++n;
        if (n == TRAP_ENTRIES) return static_cast<long>(pos);
    }
    return -1;
}

} // namespace mega65_importer
