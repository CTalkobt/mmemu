#include "test_harness.h"
#include "plugins/cbm-loader/main/d64_parser.h"
#include "plugins/cbm-loader/main/d71_parser.h"
#include "plugins/cbm-loader/main/d80_parser.h"
#include "plugins/cbm-loader/main/d81_parser.h"
#include "plugins/cbm-loader/main/d82_parser.h"
#include "plugins/cbm-loader/main/disk_loader.h"
#include "libmem/main/memory_bus.h"
#include <fstream>
#include <filesystem>
#include <cstring>

namespace fs = std::filesystem;

// Helper: compute sector offset from geometry (mirrors CbmSectorDisk::getSectorOffset)
static uint32_t sectorOffset(const int* spt, int track, int sector) {
    uint32_t off = 0;
    for (int i = 1; i < track; ++i)
        off += spt[i - 1] * 256;
    off += sector * 256;
    return off;
}

// Helper: write a standard directory entry + file data into a disk image buffer.
// Writes one PRG file named "HELLO" at the given directory and file locations.
static void writeDirAndFile(std::vector<uint8_t>& img, const int* spt,
                            int dirTrack, int dirSector,
                            int fileTrack, int fileSector) {
    uint32_t dirOff = sectorOffset(spt, dirTrack, dirSector);
    // End of directory chain
    img[dirOff] = 0;
    img[dirOff + 1] = 0xFF;
    // Entry 0: PRG file
    img[dirOff + 2] = 0x82;             // type = PRG
    img[dirOff + 3] = fileTrack;
    img[dirOff + 4] = fileSector;
    const char* name = "HELLO";
    std::memcpy(&img[dirOff + 5], name, 5);
    for (int i = 10; i < 21; ++i) img[dirOff + i] = 0xA0;
    img[dirOff + 30] = 1; // 1 block
    img[dirOff + 31] = 0;

    // File data sector
    uint32_t fileOff = sectorOffset(spt, fileTrack, fileSector);
    img[fileOff] = 0;     // end of chain
    img[fileOff + 1] = 7; // 6 data bytes (indices 2..7)
    img[fileOff + 2] = 0x01; // load addr lo
    img[fileOff + 3] = 0x08; // load addr hi ($0801)
    img[fileOff + 4] = 0xA9; // LDA #$42
    img[fileOff + 5] = 0x42;
    img[fileOff + 6] = 0xEA; // NOP
    img[fileOff + 7] = 0x60; // RTS
}

// Helper: write disk name and ID into the header sector
static void writeHeader(std::vector<uint8_t>& img, const int* spt,
                        int headerTrack, int headerSector,
                        int nameOffset, int idOffset) {
    uint32_t off = sectorOffset(spt, headerTrack, headerSector);
    const char* dname = "TEST DISK";
    std::memcpy(&img[off + nameOffset], dname, 9);
    for (int i = 9; i < 16; ++i) img[off + nameOffset + i] = 0xA0;
    img[off + idOffset] = 'T';
    img[off + idOffset + 1] = 'D';
}

// ---------------------------------------------------------------------------
// D64 tests (existing format, verifying refactored base class)
// ---------------------------------------------------------------------------

static const int s_d64Spt[] = {
    21,21,21,21,21,21,21,21,21,21,21,21,21,21,21,21,21,
    19,19,19,19,19,19,19,
    18,18,18,18,18,18,
    17,17,17,17,17
};

TEST_CASE(d64_refactored_open_dir_read) {
    const char* path = "test_d64_ref.d64";
    {
        std::vector<uint8_t> img(174848, 0);
        // BAM sector (18/0): point to dir at 18/1
        uint32_t bamOff = sectorOffset(s_d64Spt, 18, 0);
        img[bamOff] = 18;
        img[bamOff + 1] = 1;
        writeDirAndFile(img, s_d64Spt, 18, 1, 1, 0);
        writeHeader(img, s_d64Spt, 18, 0, 0x90, 0xA2);
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<char*>(img.data()), img.size());
    }

    D64Parser parser;
    ASSERT(parser.open(path));
    ASSERT(parser.getDiskName() == "TEST DISK");
    ASSERT(parser.getDiskId() == "TD");

    auto dir = parser.getDirectory();
    ASSERT(dir.size() == 1);
    ASSERT(dir[0].filename == "HELLO");
    ASSERT(dir[0].type == 0x82);

    std::vector<uint8_t> data;
    ASSERT(parser.readFile("HELLO", data));
    ASSERT(data.size() == 6);
    ASSERT(data[0] == 0x01); // load addr lo
    ASSERT(data[1] == 0x08); // load addr hi
    ASSERT(data[2] == 0xA9); // LDA
    ASSERT(data[3] == 0x42); // #$42
    ASSERT(data[5] == 0x60); // RTS

    fs::remove(path);
}

