#include "test_harness.h"
#include "plugins/devices/rampac/main/rampac.h"
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace {
std::string tempFile(const std::string& tag) {
    return (std::filesystem::temp_directory_path() / ("mmsim_rampac_" + tag + "_" + std::to_string(::getpid()) + ".bin"))
        .string();
}
}  // namespace

// Block select, the counter stepping and wrapping within the block (the card's 10-bit counter)
TEST_CASE(rampac_ports) {
    Rampac r;
    r.out(0x81, 5);
    ASSERT_EQ(r.sector(), 5);
    for (int i = 0; i < 1026; ++i) r.out(0x83, (uint8_t)(i * 7));
    ASSERT_EQ(r.sector(), 5);                       // still block 5: byte 1024 was byte 0 again
    ASSERT_EQ(r.counter(), 2);
    ASSERT_EQ(r.ram()[5 * 1024 + 3], (uint8_t)21);
    ASSERT_EQ(r.ram()[5 * 1024 + 1], (uint8_t)(1025 * 7));
    ASSERT_EQ(r.ram()[5 * 1024 + 1023], (uint8_t)(1023 * 7));
    ASSERT_EQ(r.ram()[6 * 1024], 0);
    ASSERT_EQ(r.in(0x81), 0xFF);                    // a select on a read: the card drives nothing
    ASSERT_EQ(r.counter(), 0);
    ASSERT_EQ(r.peek(0x83), (uint8_t)(1024 * 7));
    ASSERT_EQ(r.in(0x83), (uint8_t)(1024 * 7));
    ASSERT_EQ(r.in(0x83), (uint8_t)(1025 * 7));
    ASSERT_EQ(r.in(0x80), 0xFF);
    // Mirrors: any odd port in $80-$8F; bits 1-0 = 01 select, 11 the byte
    r.out(0x8D, 9);
    ASSERT_EQ(r.sector(), 9);
    ASSERT_EQ(r.bank(), 0);                         // 256 KB: one bank
    r.out(0x87, 0x42);
    ASSERT_EQ(r.ram()[9 * 1024], 0x42);
    ASSERT(Rampac::decodes(0x83) && Rampac::decodes(0x8F) && !Rampac::decodes(0x82) && !Rampac::decodes(0x93));
}

// Banks: $81 + 4n selects bank n on a card that has it, else the mirror; RAMDSK's
// bank count (write n to each bank, read bank 0) comes out 1, 2 and 4
TEST_CASE(rampac_banks) {
    for (int kb : {256, 512, 1024}) {
        Rampac r(kb);
        ASSERT_EQ(r.sizeKb(), kb);
        ASSERT_EQ(r.ram().size(), (size_t)(kb == 256 ? 1 : kb / 256) * Rampac::BANK_SIZE);
        for (int n = 0; n < 4; ++n) {
            r.out((uint8_t)(0x81 + 4 * n), 0);
            r.out(0x83, (uint8_t)n);
        }
        r.out(0x81, 0);
        const int banks = 4 - r.in(0x83);
        ASSERT_EQ(banks, r.banks());
        ASSERT_EQ(banks, kb == 256 ? 1 : kb / 256);
    }
    Rampac r(512);
    r.out(0x85, 3);
    r.out(0x83, 0x77);
    ASSERT_EQ(r.ram()[Rampac::BANK_SIZE + 3 * 1024], 0x77);
    r.out(0x89, 3);                                 // bank 2 on a 512 KB card is bank 0
    ASSERT_EQ(r.bank(), 0);
    ASSERT_EQ(r.in(0x83), 0);
}

// A 128 KB Datapac: blocks 128-255 aren't there (RAMDSK's TestRam sees the write fail)
TEST_CASE(rampac_128k) {
    Rampac r(128);
    r.out(0x81, 0x80);
    const uint8_t orig = r.in(0x83);
    r.out(0x81, 0x80);
    r.out(0x83, (uint8_t)(orig + 1));
    r.out(0x81, 0x80);
    ASSERT_EQ(r.in(0x83), orig);
    ASSERT_EQ(orig, 0xFF);
    r.out(0x81, 0x7F);
    r.out(0x83, 0x12);
    r.out(0x81, 0x7F);
    ASSERT_EQ(r.in(0x83), 0x12);
}

// The battery: the file is created, written by flush(), and read back on open
TEST_CASE(rampac_persist) {
    const std::string path = tempFile("persist");
    std::filesystem::remove(path);
    {
        Rampac r;
        ASSERT(r.open(path));
        ASSERT_EQ(std::filesystem::file_size(path), Rampac::SIZE);
        r.out(0x81, 200);
        r.out(0x83, 0x5A);
        ASSERT(r.dirty());
        ASSERT(r.flush());
        ASSERT(!r.dirty());
        r.out(0x83, 0xA5);                  // written by the destructor
    }
    Rampac r2;
    ASSERT(r2.open(path));
    ASSERT_EQ(r2.ram()[200 * 1024], 0x5A);
    ASSERT_EQ(r2.ram()[200 * 1024 + 1], 0xA5);
    std::filesystem::remove(path);
}
