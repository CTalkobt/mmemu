#pragma once
#include <string>
#include <vector>

namespace mega65_importer {

struct ImportResult {
    bool success;
    std::string message;
    std::vector<std::string> copiedFiles;
};

ImportResult importRoms(const std::string& machineId,
                        const std::string& sourcePath,
                        const std::string& destPath,
                        bool overwrite);

/**
 * Import the HYPPO hypervisor (HICKUP.M65) into destPath. A HICKUP.M65 file in
 * sourcePath is copied if present; otherwise the image embedded in the xemu
 * executable is extracted (xemuBinary, or auto-discovered when empty).
 */
ImportResult importHickup(const std::string& sourcePath,
                          const std::string& xemuBinary,
                          const std::string& destPath,
                          bool overwrite);
} // namespace mega65_importer