// ---------------------------------------------------------------------------
// D71 tests (1571 double-sided)
// ---------------------------------------------------------------------------

static const int s_d71Spt[] = {
    21,21,21,21,21,21,21,21,21,21,21,21,21,21,21,21,21,
    19,19,19,19,19,19,19, 18,18,18,18,18,18, 17,17,17,17,17,
    21,21,21,21,21,21,21,21,21,21,21,21,21,21,21,21,21,
    19,19,19,19,19,19,19, 18,18,18,18,18,18, 17,17,17,17,17
};

TEST_CASE(d71_open_dir_read) {
    const char* path = "test_fmt.d71";
    {
        std::vector<uint8_t> img(349696, 0);
        uint32_t bamOff = sectorOffset(s_d71Spt, 18, 0);
        img[bamOff] = 18;
        img[bamOff + 1] = 1;
        writeDirAndFile(img, s_d71Spt, 18, 1, 1, 0);
        writeHeader(img, s_d71Spt, 18, 0, 0x90, 0xA2);
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<char*>(img.data()), img.size());
    }

    D71Parser parser;
    ASSERT(parser.open(path));
    ASSERT(parser.getDiskName() == "TEST DISK");
    ASSERT(parser.getDiskId() == "TD");

    auto dir = parser.getDirectory();
    ASSERT(dir.size() == 1);
    ASSERT(dir[0].filename == "HELLO");

    std::vector<uint8_t> data;
    ASSERT(parser.readFile("HELLO", data));
    ASSERT(data.size() == 6);
    ASSERT(data[2] == 0xA9);

    fs::remove(path);
}

// ---------------------------------------------------------------------------
// D81 tests (1581)
// ---------------------------------------------------------------------------

static const int s_d81Spt[] = {
    40,40,40,40,40,40,40,40,40,40,40,40,40,40,40,40,40,40,40,40,
    40,40,40,40,40,40,40,40,40,40,40,40,40,40,40,40,40,40,40,40,
    40,40,40,40,40,40,40,40,40,40,40,40,40,40,40,40,40,40,40,40,
    40,40,40,40,40,40,40,40,40,40,40,40,40,40,40,40,40,40,40,40
};

TEST_CASE(d81_open_dir_read) {
    const char* path = "test_fmt.d81";
    {
        std::vector<uint8_t> img(819200, 0);
        // Header sector (40/0): point to BAM at 40/1
        uint32_t hdrOff = sectorOffset(s_d81Spt, 40, 0);
        img[hdrOff] = 40;
        img[hdrOff + 1] = 3; // link to first dir sector
        writeDirAndFile(img, s_d81Spt, 40, 3, 1, 0);
        writeHeader(img, s_d81Spt, 40, 0, 0x04, 0x16);
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<char*>(img.data()), img.size());
    }

    D81Parser parser;
    ASSERT(parser.open(path));
    ASSERT(parser.getDiskName() == "TEST DISK");
    ASSERT(parser.getDiskId() == "TD");

    auto dir = parser.getDirectory();
    ASSERT(dir.size() == 1);
    ASSERT(dir[0].filename == "HELLO");

    std::vector<uint8_t> data;
    ASSERT(parser.readFile("HELLO", data));
    ASSERT(data.size() == 6);
    ASSERT(data[0] == 0x01);
    ASSERT(data[1] == 0x08);

    fs::remove(path);
}

// ---------------------------------------------------------------------------
// D80 tests (8050)
// ---------------------------------------------------------------------------

static const int s_d80Spt[] = {
    29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,
    29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,
    27,27,27,27,27,27,27,27,27,27,27,27,27,27,
    25,25,25,25,25,25,25,25,25,25,25,
    23,23,23,23,23,23,23,23,23,23,23,23,23
};

