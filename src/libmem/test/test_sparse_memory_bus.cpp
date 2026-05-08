#include "test_harness.h"
#include "libmem/main/sparse_memory_bus.h"
#include <vector>
#include <cstring>

TEST_CASE(sparsemembus_unallocated_reads) {
    SparseMemoryBus bus("test", 28);

    ASSERT(bus.read8(0x000000) == 0xFF);
    ASSERT(bus.read8(0x100000) == 0xFF);
    ASSERT(bus.peek8(0x200000) == 0xFF);
}

TEST_CASE(sparsemembus_lazy_allocation) {
    SparseMemoryBus bus("test", 28);

    bus.write8(0x001000, 0x42);
    ASSERT(bus.read8(0x001000) == 0x42);

    bus.write8(0x002000, 0x99);
    ASSERT(bus.read8(0x002000) == 0x99);
    ASSERT(bus.read8(0x001000) == 0x42);

    ASSERT(bus.read8(0x003000) == 0xFF);
}

TEST_CASE(sparsemembus_rom_overlay) {
    SparseMemoryBus bus("test", 28);
    uint8_t romData[] = {0x01, 0x02, 0x03, 0x04};

    bus.addRegion(0x080000, 4, romData, false);

    ASSERT(bus.read8(0x080000) == 0x01);
    ASSERT(bus.read8(0x080003) == 0x04);

    bus.write8(0x080000, 0xFF);
    ASSERT(bus.read8(0x080000) == 0x01);
}

TEST_CASE(sparsemembus_multiple_regions) {
    SparseMemoryBus bus("test", 28);
    uint8_t rom1[] = {0xAA, 0xBB};
    uint8_t rom2[] = {0xCC, 0xDD};

    bus.addRegion(0x020000, 2, rom1, false);
    bus.addRegion(0x040000, 2, rom2, false);

    ASSERT(bus.read8(0x020000) == 0xAA);
    ASSERT(bus.read8(0x020001) == 0xBB);
    ASSERT(bus.read8(0x040000) == 0xCC);
    ASSERT(bus.read8(0x040001) == 0xDD);
}

TEST_CASE(sparsemembus_snapshot) {
    SparseMemoryBus bus("test", 28);

    bus.write8(0x001234, 0x55);
    bus.write8(0x005678, 0xAA);

    size_t size = bus.stateSize();
    std::vector<uint8_t> buffer(size);
    bus.saveState(buffer.data());

    bus.write8(0x001234, 0xFF);
    bus.write8(0x005678, 0xFF);
    ASSERT(bus.read8(0x001234) == 0xFF);
    ASSERT(bus.read8(0x005678) == 0xFF);

    bus.loadState(buffer.data());
    ASSERT(bus.read8(0x001234) == 0x55);
    ASSERT(bus.read8(0x005678) == 0xAA);
    ASSERT(bus.read8(0x009999) == 0xFF);
}

TEST_CASE(sparsemembus_write_log) {
    SparseMemoryBus bus("test", 28);
    bus.clearWriteLog();

    bus.write8(0x001000, 0x11);
    bus.write8(0x001000, 0x22);

    ASSERT(bus.writeCount() == 2);

    uint32_t addrs[2];
    uint8_t before[2];
    uint8_t after[2];
    bus.getWrites(addrs, before, after, 2);

    ASSERT(addrs[0] == 0x001000);
    ASSERT(before[0] == 0xFF);
    ASSERT(after[0] == 0x11);

    ASSERT(addrs[1] == 0x001000);
    ASSERT(before[1] == 0x11);
    ASSERT(after[1] == 0x22);
}

TEST_CASE(sparsemembus_reset) {
    SparseMemoryBus bus("test", 28);
    uint8_t rom[] = {0xAA};

    bus.addRegion(0x080000, 1, rom, false);
    bus.write8(0x001000, 0x42);

    bus.reset();

    ASSERT(bus.read8(0x001000) == 0xFF);
    ASSERT(bus.read8(0x080000) == 0xAA);
    ASSERT(bus.writeCount() == 0);
}
