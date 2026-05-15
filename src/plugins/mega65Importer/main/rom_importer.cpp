#include "rom_importer.h"
#include "rom_discovery.h"
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace mega65_importer {

ImportResult importRoms(const std::string& machineId, 
                        const std::string& sourcePath, 
                        const std::string& destPath, 
                        bool overwrite) {
    ImportResult result;
    result.success = true;

    auto specs = romFilesFor(machineId);
    if (specs.empty()) {
        result.success = false;
        result.message = "No ROM specs for machine " + machineId;
        return result;
    }

    if (!fs::exists(destPath)) {
        if (!fs::create_directories(destPath)) {
            result.success = false;
            result.message = "Could not create destination directory " + destPath;
            return result;
        }
    }

    for (const auto& spec : specs) {
        fs::path src = fs::path(sourcePath) / spec.srcRelPath;
        if (!fs::exists(src)) {
            // Try lowercase
            std::string lowerRel = spec.srcRelPath;
            for (char &c : lowerRel) c = std::tolower(c);
            src = fs::path(sourcePath) / lowerRel;
        }

        if (!fs::exists(src)) {
            result.success = false;
            result.message = "Source file missing: " + spec.srcRelPath;
            return result;
        }

        fs::path dest = fs::path(destPath) / spec.destName;
        if (fs::exists(dest) && !overwrite) {
            continue; // Skip existing
        }

        try {
            fs::copy_file(src, dest, fs::copy_options::overwrite_existing);
            result.copiedFiles.push_back(spec.destName);
        } catch (const fs::filesystem_error& e) {
            result.success = false;
            result.message = "Error copying " + spec.srcRelPath + ": " + e.what();
            return result;
        }
    }

    if (result.copiedFiles.empty()) {
        result.message = "All ROMs already present.";
    } else {
        result.message = "Imported " + std::to_string(result.copiedFiles.size()) + " ROM files.";
    }

    return result;
}

} // namespace mega65_importer
