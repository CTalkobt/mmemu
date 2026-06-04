#include "test_harness.h"
#include "../main/f018b_dma.h"
#include "libmem/main/ibus.h"
#include <cstring>
#include <vector>

/**
 * Mock memory bus for DMA testing
 * Provides simple 28-bit addressable memory
 */
class MockMemoryBus : public IBus {
public:
    static constexpr size_t MEMORY_SIZE = 0x500000;  // 5 MB for testing

    MockMemoryBus() {
        m_memory.resize(MEMORY_SIZE, 0);
    }

    uint8_t read8(uint32_t addr) override {
        addr &= 0x0FFFFFFF;  // Mask to 28-bit
        if (addr >= MEMORY_SIZE) return 0xFF;
        return m_memory[addr];
    }

    void write8(uint32_t addr, uint8_t val) override {
        addr &= 0x0FFFFFFF;  // Mask to 28-bit
        if (addr < MEMORY_SIZE) {
            m_memory[addr] = val;
        }
    }

    uint8_t peek8(uint32_t addr) override { return read8(addr); }
    bool isHaltRequested() override { return false; }

    int writeCount() const override { return 0; }
    void getWrites(uint32_t*, uint8_t*, uint8_t*, int) const override {}
    void clearWriteLog() override {}
    size_t stateSize() const override { return 0; }
    void saveState(uint8_t*) const override {}
    void loadState(const uint8_t*) override {}
    const BusConfig& config() const override {
        static BusConfig cfg = {0};
        return cfg;
    }
    const char* name() const override { return "MockMemoryBus"; }

    // Helper: fill memory region with pattern
    void fillRegion(uint32_t addr, uint8_t value, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            write8(addr + i, value);
        }
    }

    // Helper: verify memory region
    bool verifyRegion(uint32_t addr, uint8_t value, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            if (read8(addr + i) != value) return false;
        }
        return true;
    }

    // Helper: copy memory for verification
    void readRegion(uint32_t addr, uint8_t* buf, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            buf[i] = read8(addr + i);
        }
    }

private:
    std::vector<uint8_t> m_memory;
};

// Helper: write an F018 11-byte DMA job to memory (EN018B=0)
static void writeJobF018(MockMemoryBus& bus, uint32_t addr,
                         uint8_t cmd, uint16_t count,
                         uint32_t src, uint32_t dst,
                         uint16_t modulo = 0x0000) {
    bus.write8(addr + 0,  cmd);
    bus.write8(addr + 1,  count & 0xFF);
    bus.write8(addr + 2,  (count >> 8) & 0xFF);
    bus.write8(addr + 3,  src & 0xFF);           // Source LSB
    bus.write8(addr + 4,  (src >> 8) & 0xFF);    // Source MSB
    bus.write8(addr + 5,  (src >> 16) & 0x0F);   // Source BANK (lower nibble)
    bus.write8(addr + 6,  dst & 0xFF);           // Dest LSB
    bus.write8(addr + 7,  (dst >> 8) & 0xFF);    // Dest MSB
    bus.write8(addr + 8,  (dst >> 16) & 0x0F);   // Dest BANK (lower nibble)
    bus.write8(addr + 9,  modulo & 0xFF);        // Modulo LSB
    bus.write8(addr + 10, (modulo >> 8) & 0xFF); // Modulo MSB
}

// Helper: write an F018B 12-byte DMA job to memory (EN018B=1)
static void writeJobF018B(MockMemoryBus& bus, uint32_t addr,
                          uint8_t cmdLsb, uint16_t count,
                          uint32_t src, uint32_t dst,
                          uint8_t cmdMsb = 0x00, uint16_t modulo = 0x0000) {
    bus.write8(addr + 0,  cmdLsb);
    bus.write8(addr + 1,  count & 0xFF);
    bus.write8(addr + 2,  (count >> 8) & 0xFF);
    bus.write8(addr + 3,  src & 0xFF);
    bus.write8(addr + 4,  (src >> 8) & 0xFF);
    bus.write8(addr + 5,  (src >> 16) & 0x0F);
    bus.write8(addr + 6,  dst & 0xFF);
    bus.write8(addr + 7,  (dst >> 8) & 0xFF);
    bus.write8(addr + 8,  (dst >> 16) & 0x0F);
    bus.write8(addr + 9,  cmdMsb);               // Command MSB
    bus.write8(addr + 10, modulo & 0xFF);
    bus.write8(addr + 11, (modulo >> 8) & 0xFF);
}