TEST_CASE(d80_open_dir_read) {
    const char* path = "test_fmt.d80";
    {
        std::vector<uint8_t> img(533248, 0);
        // Header at 38/0
        uint32_t hdrOff = sectorOffset(s_d80Spt, 38, 0);
        img[hdrOff] = 38;
        img[hdrOff + 1] = 3;
        writeDirAndFile(img, s_d80Spt, 39, 1, 1, 0);
        writeHeader(img, s_d80Spt, 38, 0, 0x06, 0x18);
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<char*>(img.data()), img.size());
    }

    D80Parser parser;
    ASSERT(parser.open(path));
    ASSERT(parser.getDiskName() == "TEST DISK");
    ASSERT(parser.getDiskId() == "TD");

    auto dir = parser.getDirectory();
    ASSERT(dir.size() == 1);
    ASSERT(dir[0].filename == "HELLO");

    std::vector<uint8_t> data;
    ASSERT(parser.readFile("HELLO", data));
    ASSERT(data.size() == 6);

    fs::remove(path);
}

// ---------------------------------------------------------------------------
// D82 tests (8250 double-sided)
// ---------------------------------------------------------------------------

static const int s_d82Spt[] = {
    29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,
    29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,
    27,27,27,27,27,27,27,27,27,27,27,27,27,27,
    25,25,25,25,25,25,25,25,25,25,25,
    23,23,23,23,23,23,23,23,23,23,23,23,23,
    29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,
    29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,
    27,27,27,27,27,27,27,27,27,27,27,27,27,27,
    25,25,25,25,25,25,25,25,25,25,25,
    23,23,23,23,23,23,23,23,23,23,23,23,23
};

TEST_CASE(d82_open_dir_read) {
    const char* path = "test_fmt.d82";
    {
        std::vector<uint8_t> img(1066496, 0);
        uint32_t hdrOff = sectorOffset(s_d82Spt, 38, 0);
        img[hdrOff] = 38;
        img[hdrOff + 1] = 3;
        writeDirAndFile(img, s_d82Spt, 39, 1, 1, 0);
        writeHeader(img, s_d82Spt, 38, 0, 0x06, 0x18);
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<char*>(img.data()), img.size());
    }

    D82Parser parser;
    ASSERT(parser.open(path));
    ASSERT(parser.getDiskName() == "TEST DISK");

    auto dir = parser.getDirectory();
    ASSERT(dir.size() == 1);
    ASSERT(dir[0].filename == "HELLO");

    std::vector<uint8_t> data;
    ASSERT(parser.readFile("HELLO", data));
    ASSERT(data.size() == 6);

    fs::remove(path);
}

// ---------------------------------------------------------------------------
// Multi-sector file chain test
// ---------------------------------------------------------------------------

