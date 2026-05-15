#pragma once
#include <string>
#include <vector>
#include <cstdint>

struct RomFileSpec {
    std::string srcRelPath;
    std::string destName;
    size_t      expectedSize;
};

struct RomSource {
    std::string label;
    std::string basePath;
};

namespace mega65_importer {
std::vector<RomFileSpec> romFilesFor(const std::string& machineId);
std::vector<RomSource>   discoverSources(const std::string& machineId);
std::vector<RomSource>   discoverSourcesInPaths(const std::string& machineId, 
                                                const std::vector<std::string>& paths);
}
