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

/** Size of the HYPPO hypervisor image (HICKUP.M65). */
constexpr size_t HICKUP_SIZE = 16384;

/**
 * Locate an installed xemu-xmega65 executable (searches $PATH, then common
 * install prefixes). Returns an empty string if none is found.
 */
std::string findXemuBinary();

/**
 * Find the embedded HICKUP.M65 image inside a blob (e.g. the xemu-xmega65
 * executable). HYPPO starts with its trap table: 64 entries of
 * "JMP $8000-$BFFF ; NOP" for traps $00-$3F. Returns the offset of the image,
 * or -1 if no complete 16KB image is found.
 */
long findHickupImage(const std::vector<uint8_t>& blob);
}