// Helper: set up DMA list address registers and trigger via $D700
// Writes $D704, $D702, $D701 (non-triggering), then $D700 (triggers).
// After triggering, pumps tick() until DMA completes.
static void triggerDma(F018bDmaDevice& dma, MockMemoryBus& bus,
                       uint32_t listAddr) {
    dma.ioWrite(&bus, 0xD704, (listAddr >> 20) & 0xFF);  // ADDRMB, no trigger
    dma.ioWrite(&bus, 0xD702, (listAddr >> 16) & 0x0F);  // ADDRBANK, no trigger (resets D704!)
    // Re-write D704 after D702 since D702 resets it
    dma.ioWrite(&bus, 0xD704, (listAddr >> 20) & 0xFF);
    dma.ioWrite(&bus, 0xD701, (listAddr >> 8) & 0xFF);   // ADDRMSB, no trigger
    dma.ioWrite(&bus, 0xD700, listAddr & 0xFF);           // ADDRLSBTRIG — triggers DMA

    // Pump tick() until DMA completes (cycle-by-cycle execution)
    for (int i = 0; i < 1000000 && dma.isHaltRequested(); ++i) {
        dma.tick(1);
    }
}

// Helper: set up address and trigger Enhanced DMA via $D705
static void triggerEnhancedDma(F018bDmaDevice& dma, MockMemoryBus& bus,
                               uint32_t listAddr) {
    dma.ioWrite(&bus, 0xD702, (listAddr >> 16) & 0x0F);
    dma.ioWrite(&bus, 0xD704, (listAddr >> 20) & 0xFF);
    dma.ioWrite(&bus, 0xD701, (listAddr >> 8) & 0xFF);
    dma.ioWrite(&bus, 0xD705, listAddr & 0xFF);  // ETRIG — triggers enhanced DMA

    // Pump tick() until DMA completes
    for (int i = 0; i < 1000000 && dma.isHaltRequested(); ++i) {
        dma.tick(1);
    }
}

// ============================================================================
// Test: Register Access
// ============================================================================

TEST_CASE(dma_register_write_read) {
    F018bDmaDevice dma(0xD700);
    MockMemoryBus bus;

    // Write to DMA registers that don't trigger ($D703/$D704)
    dma.ioWrite(&bus, 0xD703, 0x01);  // EN018B
    dma.ioWrite(&bus, 0xD704, 0x00);

    // Read back
    uint8_t val;
    dma.ioRead(&bus, 0xD703, &val);
    ASSERT_EQ(val, 0x01);
    dma.ioRead(&bus, 0xD704, &val);
    ASSERT_EQ(val, 0x00);
}

// ============================================================================
// Test: $D701 and $D702 do NOT trigger DMA
// ============================================================================

TEST_CASE(dma_d701_d702_do_not_trigger) {
    F018bDmaDevice dma(0xD700);
    MockMemoryBus bus;

    // Set up a fill job at $030000
    writeJobF018(bus, 0x030000, 0x01, 8, 0x0000AA, 0x020000);
    bus.fillRegion(0x020000, 0x00, 8);

    dma.ioWrite(&bus, 0xD704, 0x00);

    // Write $D701 and $D702 — must NOT trigger DMA
    dma.ioWrite(&bus, 0xD701, 0x00);
    dma.ioWrite(&bus, 0xD702, 0x03);

    // Destination should still be zeros
    ASSERT(bus.verifyRegion(0x020000, 0x00, 8));
}

// ============================================================================
// Test: $D703 write does NOT trigger DMA
// ============================================================================

TEST_CASE(dma_d703_does_not_trigger) {
    F018bDmaDevice dma(0xD700);
    MockMemoryBus bus;

    writeJobF018(bus, 0x030000, 0x01, 8, 0x0000AA, 0x020000);
    bus.fillRegion(0x020000, 0x00, 8);

    dma.ioWrite(&bus, 0xD704, 0x00);
    dma.ioWrite(&bus, 0xD703, 0x01);

    ASSERT(bus.verifyRegion(0x020000, 0x00, 8));
}

// ============================================================================
// Test: $D700 write triggers DMA
// ============================================================================

