#include "test_harness.h"
#include "plugins/mega65Importer/main/rom_discovery.h"
#include "plugins/mega65Importer/main/rom_importer.h"
#include <filesystem>
#include <fstream>
#include <vector>
#include <string>
#include <cstdint>
#include <iterator>

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
    ASSERT(specs[0].expectedSize == 131072);
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

// ---------------------------------------------------------------------------
// HYPPO (HICKUP.M65) import
// ---------------------------------------------------------------------------

// A 16KB image whose trap table ($00-$3F) is "JMP $82xx ; NOP" entries.
static std::vector<uint8_t> makeFakeHickup() {
    std::vector<uint8_t> img(HICKUP_SIZE, 0x00);
    for (size_t i = 0; i < 64; ++i) {
        img[i * 4]     = 0x4C;
        img[i * 4 + 1] = static_cast<uint8_t>(i);
        img[i * 4 + 2] = 0x82;
        img[i * 4 + 3] = 0xEA;
    }
    img[HICKUP_SIZE - 1] = 0x5A;
    return img;
}

// Simulates an executable with the image embedded after unrelated bytes,
// including a decoy trap table that is too short.
static fs::path makeFakeXemuBinary(const fs::path& path, size_t prefix) {
    std::vector<uint8_t> blob(prefix, 0x11);
    const uint8_t decoy[] = { 0x4C, 0x00, 0x82, 0xEA, 0x4C, 0x00, 0x82, 0xEA };
    blob.insert(blob.begin() + 16, decoy, decoy + sizeof(decoy));
    auto img = makeFakeHickup();
    blob.insert(blob.end(), img.begin(), img.end());
    blob.insert(blob.end(), 1000, 0x22);
    fs::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(blob.data()), static_cast<std::streamsize>(blob.size()));
    return path;
}

static std::vector<uint8_t> readAll(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

TEST_CASE(mega65_importer_find_hickup_image) {
    std::vector<uint8_t> blob(777, 0x11);
    auto img = makeFakeHickup();
    blob.insert(blob.end(), img.begin(), img.end());
    ASSERT(findHickupImage(blob) == 777);

    // Truncated image is rejected
    blob.resize(777 + HICKUP_SIZE - 1);
    ASSERT(findHickupImage(blob) == -1);

    // Trap table entry pointing outside $8000-$BFFF is rejected
    auto bad = makeFakeHickup();
    bad[63 * 4 + 2] = 0x40;
    ASSERT(findHickupImage(bad) == -1);
}

TEST_CASE(mega65_importer_hickup_extracted_from_xemu_binary) {
    fs::path base = fs::temp_directory_path() / "mmemu_test_hickup_extract";
    fs::remove_all(base);
    fs::path xemu = makeFakeXemuBinary(base / "bin" / "xemu-xmega65", 4096);

    auto result = importHickup("", xemu.string(), (base / "dest").string(), false);
    ASSERT(result.success);
    ASSERT(result.copiedFiles.size() == 1);
    ASSERT(readAll(base / "dest" / "HICKUP.M65") == makeFakeHickup());

    fs::remove_all(base);
}

TEST_CASE(mega65_importer_hickup_prefers_source_file) {
    fs::path base = fs::temp_directory_path() / "mmemu_test_hickup_source";
    fs::remove_all(base);
    writeFile(base / "src" / "HICKUP.M65", HICKUP_SIZE, 0x77);
    fs::path xemu = makeFakeXemuBinary(base / "bin" / "xemu-xmega65", 100);

    auto result = importHickup((base / "src").string(), xemu.string(), (base / "dest").string(), false);
    ASSERT(result.success);
    ASSERT(readAll(base / "dest" / "HICKUP.M65") == std::vector<uint8_t>(HICKUP_SIZE, 0x77));

    fs::remove_all(base);
}

TEST_CASE(mega65_importer_hickup_no_overwrite_and_missing_image) {
    fs::path base = fs::temp_directory_path() / "mmemu_test_hickup_misc";
    fs::remove_all(base);
    writeFile(base / "dest" / "HICKUP.M65", HICKUP_SIZE, 0x00);
    fs::path xemu = makeFakeXemuBinary(base / "bin" / "xemu-xmega65", 100);

    auto kept = importHickup("", xemu.string(), (base / "dest").string(), false);
    ASSERT(kept.success);
    ASSERT(kept.copiedFiles.empty());
    ASSERT(readAll(base / "dest" / "HICKUP.M65") == std::vector<uint8_t>(HICKUP_SIZE, 0x00));

    writeFile(base / "bin" / "not-xemu", 50000, 0x11);
    auto missing = importHickup("", (base / "bin" / "not-xemu").string(), (base / "dest2").string(), false);
    ASSERT(!missing.success);
    ASSERT(!fs::exists(base / "dest2" / "HICKUP.M65"));

    fs::remove_all(base);
}
