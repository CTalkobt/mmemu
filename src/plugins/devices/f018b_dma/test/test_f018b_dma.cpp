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

// Helper: write an 11-byte F018 DMA job to memory
static void writeJob(MockMemoryBus& bus, uint32_t addr,
                     uint8_t cmd, uint16_t count,
                     uint32_t src, uint32_t dst,
                     uint8_t subCmd = 0x00, uint8_t modulo = 0x00) {
    bus.write8(addr + 0,  cmd);
    bus.write8(addr + 1,  count & 0xFF);
    bus.write8(addr + 2,  (count >> 8) & 0xFF);
    bus.write8(addr + 3,  src & 0xFF);
    bus.write8(addr + 4,  (src >> 8) & 0xFF);
    bus.write8(addr + 5,  (src >> 16) & 0xFF);
    bus.write8(addr + 6,  dst & 0xFF);
    bus.write8(addr + 7,  (dst >> 8) & 0xFF);
    bus.write8(addr + 8,  (dst >> 16) & 0xFF);
    bus.write8(addr + 9,  subCmd);
    bus.write8(addr + 10, modulo);
}

// Helper: set up DMA list address registers (without triggering)
static void setListAddr(F018bDmaDevice& dma, MockMemoryBus& bus,
                        uint32_t listAddr) {
    // Write $D704 (upper bits) and $D703 (flags) first — these don't trigger
    dma.ioWrite(&bus, 0xD704, (listAddr >> 20) & 0xFF);
    dma.ioWrite(&bus, 0xD703, 0x00);  // EN018B/NOMBWRAP flags, no trigger
}

// Helper: trigger DMA by writing address low byte ($D700)
static void triggerDma(F018bDmaDevice& dma, MockMemoryBus& bus,
                       uint32_t listAddr) {
    // Write high and bank first (each triggers, but address isn't complete yet —
    // in practice the last write sets the final address and triggers).
    // To avoid premature triggers from $D701/$D702, we set those via $D703
    // approach: store mid/bank in regs without trigger, then trigger via $D700.
    // Actually on real hardware every write to $D700-$D702 triggers.
    // For testing, we set up the full address then write low byte last to trigger.
    dma.ioWrite(&bus, 0xD703, 0x00);  // flags only, no trigger
    dma.ioWrite(&bus, 0xD704, (listAddr >> 20) & 0xFF);  // upper, no trigger

    // These each trigger DMA, but the last one ($D700) is the canonical trigger.
    // We write $D701 and $D702 first, then $D700 last.
    dma.ioWrite(&bus, 0xD702, (listAddr >> 16) & 0x0F);
    dma.ioWrite(&bus, 0xD701, (listAddr >> 8) & 0xFF);
    dma.ioWrite(&bus, 0xD700, listAddr & 0xFF);
}

// ============================================================================
// Test: Register Access
// ============================================================================