TEST_CASE(dma_d700_triggers) {
    F018bDmaDevice dma(0xD700);
    MockMemoryBus bus;

    bus.fillRegion(0x010000, 0xAA, 16);
    writeJobF018(bus, 0x030000, 0x00, 16, 0x010000, 0x020000);

    // Set non-trigger registers first
    dma.ioWrite(&bus, 0xD702, 0x03);
    dma.ioWrite(&bus, 0xD704, 0x00);
    dma.ioWrite(&bus, 0xD701, 0x00);

    // Write $D700 — triggers DMA
    dma.ioWrite(&bus, 0xD700, 0x00);

    // Pump tick() until DMA completes
    for (int i = 0; i < 1000000 && dma.isHaltRequested(); ++i) {
        dma.tick(1);
    }

    ASSERT(bus.verifyRegion(0x020000, 0xAA, 16));
}

// ============================================================================
// Test: $D702 write resets $D704 (ADDRMB) to zero
// ============================================================================

TEST_CASE(dma_d702_resets_d704) {
    F018bDmaDevice dma(0xD700);
    MockMemoryBus bus;

    // Set ADDRMB to a non-zero value
    dma.ioWrite(&bus, 0xD704, 0x05);
    uint8_t val;
    dma.ioRead(&bus, 0xD704, &val);
    ASSERT_EQ(val, 0x05);

    // Write ADDRBANK ($D702) — should reset ADDRMB to 0
    dma.ioWrite(&bus, 0xD702, 0x03);
    dma.ioRead(&bus, 0xD704, &val);
    ASSERT_EQ(val, 0x00);
}

// ============================================================================
// Test: F018 DMA Copy (11-byte job, EN018B=0)
// ============================================================================

TEST_CASE(dma_copy_f018) {
    F018bDmaDevice dma(0xD700);
    MockMemoryBus bus;

    bus.fillRegion(0x010000, 0xAA, 16);
    writeJobF018(bus, 0x030000, 0x00, 16, 0x010000, 0x020000);

    // EN018B=0 (default) → F018 11-byte format
    triggerDma(dma, bus, 0x030000);

    ASSERT(bus.verifyRegion(0x020000, 0xAA, 16));
}

// ============================================================================
// Test: F018B DMA Copy (12-byte job, EN018B=1)
// ============================================================================

TEST_CASE(dma_copy_f018b) {
    F018bDmaDevice dma(0xD700);
    MockMemoryBus bus;

    bus.fillRegion(0x010000, 0xBB, 16);
    writeJobF018B(bus, 0x030000, 0x00, 16, 0x010000, 0x020000);

    // Set EN018B=1 → F018B 12-byte format
    dma.ioWrite(&bus, 0xD703, 0x01);

    triggerDma(dma, bus, 0x030000);

    ASSERT(bus.verifyRegion(0x020000, 0xBB, 16));
}

// ============================================================================
// Test: DMA Fill Operation (F018 format)
// ============================================================================

TEST_CASE(dma_fill_basic) {
    F018bDmaDevice dma(0xD700);
    MockMemoryBus bus;

    // Fill mode: source low byte = fill value
    writeJobF018(bus, 0x030000, 0x01, 32, 0x000055, 0x020000);

    triggerDma(dma, bus, 0x030000);

    ASSERT(bus.verifyRegion(0x020000, 0x55, 32));
}

// ============================================================================
// Test: DMA Swap Operation
// ============================================================================

TEST_CASE(dma_swap_basic) {
    F018bDmaDevice dma(0xD700);
    MockMemoryBus bus;

    bus.fillRegion(0x010000, 0xAA, 8);
    bus.fillRegion(0x020000, 0xBB, 8);

    writeJobF018(bus, 0x030000, 0x02, 8, 0x010000, 0x020000);

    triggerDma(dma, bus, 0x030000);

    ASSERT(bus.verifyRegion(0x010000, 0xBB, 8));
    ASSERT(bus.verifyRegion(0x020000, 0xAA, 8));
}

// ============================================================================
// Test: DMA Copy with Overlap (Forward)
// ============================================================================

TEST_CASE(dma_copy_overlap_forward) {
    F018bDmaDevice dma(0xD700);
    MockMemoryBus bus;

    for (int i = 0; i < 16; ++i) {
        bus.write8(0x010000 + i, 0x10 + i);
    }

    // Copy from $010000 to $010008 (overlapping, 16 bytes)
    writeJobF018(bus, 0x030000, 0x00, 16, 0x010000, 0x010008);

    triggerDma(dma, bus, 0x030000);

    uint8_t val = bus.read8(0x010008);
    ASSERT_EQ(val, 0x10);
}