TEST_CASE(d64_multi_sector_chain) {
    const char* path = "test_d64_chain.d64";
    {
        std::vector<uint8_t> img(174848, 0);

        // BAM sector (18/0): point to dir at 18/1
        uint32_t bamOff = sectorOffset(s_d64Spt, 18, 0);
        img[bamOff] = 18;
        img[bamOff + 1] = 1;

        // Directory at 18/1: one file starting at track 1, sector 0
        uint32_t dirOff = sectorOffset(s_d64Spt, 18, 1);
        img[dirOff] = 0;      // end of dir chain
        img[dirOff + 1] = 0xFF;
        img[dirOff + 2] = 0x82; // PRG
        img[dirOff + 3] = 1;    // start track
        img[dirOff + 4] = 0;    // start sector
        const char* name = "BIGFILE";
        std::memcpy(&img[dirOff + 5], name, 7);
        for (int i = 12; i < 21; ++i) img[dirOff + i] = 0xA0;
        img[dirOff + 30] = 3; // 3 blocks

        // Sector 1: track 1, sector 0 -> next: track 1, sector 1
        uint32_t s0 = sectorOffset(s_d64Spt, 1, 0);
        img[s0] = 1;       // next track
        img[s0 + 1] = 1;   // next sector
        // 254 data bytes (load addr + data)
        img[s0 + 2] = 0x01; // load addr lo ($0801)
        img[s0 + 3] = 0x08; // load addr hi
        for (int i = 4; i < 256; ++i) img[s0 + i] = 0xAA;

        // Sector 2: track 1, sector 1 -> next: track 1, sector 2
        uint32_t s1 = sectorOffset(s_d64Spt, 1, 1);
        img[s1] = 1;       // next track
        img[s1 + 1] = 2;   // next sector
        for (int i = 2; i < 256; ++i) img[s1 + i] = 0xBB;

        // Sector 3: track 1, sector 2 -> end (last sector)
        uint32_t s2 = sectorOffset(s_d64Spt, 1, 2);
        img[s2] = 0;       // end of chain
        img[s2 + 1] = 50;  // 49 data bytes (indices 2..50)
        for (int i = 2; i <= 50; ++i) img[s2 + i] = 0xCC;

        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<char*>(img.data()), img.size());
    }

    D64Parser parser;
    ASSERT(parser.open(path));

    auto dir = parser.getDirectory();
    ASSERT(dir.size() == 1);
    ASSERT(dir[0].filename == "BIGFILE");

    std::vector<uint8_t> data;
    ASSERT(parser.readFile("BIGFILE", data));

    // Expected size: 254 (sector 0) + 254 (sector 1) + 49 (sector 2) = 557 bytes
    ASSERT(data.size() == 557);

    // Check load address
    ASSERT(data[0] == 0x01);
    ASSERT(data[1] == 0x08);

    // Check sector boundaries
    ASSERT(data[2] == 0xAA);     // first sector data
    ASSERT(data[253] == 0xAA);   // last byte of first sector
    ASSERT(data[254] == 0xBB);   // first byte of second sector
    ASSERT(data[507] == 0xBB);   // last byte of second sector
    ASSERT(data[508] == 0xCC);   // first byte of third sector
    ASSERT(data[556] == 0xCC);   // last byte of third sector

    fs::remove(path);
}

// ---------------------------------------------------------------------------
// DiskImageLoader extension dispatch
// ---------------------------------------------------------------------------

TEST_CASE(disk_loader_recognizes_new_formats) {
    DiskImageLoader loader;
    ASSERT(loader.canLoad("test.d64"));
    ASSERT(loader.canLoad("test.d71"));
    ASSERT(loader.canLoad("test.D71")); // case insensitive
    ASSERT(loader.canLoad("test.d80"));
    ASSERT(loader.canLoad("test.d81"));
    ASSERT(loader.canLoad("test.d82"));
    ASSERT(loader.canLoad("test.t64"));
    ASSERT(!loader.canLoad("test.xyz"));
}

// ---------------------------------------------------------------------------
// Reject wrong sizes
// ---------------------------------------------------------------------------

TEST_CASE(d81_rejects_wrong_size) {
    const char* path = "test_bad.d81";
    {
        std::vector<uint8_t> img(100000, 0);
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<char*>(img.data()), img.size());
    }
    D81Parser parser;
    ASSERT(!parser.open(path));
    fs::remove(path);
}

TEST_CASE(d71_rejects_wrong_size) {
    const char* path = "test_bad.d71";
    {
        std::vector<uint8_t> img(200000, 0);
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<char*>(img.data()), img.size());
    }
    D71Parser parser;
    ASSERT(!parser.open(path));
    fs::remove(path);
}

// ---------------------------------------------------------------------------
// Geometry sanity checks
// ---------------------------------------------------------------------------

TEST_CASE(geometry_total_sectors) {
    D64Parser d64;
    ASSERT(d64.geometry().totalSectors() == 683);

    D71Parser d71;
    ASSERT(d71.geometry().totalSectors() == 1366);

    D81Parser d81;
    ASSERT(d81.geometry().totalSectors() == 3200);

    D80Parser d80;
    ASSERT(d80.geometry().totalSectors() == 2083);

    D82Parser d82;
    ASSERT(d82.geometry().totalSectors() == 4166);
}

TEST_CASE(geometry_base_sizes) {
    D64Parser d64;
    ASSERT(d64.geometry().baseSize() == 174848);

    D71Parser d71;
    ASSERT(d71.geometry().baseSize() == 349696);

    D81Parser d81;
    ASSERT(d81.geometry().baseSize() == 819200);

    D80Parser d80;
    ASSERT(d80.geometry().baseSize() == 533248);

    D82Parser d82;
    ASSERT(d82.geometry().baseSize() == 1066496);
}
