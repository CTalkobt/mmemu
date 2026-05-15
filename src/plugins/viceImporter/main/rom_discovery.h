#pragma once

#include <string>
#include <vector>

struct RomSource {
    std::string label;
    std::string basePath;
};

struct RomFileSpec {
    std::string srcRelPath;
    std::string destName;
    size_t expectedSize;
};

namespace vice_importer {
std::vector<RomSource> discoverSources(const std::string& machineId);
std::vector<RomSource> discoverSourcesInPaths(const std::string& machineId,
                                               const std::vector<std::string>& searchPaths);
std::vector<RomFileSpec> romFilesFor(const std::string& machineId);
}