// ============================================================================
// Test: F018 Chained Jobs (11 bytes per job)
// ============================================================================

TEST_CASE(dma_chained_f018) {
    F018bDmaDevice dma(0xD700);
    MockMemoryBus bus;

    bus.fillRegion(0x010000, 0x11, 8);
    bus.fillRegion(0x010100, 0x22, 8);

    // Job 1: copy with chain bit (11 bytes)
    writeJobF018(bus, 0x030000, 0x04, 8, 0x010000, 0x020000);

    // Job 2: copy without chain (starts at offset 11 = 0x0B)
    writeJobF018(bus, 0x03000B, 0x00, 8, 0x010100, 0x040000);

    triggerDma(dma, bus, 0x030000);

    ASSERT(bus.verifyRegion(0x020000, 0x11, 8));
    ASSERT(bus.verifyRegion(0x040000, 0x22, 8));
}

// ============================================================================
// Test: F018B Chained Jobs (12 bytes per job)
// ============================================================================

TEST_CASE(dma_chained_f018b) {
    F018bDmaDevice dma(0xD700);
    MockMemoryBus bus;

    bus.fillRegion(0x010000, 0x33, 8);
    bus.fillRegion(0x010100, 0x44, 8);

    // Job 1: copy with chain bit (12 bytes)
    writeJobF018B(bus, 0x030000, 0x04, 8, 0x010000, 0x020000);

    // Job 2: copy without chain (starts at offset 12 = 0x0C)
    writeJobF018B(bus, 0x03000C, 0x00, 8, 0x010100, 0x040000);

    // Set EN018B=1
    dma.ioWrite(&bus, 0xD703, 0x01);
    triggerDma(dma, bus, 0x030000);

    ASSERT(bus.verifyRegion(0x020000, 0x33, 8));
    ASSERT(bus.verifyRegion(0x040000, 0x44, 8));
}

// ============================================================================
// Test: F018 vs F018B chain offset matters
// ============================================================================

TEST_CASE(dma_chain_offset_f018_vs_f018b) {
    // Verify that using the wrong format produces incorrect results.
    // Place two fills in F018B format (12 bytes each).
    // If the engine used 11-byte stride, job 2 would parse incorrectly.
    F018bDmaDevice dma(0xD700);
    MockMemoryBus bus;

    bus.fillRegion(0x020000, 0x00, 16);
    bus.fillRegion(0x020100, 0x00, 16);

    // Job 1 at $030000: fill 8 bytes at $020000 with $AA (chain)
    writeJobF018B(bus, 0x030000, 0x05, 8, 0x0000AA, 0x020000);

    // Job 2 at $03000C (offset 12): fill 8 bytes at $020100 with $BB
    writeJobF018B(bus, 0x03000C, 0x01, 8, 0x0000BB, 0x020100);

    dma.ioWrite(&bus, 0xD703, 0x01);  // EN018B=1
    triggerDma(dma, bus, 0x030000);

    ASSERT(bus.verifyRegion(0x020000, 0xAA, 8));
    ASSERT(bus.verifyRegion(0x020100, 0xBB, 8));
}

// ============================================================================
// Test: CPU Halt Request
// ============================================================================

TEST_CASE(dma_cpu_halt_request) {
    F018bDmaDevice dma(0xD700);
    MockMemoryBus bus;

    ASSERT(!dma.isHaltRequested());

    bus.fillRegion(0x010000, 0xCC, 8);
    writeJobF018(bus, 0x030000, 0x00, 8, 0x010000, 0x020000);

    triggerDma(dma, bus, 0x030000);

    // After DMA completes (ticks pumped by triggerDma), halt should be cleared
    ASSERT(!dma.isHaltRequested());
}

// ============================================================================
// Test: Device Reset
// ============================================================================

TEST_CASE(dma_reset) {
    F018bDmaDevice dma(0xD700);
    MockMemoryBus bus;

    dma.ioWrite(&bus, 0xD703, 0x01);
    dma.ioWrite(&bus, 0xD704, 0x12);

    uint8_t val;
    dma.ioRead(&bus, 0xD703, &val);
    ASSERT_EQ(val, 0x01);

    dma.reset();

    dma.ioRead(&bus, 0xD703, &val);
    ASSERT_EQ(val, 0x00);
    dma.ioRead(&bus, 0xD704, &val);
    ASSERT_EQ(val, 0x00);
}

// ============================================================================
// Test: Address Masking (Out of Range)
// ============================================================================

