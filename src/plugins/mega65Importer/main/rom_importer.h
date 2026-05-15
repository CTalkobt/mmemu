#pragma once
#include <string>
#include <vector>

struct ImportResult {
    bool success;
    std::string message;
    std::vector<std::string> copiedFiles;
};

namespace mega65_importer {
ImportResult importRoms(const std::string& machineId, 
                        const std::string& sourcePath, 
                        const std::string& destPath, 
                        bool overwrite);
}
