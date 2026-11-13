#include "test_harness.h"
#include "libmem/main/memory_bus.h"
#include <vector>
#include <cstring>

TEST_CASE(flatmembus_basic_rw) {
    FlatMemoryBus bus("test", 16); // 64 KB
    
    bus.write8(0x1000, 0x42);
    ASSERT(bus.read8(0x1000) == 0x42);
    
    bus.write16(0x2000, 0x1234);
    ASSERT(bus.read16(0x2000) == 0x1234);
    ASSERT(bus.read8(0x2000) == 0x34); // Little endian
    ASSERT(bus.read8(0x2001) == 0x12);

    bus.write32(0x3000, 0xDEADBEEF);
    ASSERT(bus.read32(0x3000) == 0xDEADBEEF);
}

TEST_CASE(flatmembus_masking) {
    FlatMemoryBus bus("test", 8); // 256 bytes
    bus.write8(0x00, 0xAA);
    ASSERT(bus.read8(0x100) == 0xAA); // Should wrap
}

TEST_CASE(flatmembus_rom_overlay) {
    FlatMemoryBus bus("test", 16);
    uint8_t romData[] = {0x01, 0x02, 0x03, 0x04};
    
    bus.addOverlay(0x8000, 4, romData, false);
    
    ASSERT(bus.read8(0x8000) == 0x01);
    ASSERT(bus.read8(0x8003) == 0x04);
    
    // Write should be ignored
    bus.write8(0x8000, 0xFF);
    ASSERT(bus.read8(0x8000) == 0x01);
    
    // Underlying RAM should be untouched if we peek it (well, peek8 also uses overlay)
    // There is no easy way to peek underlying RAM if overlay is active in current implementation.
}

TEST_CASE(flatmembus_write_log) {
    FlatMemoryBus bus("test", 16);
    bus.clearWriteLog();
    
    bus.write8(0x1000, 0x11);
    bus.write8(0x1000, 0x22);
    
    ASSERT(bus.writeCount() == 2);
    
    uint32_t addrs[2];
    uint8_t before[2];
    uint8_t after[2];
    bus.getWrites(addrs, before, after, 2);
    
    ASSERT(addrs[0] == 0x1000);
    ASSERT(before[0] == 0x00);
    ASSERT(after[0] == 0x11);
    
    ASSERT(addrs[1] == 0x1000);
    ASSERT(before[1] == 0x11);
    ASSERT(after[1] == 0x22);
}

TEST_CASE(flatmembus_snapshot) {
    FlatMemoryBus bus("test", 16);
    bus.write8(0x1234, 0x55);
    
    size_t size = bus.stateSize();
    std::vector<uint8_t> buffer(size);
    bus.saveState(buffer.data());
    
    bus.write8(0x1234, 0xAA);
    ASSERT(bus.read8(0x1234) == 0xAA);
    
    bus.loadState(buffer.data());
    ASSERT(bus.read8(0x1234) == 0x55);
}

// No main here

#include "libmem/main/ram_init_pattern.h"

// VICE's C64 power-up RAM (ram_resources_init): $00 x2, then $FF x4 / $00 x4
// stripes, the whole pattern inverted every 16K (testprogs C64/raminitpattern)
TEST_CASE(ram_init_pattern_c64) {
    std::vector<uint8_t> m(0x10000);
    ramInitPattern(m.data(), m.size(), ramInitC64());
    ASSERT_EQ(m[0x0000], 0x00);
    ASSERT_EQ(m[0x0001], 0x00);
    ASSERT_EQ(m[0x0002], 0xFF);
    ASSERT_EQ(m[0x0005], 0xFF);
    ASSERT_EQ(m[0x0006], 0x00);
    ASSERT_EQ(m[0x4000], 0xFF);                    // inverted from 16K
    ASSERT_EQ(m[0x4002], 0x00);
    ASSERT_EQ(m[0x8000], 0x00);
}
