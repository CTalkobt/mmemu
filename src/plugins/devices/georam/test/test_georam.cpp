#include "test_harness.h"
#include "plugins/devices/georam/main/georam.h"
#include <cstdio>
#include <string>

// GeoRAM against VICE's georam.c: the page window at $DE00, the write-only
// page/block registers (mirrored from $DF80), the block taken modulo the size
TEST_CASE(georam_paging) {
    GeoRam g;                                       // 512K: 32 blocks
    auto w = [&](uint32_t a, uint8_t v) { return g.ioWrite(nullptr, a, v); };
    auto r = [&](uint32_t a) { uint8_t v = 0; g.ioRead(nullptr, a, &v); return v; };
    EXPECT_TRUE(w(0xDFFE, 5));
    EXPECT_TRUE(w(0xDFFF, 3));
    w(0xDE10, 0xAB);
    EXPECT_EQ(g.ram()[3 * 16384 + 5 * 256 + 0x10], 0xAB);
    EXPECT_EQ(r(0xDE10), 0xAB);
    // The registers read nothing back (not driven); the debugger peeks them
    uint8_t v = 0;
    EXPECT_TRUE(!g.ioRead(nullptr, 0xDFFE, &v));
    EXPECT_TRUE(g.ioPeek(nullptr, 0xDFFF, &v));
    EXPECT_EQ(v, 3);
    // Mirrors: odd addresses from $DF80 are the block, even the page
    w(0xDF81, 7); w(0xDF90, 2);
    EXPECT_EQ(g.block(), 7);
    EXPECT_EQ(g.page(), 2);
    EXPECT_TRUE(!w(0xDF7F, 1));                     // below the mirrors: not its
    EXPECT_EQ(g.block(), 7);
    // Modulo: page 64 is 0, block 32 of 32 is 0, block 33 is 1
    w(0xDFFE, 64 + 9);
    EXPECT_EQ(g.page(), 9);
    w(0xDFFF, 33);
    EXPECT_EQ(g.block(), 1);
    // Every byte of the RAM is reachable
    std::string err;
    EXPECT_TRUE(g.setOption("size", "4M", err));
    w(0xDFFF, 255); w(0xDFFE, 63); w(0xDEFF, 0x5A);
    EXPECT_EQ(g.ram()[(4u << 20) - 1], 0x5A);
    // A reset: the registers, not the RAM
    g.reset();
    EXPECT_EQ(g.page(), 0);
    EXPECT_EQ(g.block(), 0);
    EXPECT_EQ(g.ram()[(4u << 20) - 1], 0x5A);
    EXPECT_TRUE(!g.ioWrite(nullptr, 0xFF00, 1));
}

TEST_CASE(georam_options_and_images) {
    GeoRam g;
    std::string err;
    EXPECT_TRUE(!g.setOption("size", "256K", err));  // smaller units never existed
    EXPECT_TRUE(!g.setOption("size", "8M", err));
    EXPECT_TRUE(g.setOption("size", "2m", err));
    EXPECT_EQ(g.size(), 2u << 20);
    g.ram()[12345] = 0x42;
    const std::string path = "/tmp/mmemu_test_georam.bin";
    EXPECT_TRUE(g.saveImage(path, err));
    GeoRam h;
    EXPECT_TRUE(h.setOption("image", path, err));
    EXPECT_EQ(h.size(), 2u << 20);
    EXPECT_EQ(h.ram()[12345], 0x42);
    std::remove(path.c_str());
}
