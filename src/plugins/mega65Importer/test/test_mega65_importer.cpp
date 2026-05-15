#include "test_harness.h"
#include "plugins/mega65Importer/main/rom_discovery.h"
#include "plugins/mega65Importer/main/rom_importer.h"
#include <filesystem>
#include <fstream>
#include <vector>
#include <string>
#include <cstdint>

namespace fs = std::filesystem;
using namespace mega65_importer;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void writeFile(const fs::path& path, size_t size, uint8_t fill = 0xAA) {
    fs::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::binary);
    std::vector<uint8_t> buf(size, fill);
    f.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(size));
}

static fs::path makeFakeMega65Install() {
    fs::path base = fs::temp_directory_path() / "mmemu_test_mega65_xemu";
    fs::remove_all(base);
    fs::create_directories(base);

    for (const auto& spec : romFilesFor("mega65")) {
        writeFile(base / spec.srcRelPath, spec.expectedSize);
    }
    return base;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_CASE(mega65_importer_rom_files) {
    auto specs = romFilesFor("mega65");
    ASSERT(specs.size() == 1);
    ASSERT(specs[0].destName == "mega65.rom");
    ASSERT(specs[0].expectedSize == 786432);
}

TEST_CASE(mega65_importer_discover_mock) {
    fs::path install = makeFakeMega65Install();

    auto sources = discoverSourcesInPaths("mega65", { install.string() });
    ASSERT(sources.size() == 1);
    ASSERT(sources[0].basePath == install.string());

    fs::remove_all(install);
}

TEST_CASE(mega65_importer_discover_wrong_size) {
    fs::path base = fs::temp_directory_path() / "mmemu_test_mega65_wrongsize";
    fs::remove_all(base);
    fs::create_directories(base);

    for (const auto& spec : romFilesFor("mega65")) {
        writeFile(base / spec.srcRelPath, spec.expectedSize - 1);
    }

    auto sources = discoverSourcesInPaths("mega65", { base.string() });
    ASSERT(sources.empty());

    fs::remove_all(base);
}

TEST_CASE(mega65_importer_import_copies) {
    fs::path install = makeFakeMega65Install();
    fs::path dest    = fs::temp_directory_path() / "mmemu_test_mega65_dest";
    fs::remove_all(dest);

    auto result = importRoms("mega65", install.string(), dest.string(), false);

    ASSERT(result.success);
    ASSERT(result.copiedFiles.size() == 1);

    for (const auto& spec : romFilesFor("mega65")) {
        ASSERT(fs::exists(dest / spec.destName));
        ASSERT(fs::file_size(dest / spec.destName) == spec.expectedSize);
    }

    fs::remove_all(install);
    fs::remove_all(dest);
}

TEST_CASE(mega65_importer_no_overwrite) {
    fs::path install = makeFakeMega65Install();
    fs::path dest    = fs::temp_directory_path() / "mmemu_test_mega65_no_ow";
    fs::remove_all(dest);
    fs::create_directories(dest);

    const RomFileSpec spec = romFilesFor("mega65")[0];
    writeFile(dest / spec.destName, spec.expectedSize, 0x00);

    auto result = importRoms("mega65", install.string(), dest.string(), false);

    ASSERT(result.success);
    ASSERT(result.copiedFiles.empty()); // None copied because existing

    // Content should still be 0x00
    std::ifstream f(dest / spec.destName, std::ios::binary);
    uint8_t byte = 0xFF;
    f.read(reinterpret_cast<char*>(&byte), 1);
    ASSERT(byte == 0x00);

    fs::remove_all(install);
    fs::remove_all(dest);
}
