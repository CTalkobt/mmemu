#include "rom_importer.h"
#include "rom_discovery.h"
#include <filesystem>
#include <fstream>
#include <iterator>

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

ImportResult importHickup(const std::string& sourcePath,
                          const std::string& xemuBinary,
                          const std::string& destPath,
                          bool overwrite) {
    ImportResult result;
    result.success = true;

    fs::path dest = fs::path(destPath) / "HICKUP.M65";
    if (fs::exists(dest) && !overwrite) {
        result.message = "HICKUP.M65 already present.";
        return result;
    }

    std::error_code ec;
    fs::create_directories(destPath, ec);

    // Prefer a standalone HICKUP.M65 in the source directory
    if (!sourcePath.empty()) {
        fs::path src = fs::path(sourcePath) / "HICKUP.M65";
        if (fs::exists(src) && fs::file_size(src) == HICKUP_SIZE) {
            try {
                fs::copy_file(src, dest, fs::copy_options::overwrite_existing);
            } catch (const fs::filesystem_error& e) {
                result.success = false;
                result.message = std::string("Error copying HICKUP.M65: ") + e.what();
                return result;
            }
            result.copiedFiles.push_back("HICKUP.M65");
            result.message = "Copied HICKUP.M65 from " + src.string();
            return result;
        }
    }

    // Otherwise extract the copy built into xemu-xmega65
    std::string binary = xemuBinary.empty() ? findXemuBinary() : xemuBinary;
    if (binary.empty()) {
        result.success = false;
        result.message = "No HICKUP.M65 found and xemu-xmega65 is not installed.";
        return result;
    }

    std::ifstream in(binary, std::ios::binary);
    if (!in) {
        result.success = false;
        result.message = "Cannot read " + binary;
        return result;
    }
    std::vector<uint8_t> blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    long offset = findHickupImage(blob);
    if (offset < 0) {
        result.success = false;
        result.message = "No HYPPO image found in " + binary;
        return result;
    }

    std::ofstream out(dest, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(blob.data() + offset), HICKUP_SIZE);
    if (!out) {
        result.success = false;
        result.message = "Error writing " + dest.string();
        return result;
    }
    result.copiedFiles.push_back("HICKUP.M65");
    result.message = "Extracted HICKUP.M65 from " + binary;
    return result;
}

} // namespace mega65_importer
