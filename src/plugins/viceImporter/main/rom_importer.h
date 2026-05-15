#pragma once

#include "rom_discovery.h"
#include <string>
#include <vector>

struct ImportResult {
    bool success;
    std::vector<std::string> copiedFiles;
    std::string errorMessage;
};

namespace vice_importer {
ImportResult importRoms(const RomSource& src, const std::string& machineId, const std::string& destDir, bool overwrite = false);
}
