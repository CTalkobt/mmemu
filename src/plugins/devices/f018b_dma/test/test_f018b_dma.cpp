#include "test_harness.h"
#include "plugins/devices/f018b_dma/main/f018b_dma.h"
#include "libmem/main/memory_bus.h"
#include "libmem/main/sparse_memory_bus.h"
#include <cstring>

struct F018bFixture {
    FlatMemoryBus flatBus{"flat", 16};
    SparseMemoryBus sparseBus{"sparse", 28};  // 28-bit address space for MEGA65
    F018bDmaDevice dma{0xD700};

    F018bFixture() {
        dma.setDmaBus(&sparseBus);
        // Pre-allocate some pages to avoid issues
        for (uint32_t i = 0; i < 0x10000; i += 0x1000) {
            sparseBus.read8(i);  // Force page allocation
        }
    }
};

// ============================================================================
// Basic Register I/O Tests
// ============================================================================

TEST_CASE(f018b_registers_read_write) {
    F018bFixture f;
    uint8_t val;

    // Write to ADDRLSBTRIG
    ASSERT(f.dma.ioWrite(&f.flatBus, 0xD700, 0x34));
    ASSERT(f.dma.ioRead(&f.flatBus, 0xD700, &val));
    ASSERT_EQ((int)val, 0x34);

    // Write to ADDRMSB
    ASSERT(f.dma.ioWrite(&f.flatBus, 0xD701, 0x12));
    ASSERT(f.dma.ioRead(&f.flatBus, 0xD701, &val));
    ASSERT_EQ((int)val, 0x12);

    // Write to ADDRBANK
    ASSERT(f.dma.ioWrite(&f.flatBus, 0xD702, 0x05));
    ASSERT(f.dma.ioRead(&f.flatBus, 0xD702, &val));
    ASSERT_EQ((int)val, 0x05);
}

TEST_CASE(f018b_base_addr_mask) {
    F018bFixture f;
    uint8_t val;

    // Only 16-byte range is valid ($D700-$D70F)
    ASSERT(f.dma.ioRead(&f.flatBus, 0xD700, &val));   // OK
    ASSERT(f.dma.ioRead(&f.flatBus, 0xD70F, &val));   // OK (boundary)
    ASSERT(!f.dma.ioRead(&f.flatBus, 0xD710, &val));  // Out of range
}

TEST_CASE(f018b_device_info) {
    F018bFixture f;
    DeviceInfo info;
    f.dma.getDeviceInfo(info);

    ASSERT_NE(info.name.length(), 0);
    ASSERT_EQ(info.baseAddr, 0xD700u);
    ASSERT_EQ(info.addrMask, 0x0Fu);
}

// ============================================================================
// DMA Copy Operation Tests
// ============================================================================

TEST_CASE(f018b_dma_simple_copy) {
    F018bFixture f;

    // Set up source data
    for (int i = 0; i < 256; ++i) {
        f.sparseBus.write8(0x0100 + i, i);
    }

    // Set up DMA job list at $2000
    uint32_t listAddr = 0x2000;
    f.sparseBus.write8(listAddr + 0, 0x00);      // Copy, no chain
    f.sparseBus.write8(listAddr + 1, 0x00);      // Count = 256
    f.sparseBus.write8(listAddr + 2, 0x01);      // Count MSB
    f.sparseBus.write8(listAddr + 3, 0x00);      // Src addr LSB
    f.sparseBus.write8(listAddr + 4, 0x01);      // Src addr MSB
    f.sparseBus.write8(listAddr + 5, 0x00);      // Src bank
    f.sparseBus.write8(listAddr + 6, 0x00);      // Dst addr LSB
    f.sparseBus.write8(listAddr + 7, 0x30);      // Dst addr MSB
    f.sparseBus.write8(listAddr + 8, 0x00);      // Dst bank
    f.sparseBus.write8(listAddr + 9, 0x00);      // Modulo LSB
    f.sparseBus.write8(listAddr + 10, 0x00);     // Modulo MSB

    // Trigger DMA via $D700 with list address
    f.dma.ioWrite(&f.sparseBus, 0xD701, 0x20);
    f.dma.ioWrite(&f.sparseBus, 0xD700, 0x00);

    // Run DMA (256 bytes = 256 ticks)
    for (int i = 0; i < 300; ++i) {
        f.dma.tick(1);
    }

    // Verify copy succeeded
    for (int i = 0; i < 256; ++i) {
        uint8_t dstVal = f.sparseBus.read8(0x3000 + i);
        ASSERT_EQ((int)dstVal, i);
    }
}

TEST_CASE(f018b_dma_fill_operation) {
    F018bFixture f;

    uint32_t listAddr = 0x2000;
    f.sparseBus.write8(listAddr + 0, 0x03);      // Fill operation
    f.sparseBus.write8(listAddr + 1, 0x00);      // Count = 128
    f.sparseBus.write8(listAddr + 2, 0x00);      // Count MSB
    f.sparseBus.write8(listAddr + 3, 0x42);      // Fill byte
    f.sparseBus.write8(listAddr + 4, 0x00);
    f.sparseBus.write8(listAddr + 5, 0x00);
    f.sparseBus.write8(listAddr + 6, 0x00);      // Dst addr LSB
    f.sparseBus.write8(listAddr + 7, 0x40);      // Dst addr MSB
    f.sparseBus.write8(listAddr + 8, 0x00);      // Dst bank
    f.sparseBus.write8(listAddr + 9, 0x00);
    f.sparseBus.write8(listAddr + 10, 0x00);

    f.dma.ioWrite(&f.sparseBus, 0xD701, 0x20);
    f.dma.ioWrite(&f.sparseBus, 0xD700, 0x00);

    for (int i = 0; i < 200; ++i) {
        f.dma.tick(1);
    }

    // Verify fill with 0x42
    for (int i = 0; i < 128; ++i) {
        uint8_t val = f.sparseBus.read8(0x4000 + i);
        ASSERT_EQ((int)val, 0x42);
    }
}