TEST_CASE(dma_register_write_read) {
    F018bDmaDevice dma(0xD700);
    MockMemoryBus bus;

    // Write to DMA registers (use $D703/$D704 which don't trigger)
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
// Test: $D703 write does NOT trigger DMA
// ============================================================================

TEST_CASE(dma_d703_does_not_trigger) {
    F018bDmaDevice dma(0xD700);
    MockMemoryBus bus;

    // Set up a fill job at $030000
    writeJob(bus, 0x030000, 0x01, 8, 0xAA, 0x020000);

    // Pre-fill destination with known value
    bus.fillRegion(0x020000, 0x00, 8);

    // Set up the list address via non-trigger registers
    dma.ioWrite(&bus, 0xD704, 0x00);

    // Write $D703 — this must NOT trigger DMA
    dma.ioWrite(&bus, 0xD703, 0x01);

    // Destination should still be zeros (DMA did not execute)
    ASSERT(bus.verifyRegion(0x020000, 0x00, 8));
}

// ============================================================================
// Test: $D700 write triggers DMA
// ============================================================================

TEST_CASE(dma_d700_triggers) {
    F018bDmaDevice dma(0xD700);
    MockMemoryBus bus;

    bus.fillRegion(0x010000, 0xAA, 16);
    writeJob(bus, 0x030000, 0x00, 16, 0x010000, 0x020000);

    // Set non-trigger registers first
    dma.ioWrite(&bus, 0xD704, 0x00);
    dma.ioWrite(&bus, 0xD703, 0x00);

    // Write $D702 and $D701 (these trigger too, but list addr is incomplete)
    dma.ioWrite(&bus, 0xD702, 0x03);
    dma.ioWrite(&bus, 0xD701, 0x00);

    // Write $D700 — triggers DMA with complete address $030000
    dma.ioWrite(&bus, 0xD700, 0x00);

    ASSERT(bus.verifyRegion(0x020000, 0xAA, 16));
}

// ============================================================================
// Test: DMA Copy Operation (11-byte job list)
// ============================================================================

TEST_CASE(dma_copy_basic) {
    F018bDmaDevice dma(0xD700);
    MockMemoryBus bus;

    bus.fillRegion(0x010000, 0xAA, 16);
    writeJob(bus, 0x030000, 0x00, 16, 0x010000, 0x020000);

    triggerDma(dma, bus, 0x030000);

    ASSERT(bus.verifyRegion(0x020000, 0xAA, 16));
}

// ============================================================================
// Test: DMA Fill Operation
// ============================================================================

TEST_CASE(dma_fill_basic) {
    F018bDmaDevice dma(0xD700);
    MockMemoryBus bus;

    // Fill mode: source low byte = fill value
    writeJob(bus, 0x030000, 0x01, 32, 0x000055, 0x020000);

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

    writeJob(bus, 0x030000, 0x02, 8, 0x010000, 0x020000);

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
    writeJob(bus, 0x030000, 0x00, 16, 0x010000, 0x010008);

    triggerDma(dma, bus, 0x030000);

    uint8_t val = bus.read8(0x010008);
    ASSERT_EQ(val, 0x10);
}

// ============================================================================
// Test: DMA Chained Jobs (11 bytes per job)
// ============================================================================

TEST_CASE(dma_chained_jobs) {
    F018bDmaDevice dma(0xD700);
    MockMemoryBus bus;

    bus.fillRegion(0x010000, 0x11, 8);
    bus.fillRegion(0x010100, 0x22, 8);

    // Job 1: copy with chain bit set (11 bytes)
    writeJob(bus, 0x030000, 0x04, 8, 0x010000, 0x020000);

    // Job 2: copy without chain (11 bytes, starts at offset 11)
    writeJob(bus, 0x03000B, 0x00, 8, 0x010100, 0x040000);

    triggerDma(dma, bus, 0x030000);

    ASSERT(bus.verifyRegion(0x020000, 0x11, 8));
    ASSERT(bus.verifyRegion(0x040000, 0x22, 8));
}

// ============================================================================
// Test: CPU Halt Request
// ============================================================================

TEST_CASE(dma_cpu_halt_request) {
    F018bDmaDevice dma(0xD700);
    MockMemoryBus bus;

    ASSERT(!dma.isHaltRequested());

    bus.fillRegion(0x010000, 0xCC, 8);
    writeJob(bus, 0x030000, 0x00, 8, 0x010000, 0x020000);

    triggerDma(dma, bus, 0x030000);

    // After DMA completes synchronously, halt should be cleared
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
    writeJob(bus, 0x030000, 0x00, 16, 0x030100, 0x030200);

    triggerDma(dma, bus, 0x030000);

    ASSERT(bus.verifyRegion(0x030200, 0xAA, 16));
}

// ============================================================================
// Test: Empty Job List (Zero Count)
// ============================================================================

TEST_CASE(dma_zero_count) {
    F018bDmaDevice dma(0xD700);
    MockMemoryBus bus;

    writeJob(bus, 0x030000, 0x00, 0, 0x010000, 0x020000);

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

    // 11-byte F018 DMA job
    uint32_t dmaJobAddr = jobAddr + 5;
    writeJob(bus, dmaJobAddr, 0x00, 8, 0x010000, 0x020000);

    // Set address registers (non-triggering)
    dma.ioWrite(&bus, 0xD704, 0x00);
    dma.ioWrite(&bus, 0xD703, 0x00);

    // Trigger Enhanced DMA via $D705
    // But first set the address bytes in the shadow registers.
    // $D705 reads list addr from the same shadow regs.
    // We need to set $D700-$D702 without triggering standard DMA first.
    // Since writing $D700-$D702 triggers standard DMA, we store the values
    // directly and then trigger enhanced via $D705.
    // For this test, we rely on the fact that $D702 trigger will run
    // a standard (non-enhanced) DMA, then $D705 runs enhanced.
    // Instead, set list address via triggering registers (standard DMA runs
    // but with wrong enhanced options — no harm since options only apply
    // to enhanced mode), then trigger enhanced.
    dma.ioWrite(&bus, 0xD702, 0x03);
    dma.ioWrite(&bus, 0xD701, 0x00);
    dma.ioWrite(&bus, 0xD700, 0x00);

    // Clear destination to verify enhanced DMA writes correctly
    bus.fillRegion(0x020000, 0xFF, 8);

    // Trigger Enhanced DMA
    dma.ioWrite(&bus, 0xD705, 0x01);

    // With skip rates of 2.0 (src) and 1.0 (dst):
    // src[0]=0 -> dst[0], src[2]=2 -> dst[1], etc.
    ASSERT_EQ(bus.read8(0x020000), 0);
    ASSERT_EQ(bus.read8(0x020001), 2);
    ASSERT_EQ(bus.read8(0x020002), 4);
    ASSERT_EQ(bus.read8(0x020003), 6);
}

// ============================================================================
// Test: 11-byte job list format verified
// ============================================================================

TEST_CASE(dma_11_byte_job_list) {
    F018bDmaDevice dma(0xD700);
    MockMemoryBus bus;

    // Place two distinct fill values in two chained jobs.
    // If the engine incorrectly uses 10-byte jobs, job 2 will read from
    // the wrong offset and produce incorrect results.

    bus.fillRegion(0x020000, 0x00, 16);
    bus.fillRegion(0x020100, 0x00, 16);

    // Job 1 at $030000: fill 8 bytes at $020000 with $AA (chain bit set)
    writeJob(bus, 0x030000, 0x05, 8, 0x0000AA, 0x020000);
    //  0x05 = fill (0x01) | chain (0x04)

    // Job 2 at $03000B (offset 11): fill 8 bytes at $020100 with $BB
    writeJob(bus, 0x03000B, 0x01, 8, 0x0000BB, 0x020100);

    triggerDma(dma, bus, 0x030000);

    ASSERT(bus.verifyRegion(0x020000, 0xAA, 8));
    ASSERT(bus.verifyRegion(0x020100, 0xBB, 8));
}
