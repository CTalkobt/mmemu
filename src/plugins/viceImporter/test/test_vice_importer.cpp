#include "test_harness.h"
#include "plugins/viceImporter/main/rom_discovery.h"
#include "plugins/viceImporter/main/rom_importer.h"
#include <filesystem>
#include <fstream>
#include <vector>
#include <string>
#include <cstdint>

namespace fs = std::filesystem;
using namespace vice_importer;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void writeFile(const fs::path& path, size_t size, uint8_t fill = 0xAA) {
    fs::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::binary);
    std::vector<uint8_t> buf(size, fill);
    f.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(size));
}

// Create a temporary directory tree that looks like a valid VICE installation
// for the given machineId, using the ROM file specs as a guide.
static fs::path makeFakeViceInstall(const std::string& machineId) {
    fs::path base = fs::temp_directory_path() / ("mmemu_test_vice_" + machineId);
    fs::remove_all(base);
    fs::create_directories(base);

    for (const auto& spec : romFilesFor(machineId)) {
        writeFile(base / spec.srcRelPath, spec.expectedSize);
    }
    return base;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_CASE(vice_importer_rom_files_vic20) {
    auto specs = romFilesFor("vic20");
    ASSERT(specs.size() == 3);

    bool hasBasic = false, hasKernal = false, hasChar = false;
    for (const auto& s : specs) {
        if (s.destName == "basic.bin")  { ASSERT(s.expectedSize == 8192); hasBasic  = true; }
        if (s.destName == "kernal.bin") { ASSERT(s.expectedSize == 8192); hasKernal = true; }
        if (s.destName == "char.bin")   { ASSERT(s.expectedSize == 4096); hasChar   = true; }
    }
    ASSERT(hasBasic && hasKernal && hasChar);
}

TEST_CASE(vice_importer_rom_files_unknown) {
    ASSERT(romFilesFor("unknownmachine").empty());
}

TEST_CASE(vice_importer_discover_mock_filesystem) {
    fs::path install = makeFakeViceInstall("vic20");

    auto sources = discoverSourcesInPaths("vic20", { install.string() });
    ASSERT(sources.size() == 1);
    ASSERT(sources[0].basePath == install.string());

    fs::remove_all(install);
}

TEST_CASE(vice_importer_discover_missing_file) {
    // Install missing one ROM — should not appear in results
    fs::path base = fs::temp_directory_path() / "mmemu_test_vice_partial";
    fs::remove_all(base);
    fs::create_directories(base);

    auto specs = romFilesFor("vic20");
    // Write all but the last spec
    for (size_t i = 0; i + 1 < specs.size(); ++i) {
        writeFile(base / specs[i].srcRelPath, specs[i].expectedSize);
    }

    auto sources = discoverSourcesInPaths("vic20", { base.string() });
    ASSERT(sources.empty());

    fs::remove_all(base);
}

TEST_CASE(vice_importer_discover_wrong_size) {
    fs::path base = fs::temp_directory_path() / "mmemu_test_vice_wrongsize";
    fs::remove_all(base);
    fs::create_directories(base);

    for (const auto& spec : romFilesFor("vic20")) {
        // Write wrong size
        writeFile(base / spec.srcRelPath, spec.expectedSize - 1);
    }

    auto sources = discoverSourcesInPaths("vic20", { base.string() });
    ASSERT(sources.empty());

    fs::remove_all(base);
}

TEST_CASE(vice_importer_import_copies_files) {
    fs::path install = makeFakeViceInstall("vic20");
    fs::path dest    = fs::temp_directory_path() / "mmemu_test_dest_copy";
    fs::remove_all(dest);

    RomSource src { "Test", install.string() };
    auto result = importRoms(src, "vic20", dest.string(), false);

    ASSERT(result.success);
    ASSERT(result.copiedFiles.size() == 3);
    ASSERT(result.errorMessage.empty());

    for (const auto& spec : romFilesFor("vic20")) {
        ASSERT(fs::exists(dest / spec.destName));
        ASSERT(fs::file_size(dest / spec.destName) == spec.expectedSize);
    }

    fs::remove_all(install);
    fs::remove_all(dest);
}

TEST_CASE(vice_importer_import_size_mismatch_rollback) {
    // Build a fake install where one file has the wrong size
    fs::path base = fs::temp_directory_path() / "mmemu_test_vice_badsize";
    fs::remove_all(base);

    auto specs = romFilesFor("vic20");
    for (size_t i = 0; i < specs.size(); ++i) {
        // Last file is too short — simulates corrupt source
        size_t sz = (i + 1 == specs.size()) ? specs[i].expectedSize - 1 : specs[i].expectedSize;
        writeFile(base / specs[i].srcRelPath, sz);
    }

    fs::path dest = fs::temp_directory_path() / "mmemu_test_dest_rollback";
    fs::remove_all(dest);

    RomSource src { "Bad", base.string() };
    auto result = importRoms(src, "vic20", dest.string(), false);

    ASSERT(!result.success);
    ASSERT(!result.errorMessage.empty());

    // All partially-copied files must have been removed (rollback)
    for (const auto& spec : specs) {
        ASSERT(!fs::exists(dest / spec.destName));
    }

    fs::remove_all(base);
    fs::remove_all(dest);
}

TEST_CASE(vice_importer_import_no_overwrite) {
    fs::path install = makeFakeViceInstall("vic20");
    fs::path dest    = fs::temp_directory_path() / "mmemu_test_dest_nooverwrite";
    fs::remove_all(dest);
    fs::create_directories(dest);

    // Pre-place one of the destination files
    const RomFileSpec firstSpec = romFilesFor("vic20")[0];
    writeFile(dest / firstSpec.destName, firstSpec.expectedSize, 0x00);

    RomSource src { "Test", install.string() };
    auto result = importRoms(src, "vic20", dest.string(), false);

    ASSERT(!result.success);
    ASSERT(!result.errorMessage.empty());

    // The pre-existing file must still have its original content (fill 0x00, not 0xAA)
    std::ifstream f(dest / firstSpec.destName, std::ios::binary);
    uint8_t byte = 0xFF;
    f.read(reinterpret_cast<char*>(&byte), 1);
    ASSERT(byte == 0x00);

    fs::remove_all(install);
    fs::remove_all(dest);
}

TEST_CASE(vice_importer_import_overwrite_allowed) {
    fs::path install = makeFakeViceInstall("vic20");
    fs::path dest    = fs::temp_directory_path() / "mmemu_test_dest_overwrite";
    fs::remove_all(dest);
    fs::create_directories(dest);

    // Pre-place all destination files with different content
    for (const auto& spec : romFilesFor("vic20")) {
        writeFile(dest / spec.destName, spec.expectedSize, 0x00);
    }

    RomSource src { "Test", install.string() };
    auto result = importRoms(src, "vic20", dest.string(), true);

    ASSERT(result.success);
    // Verify content was replaced (source fill is 0xAA)
    const RomFileSpec firstSpec = romFilesFor("vic20")[0];
    std::ifstream f(dest / firstSpec.destName, std::ios::binary);
    uint8_t byte = 0x00;
    f.read(reinterpret_cast<char*>(&byte), 1);
    ASSERT(byte == 0xAA);

    fs::remove_all(install);
    fs::remove_all(dest);
}
