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

// Sector select, the counter stepping through a sector into the next, the read back
TEST_CASE(rampac_ports) {
    Rampac r;
    r.out(0x81, 5);
    ASSERT_EQ(r.in(0x81), 5);
    for (int i = 0; i < 1026; ++i) r.out(0x83, (uint8_t)(i * 7));
    ASSERT_EQ(r.sector(), 6);
    ASSERT_EQ(r.counter(), 2);
    ASSERT_EQ(r.ram()[5 * 1024 + 3], (uint8_t)21);
    ASSERT_EQ(r.ram()[6 * 1024 + 1], (uint8_t)(1025 * 7));
    r.out(0x85, 5);                         // $85 also sets the sector (VirtualT)
    ASSERT_EQ(r.counter(), 0);
    ASSERT_EQ(r.peek(0x83), 0);
    ASSERT_EQ(r.in(0x83), 0);
    ASSERT_EQ(r.in(0x83), 7);
    ASSERT_EQ(r.in(0x80), 0xFF);
    // The last sector wraps to 0
    r.out(0x81, 255);
    for (int i = 0; i < 1024; ++i) r.in(0x83);
    ASSERT_EQ(r.sector(), 0);
    ASSERT(Rampac::decodes(0x83) && !Rampac::decodes(0x82));
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