// ============================================================================
// SparseMemoryBus Integration Tests
// ============================================================================

TEST_CASE(f018b_dma_sparse_bus_allocation) {
    F018bFixture f;

    uint32_t srcAddr = 0x18000;  // 1MB + 32KB
    uint32_t dstAddr = 0x28000;  // 2MB + 32KB

    // Write source data
    for (int i = 0; i < 64; ++i) {
        f.sparseBus.write8(srcAddr + i, 0xAA);
    }

    // Set up DMA job
    uint32_t listAddr = 0x2000;
    f.sparseBus.write8(listAddr + 0, 0x00);      // Copy
    f.sparseBus.write8(listAddr + 1, 0x40);      // Count = 64
    f.sparseBus.write8(listAddr + 2, 0x00);
    f.sparseBus.write8(listAddr + 3, 0x00);      // Src LSB
    f.sparseBus.write8(listAddr + 4, 0x80);      // Src MSB
    f.sparseBus.write8(listAddr + 5, 0x01);      // Src bank
    f.sparseBus.write8(listAddr + 6, 0x00);      // Dst LSB
    f.sparseBus.write8(listAddr + 7, 0x80);      // Dst MSB
    f.sparseBus.write8(listAddr + 8, 0x02);      // Dst bank
    f.sparseBus.write8(listAddr + 9, 0x00);
    f.sparseBus.write8(listAddr + 10, 0x00);

    f.dma.ioWrite(&f.sparseBus, 0xD701, 0x20);
    f.dma.ioWrite(&f.sparseBus, 0xD700, 0x00);

    for (int i = 0; i < 150; ++i) {
        f.dma.tick(1);
    }

    // Verify copy across banks
    for (int i = 0; i < 64; ++i) {
        uint8_t val = f.sparseBus.read8(dstAddr + i);
        ASSERT_EQ((int)val, 0xAA);
    }
}

TEST_CASE(f018b_dma_not_active_by_default) {
    F018bFixture f;
    ASSERT(!f.dma.isHaltRequested());
}

TEST_CASE(f018b_dma_halt_request_during_transfer) {
    F018bFixture f;

    f.sparseBus.write8(0x2000, 0x00);  // Copy
    f.sparseBus.write8(0x2001, 0x20);  // Count = 32
    f.sparseBus.write8(0x2002, 0x00);
    f.sparseBus.write8(0x2003, 0x00);
    f.sparseBus.write8(0x2004, 0x00);
    f.sparseBus.write8(0x2005, 0x00);
    f.sparseBus.write8(0x2006, 0x00);
    f.sparseBus.write8(0x2007, 0x10);
    f.sparseBus.write8(0x2008, 0x00);
    f.sparseBus.write8(0x2009, 0x00);
    f.sparseBus.write8(0x200A, 0x00);

    f.dma.ioWrite(&f.sparseBus, 0xD701, 0x20);
    f.dma.ioWrite(&f.sparseBus, 0xD700, 0x00);

    ASSERT(f.dma.isHaltRequested());

    for (int i = 0; i < 100; ++i) {
        f.dma.tick(1);
    }
    ASSERT(!f.dma.isHaltRequested());
}

// ============================================================================
// Reset and State Tests
// ============================================================================

TEST_CASE(f018b_dma_reset_clears_state) {
    F018bFixture f;

    f.dma.ioWrite(&f.flatBus, 0xD700, 0xFF);
    f.dma.ioWrite(&f.flatBus, 0xD701, 0xAA);
    f.dma.ioWrite(&f.flatBus, 0xD702, 0x55);

    f.dma.reset();

    uint8_t val;
    f.dma.ioRead(&f.flatBus, 0xD700, &val);
    ASSERT_EQ((int)val, 0);
    f.dma.ioRead(&f.flatBus, 0xD701, &val);
    ASSERT_EQ((int)val, 0);
    f.dma.ioRead(&f.flatBus, 0xD702, &val);
    ASSERT_EQ((int)val, 0);
}

TEST_CASE(f018b_dma_aliases) {
    F018bFixture f;
    auto aliases = f.dma.deviceAliases();

    bool found_dma = false, found_f018b = false;
    for (const auto& alias : aliases) {
        if (alias == "DMA") found_dma = true;
        if (alias == "F018B") found_f018b = true;
    }
    ASSERT(found_dma);
    ASSERT(found_f018b);
}

TEST_CASE(f018b_dma_invalid_address_range) {
    F018bFixture f;
    uint8_t val = 0x42;

    ASSERT(!f.dma.ioRead(&f.flatBus, 0xD6FF, &val));
    ASSERT(!f.dma.ioWrite(&f.flatBus, 0xD710, val));
}
