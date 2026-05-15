#include "rom_importer.h"
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;

namespace vice_importer {

ImportResult importRoms(const RomSource& src, const std::string& machineId, const std::string& destDir, bool overwrite) {
    ImportResult result;
    result.success = true;

    auto specs = romFilesFor(machineId);
    if (specs.empty()) {
        result.success = false;
        result.errorMessage = "No ROM specs for machine: " + machineId;
        return result;
    }

    if (!fs::exists(destDir)) {
        fs::create_directories(destDir);
    }

    std::vector<std::string> copied;
    for (const auto& spec : specs) {
        fs::path srcPath = fs::path(src.basePath) / spec.srcRelPath;
        fs::path dstPath = fs::path(destDir) / spec.destName;

        if (fs::exists(dstPath) && !overwrite) {
            result.success = false;
            result.errorMessage = "Destination file already exists: " + spec.destName;
            // Rollback
            for (const auto& f : copied) fs::remove(fs::path(destDir) / f);
            return result;
        }

        try {
            if (!fs::exists(srcPath)) {
                result.success = false;
                result.errorMessage = "Source file not found: " + srcPath.string();
                for (const auto& f : copied) fs::remove(fs::path(destDir) / f);
                return result;
            }

            if (fs::file_size(srcPath) != spec.expectedSize) {
                result.success = false;
                result.errorMessage = "Source file size mismatch: " + srcPath.string();
                for (const auto& f : copied) fs::remove(fs::path(destDir) / f);
                return result;
            }

            fs::copy_file(srcPath, dstPath, fs::copy_options::overwrite_existing);
            copied.push_back(spec.destName);
        } catch (const fs::filesystem_error& e) {
            result.success = false;
            result.errorMessage = e.what();
            for (const auto& f : copied) fs::remove(fs::path(destDir) / f);
            return result;
        }
    }

    result.copiedFiles = copied;
    return result;
}

} // namespace vice_importer