TEST_CASE(dma_address_out_of_range) {
    F018bDmaDevice dma(0xD700);
    MockMemoryBus bus;

    uint8_t val = 0xFF;
    bool result = dma.ioWrite(&bus, 0xD710, val);
    ASSERT(!result);

    result = dma.ioRead(&bus, 0xD720, &val);
    ASSERT(!result);
}

// ============================================================================
// Test: Bank Register Address Extension
// ============================================================================

TEST_CASE(dma_bank_register_address) {
    F018bDmaDevice dma(0xD700);
    MockMemoryBus bus;

    bus.fillRegion(0x030100, 0xAA, 16);
    writeJobF018(bus, 0x030000, 0x00, 16, 0x030100, 0x030200);

    triggerDma(dma, bus, 0x030000);

    ASSERT(bus.verifyRegion(0x030200, 0xAA, 16));
}

// ============================================================================
// Test: Empty Job List (Zero Count)
// ============================================================================

TEST_CASE(dma_zero_count) {
    F018bDmaDevice dma(0xD700);
    MockMemoryBus bus;

    writeJobF018(bus, 0x030000, 0x00, 0, 0x010000, 0x020000);

    triggerDma(dma, bus, 0x030000);

    ASSERT(true);  // No crash
}

// ============================================================================
// Test: Enhanced DMA Jobs with Fractional Stepping
// ============================================================================

TEST_CASE(dma_enhanced_fractional_stepping_copy) {
    F018bDmaDevice dma(0xD700);
    MockMemoryBus bus;

    for (int i = 0; i < 16; ++i) {
        bus.write8(0x010000 + i, i);
    }

    // Enhanced DMA job option tokens
    uint32_t jobAddr = 0x030000;
    bus.write8(jobAddr + 0, 0x83);  // Source skip rate (whole bytes)
    bus.write8(jobAddr + 1, 0x02);  // 2.0 bytes per iteration
    bus.write8(jobAddr + 2, 0x85);  // Destination skip rate (whole bytes)
    bus.write8(jobAddr + 3, 0x01);  // 1.0 bytes per iteration
    bus.write8(jobAddr + 4, 0x00);  // End of options

    // F018 11-byte DMA job after options
    uint32_t dmaJobAddr = jobAddr + 5;
    writeJobF018(bus, dmaJobAddr, 0x00, 8, 0x010000, 0x020000);

    // Clear destination
    bus.fillRegion(0x020000, 0xFF, 8);

    // Trigger Enhanced DMA via $D705
    triggerEnhancedDma(dma, bus, 0x030000);

    // With skip rates of 2.0 (src) and 1.0 (dst):
    // src[0]=0 -> dst[0], src[2]=2 -> dst[1], etc.
    ASSERT_EQ(bus.read8(0x020000), 0);
    ASSERT_EQ(bus.read8(0x020001), 2);
    ASSERT_EQ(bus.read8(0x020002), 4);
    ASSERT_EQ(bus.read8(0x020003), 6);
}

// ============================================================================
// Test: Source BANK+FLAGS byte masks correctly
// ============================================================================

TEST_CASE(dma_bank_flags_masking) {
    F018bDmaDevice dma(0xD700);
    MockMemoryBus bus;

    bus.fillRegion(0x010000, 0xDD, 8);

    // Write job with flags in upper nibble of BANK bytes
    uint32_t addr = 0x030000;
    bus.write8(addr + 0,  0x00);  // copy
    bus.write8(addr + 1,  0x08);  // count=8
    bus.write8(addr + 2,  0x00);
    bus.write8(addr + 3,  0x00);  // src LSB
    bus.write8(addr + 4,  0x00);  // src MSB
    bus.write8(addr + 5,  0x81);  // src BANK=1, FLAGS=8 (upper nibble set)
    bus.write8(addr + 6,  0x00);  // dst LSB
    bus.write8(addr + 7,  0x00);  // dst MSB
    bus.write8(addr + 8,  0x82);  // dst BANK=2, FLAGS=8
    bus.write8(addr + 9,  0x00);  // modulo LSB
    bus.write8(addr + 10, 0x00);  // modulo MSB

    triggerDma(dma, bus, 0x030000);

    // Source should resolve to bank 1 ($010000), dst to bank 2 ($020000)
    // Flags in upper nibble should NOT corrupt the address
    ASSERT(bus.verifyRegion(0x020000, 0xDD, 8));
}
