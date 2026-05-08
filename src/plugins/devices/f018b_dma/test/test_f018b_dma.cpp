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

// ============================================================================
// Test: Register Access
// ============================================================================

TEST_CASE(dma_register_write_read) {
    F018bDmaDevice dma(0xD700);
    MockMemoryBus bus;

    // Write to DMA registers
    dma.ioWrite(&bus, 0xD700, 0x12);
    dma.ioWrite(&bus, 0xD701, 0x34);
    dma.ioWrite(&bus, 0xD702, 0x05);
    dma.ioWrite(&bus, 0xD704, 0x00);

    // Read back
    uint8_t val;
    dma.ioRead(&bus, 0xD700, &val);
    ASSERT_EQ(val, 0x12);
    dma.ioRead(&bus, 0xD701, &val);
    ASSERT_EQ(val, 0x34);
    dma.ioRead(&bus, 0xD702, &val);
    ASSERT_EQ(val, 0x05);
}

// ============================================================================
// Test: DMA Copy Operation
// ============================================================================

TEST_CASE(dma_copy_basic) {
    F018bDmaDevice dma(0xD700);
    MockMemoryBus bus;

    // Fill source region with known pattern
    bus.fillRegion(0x010000, 0xAA, 16);

    // Create DMA job: copy 16 bytes from $010000 to $020000
    // Job at $030000:
    uint32_t jobAddr = 0x030000;
    bus.write8(jobAddr + 0, 0x00);  // Command: copy (bits 0-1=00)
    bus.write8(jobAddr + 1, 0x10);  // Count low = 16
    bus.write8(jobAddr + 2, 0x00);  // Count high = 0
    bus.write8(jobAddr + 3, 0x00);  // Source low
    bus.write8(jobAddr + 4, 0x00);  // Source mid
    bus.write8(jobAddr + 5, 0x01);  // Source high = $01
    bus.write8(jobAddr + 6, 0x00);  // Dest low
    bus.write8(jobAddr + 7, 0x00);  // Dest mid
    bus.write8(jobAddr + 8, 0x02);  // Dest high = $02
    bus.write8(jobAddr + 9, 0x00);  // End of chain

    // Set DMA list address
    dma.ioWrite(&bus, 0xD700, 0x00);  // List low
    dma.ioWrite(&bus, 0xD701, 0x00);  // List mid
    dma.ioWrite(&bus, 0xD702, 0x03);  // List bank = $03
    dma.ioWrite(&bus, 0xD704, 0x00);  // List upper = $00

    // Trigger DMA
    dma.ioWrite(&bus, 0xD703, 0x01);

    // Verify copy succeeded
    ASSERT(bus.verifyRegion(0x020000, 0xAA, 16));
}

// ============================================================================
// Test: DMA Fill Operation
// ============================================================================

TEST_CASE(dma_fill_basic) {
    F018bDmaDevice dma(0xD700);
    MockMemoryBus bus;

    // Create DMA fill job: fill 32 bytes at $020000 with value $55
    // In fill mode, fill byte is source address low byte
    uint32_t jobAddr = 0x030000;
    bus.write8(jobAddr + 0, 0x01);  // Command: fill (bits 0-1=01)
    bus.write8(jobAddr + 1, 0x20);  // Count low = 32
    bus.write8(jobAddr + 2, 0x00);  // Count high = 0
    bus.write8(jobAddr + 3, 0x55);  // Fill byte (source low in fill mode)
    bus.write8(jobAddr + 4, 0x00);  // Source mid
    bus.write8(jobAddr + 5, 0x00);  // Source high
    bus.write8(jobAddr + 6, 0x00);  // Dest low
    bus.write8(jobAddr + 7, 0x00);  // Dest mid
    bus.write8(jobAddr + 8, 0x02);  // Dest high = $02
    bus.write8(jobAddr + 9, 0x00);  // End of chain

    // Set DMA list address
    dma.ioWrite(&bus, 0xD700, 0x00);
    dma.ioWrite(&bus, 0xD701, 0x00);
    dma.ioWrite(&bus, 0xD702, 0x03);
    dma.ioWrite(&bus, 0xD704, 0x00);

    // Trigger DMA
    dma.ioWrite(&bus, 0xD703, 0x01);

    // Verify fill succeeded
    ASSERT(bus.verifyRegion(0x020000, 0x55, 32));
}

// ============================================================================
// Test: DMA Swap Operation
// ============================================================================

TEST_CASE(dma_swap_basic) {
    F018bDmaDevice dma(0xD700);
    MockMemoryBus bus;

    // Setup source with 0xAA and destination with 0xBB
    bus.fillRegion(0x010000, 0xAA, 8);
    bus.fillRegion(0x020000, 0xBB, 8);

    // Create DMA swap job
    uint32_t jobAddr = 0x030000;
    bus.write8(jobAddr + 0, 0x02);  // Command: swap (bits 0-1=10)
    bus.write8(jobAddr + 1, 0x08);  // Count = 8
    bus.write8(jobAddr + 2, 0x00);
    bus.write8(jobAddr + 3, 0x00);  // Source low
    bus.write8(jobAddr + 4, 0x00);  // Source mid
    bus.write8(jobAddr + 5, 0x01);  // Source high
    bus.write8(jobAddr + 6, 0x00);  // Dest low
    bus.write8(jobAddr + 7, 0x00);  // Dest mid
    bus.write8(jobAddr + 8, 0x02);  // Dest high
    bus.write8(jobAddr + 9, 0x00);  // End

    // Set DMA list address
    dma.ioWrite(&bus, 0xD700, 0x00);
    dma.ioWrite(&bus, 0xD701, 0x00);
    dma.ioWrite(&bus, 0xD702, 0x03);
    dma.ioWrite(&bus, 0xD704, 0x00);

    // Trigger DMA
    dma.ioWrite(&bus, 0xD703, 0x01);

    // After swap: source should have 0xBB, destination should have 0xAA
    ASSERT(bus.verifyRegion(0x010000, 0xBB, 8));
    ASSERT(bus.verifyRegion(0x020000, 0xAA, 8));
}

// ============================================================================
// Test: DMA Copy with Overlap (Forward)
// ============================================================================

TEST_CASE(dma_copy_overlap_forward) {
    F018bDmaDevice dma(0xD700);
    MockMemoryBus bus;

    // Fill source region
    for (int i = 0; i < 16; ++i) {
        bus.write8(0x010000 + i, 0x10 + i);
    }

    // Copy from $010000 to $010008 (overlapping, 16 bytes)
    // This should forward-copy since dst > src
    uint32_t jobAddr = 0x030000;
    bus.write8(jobAddr + 0, 0x00);  // Command: copy
    bus.write8(jobAddr + 1, 0x10);  // Count = 16
    bus.write8(jobAddr + 2, 0x00);
    bus.write8(jobAddr + 3, 0x00);  // Source = $010000
    bus.write8(jobAddr + 4, 0x00);
    bus.write8(jobAddr + 5, 0x01);
    bus.write8(jobAddr + 6, 0x08);  // Dest = $010008
    bus.write8(jobAddr + 7, 0x00);
    bus.write8(jobAddr + 8, 0x01);
    bus.write8(jobAddr + 9, 0x00);  // End

    dma.ioWrite(&bus, 0xD700, 0x00);
    dma.ioWrite(&bus, 0xD701, 0x00);
    dma.ioWrite(&bus, 0xD702, 0x03);
    dma.ioWrite(&bus, 0xD704, 0x00);
    dma.ioWrite(&bus, 0xD703, 0x01);

    // Verify the copy worked (first byte at destination should be 0x10)
    uint8_t val;
    bus.read8(0x010008);
    val = bus.read8(0x010008);
    ASSERT_EQ(val, 0x10);
}

// ============================================================================
// Test: DMA Chained Jobs
// ============================================================================

TEST_CASE(dma_chained_jobs) {
    F018bDmaDevice dma(0xD700);
    MockMemoryBus bus;

    // Fill source
    bus.fillRegion(0x010000, 0x11, 8);
    bus.fillRegion(0x010100, 0x22, 8);

    // Job 1: copy 8 bytes from $010000 to $020000 (with chain bit set)
    uint32_t job1Addr = 0x030000;
    bus.write8(job1Addr + 0, 0x04);  // Command: copy with chain (bit 2 = 1)
    bus.write8(job1Addr + 1, 0x08);  // Count = 8
    bus.write8(job1Addr + 2, 0x00);
    bus.write8(job1Addr + 3, 0x00);  // Source = $010000
    bus.write8(job1Addr + 4, 0x00);
    bus.write8(job1Addr + 5, 0x01);
    bus.write8(job1Addr + 6, 0x00);  // Dest = $020000
    bus.write8(job1Addr + 7, 0x00);
    bus.write8(job1Addr + 8, 0x02);
    bus.write8(job1Addr + 9, 0x04);  // Chain indicator

    // Job 2: copy 8 bytes from $010100 to $040000 (no chain)
    uint32_t job2Addr = 0x03000A;  // Immediately after Job 1
    bus.write8(job2Addr + 0, 0x00);  // Command: copy, no chain
    bus.write8(job2Addr + 1, 0x08);  // Count = 8
    bus.write8(job2Addr + 2, 0x00);
    bus.write8(job2Addr + 3, 0x00);  // Source = $010100
    bus.write8(job2Addr + 4, 0x01);
    bus.write8(job2Addr + 5, 0x01);
    bus.write8(job2Addr + 6, 0x00);  // Dest = $040000
    bus.write8(job2Addr + 7, 0x00);
    bus.write8(job2Addr + 8, 0x04);
    bus.write8(job2Addr + 9, 0x00);  // End

    // Set DMA list address to job1
    dma.ioWrite(&bus, 0xD700, 0x00);
    dma.ioWrite(&bus, 0xD701, 0x00);
    dma.ioWrite(&bus, 0xD702, 0x03);
    dma.ioWrite(&bus, 0xD704, 0x00);

    // Trigger DMA
    dma.ioWrite(&bus, 0xD703, 0x01);

    // Verify both jobs executed
    ASSERT(bus.verifyRegion(0x020000, 0x11, 8));
    ASSERT(bus.verifyRegion(0x040000, 0x22, 8));
}

// ============================================================================
// Test: CPU Halt Request
// ============================================================================

TEST_CASE(dma_cpu_halt_request) {
    F018bDmaDevice dma(0xD700);
    MockMemoryBus bus;

    // Initially not halting
    ASSERT(!dma.isHaltRequested());

    // Setup a simple copy job
    bus.fillRegion(0x010000, 0xCC, 8);

    uint32_t jobAddr = 0x030000;
    bus.write8(jobAddr + 0, 0x00);  // Command: copy
    bus.write8(jobAddr + 1, 0x08);  // Count = 8
    bus.write8(jobAddr + 2, 0x00);
    bus.write8(jobAddr + 3, 0x00);
    bus.write8(jobAddr + 4, 0x00);
    bus.write8(jobAddr + 5, 0x01);
    bus.write8(jobAddr + 6, 0x00);
    bus.write8(jobAddr + 7, 0x00);
    bus.write8(jobAddr + 8, 0x02);
    bus.write8(jobAddr + 9, 0x00);

    dma.ioWrite(&bus, 0xD700, 0x00);
    dma.ioWrite(&bus, 0xD701, 0x00);
    dma.ioWrite(&bus, 0xD702, 0x03);
    dma.ioWrite(&bus, 0xD704, 0x00);

    // After DMA completes, halt should be cleared
    dma.ioWrite(&bus, 0xD703, 0x01);
    ASSERT(!dma.isHaltRequested());
}

// ============================================================================
// Test: Device Reset
// ============================================================================

TEST_CASE(dma_reset) {
    F018bDmaDevice dma(0xD700);
    MockMemoryBus bus;

    // Write to registers
    dma.ioWrite(&bus, 0xD700, 0x12);
    dma.ioWrite(&bus, 0xD701, 0x34);

    // Verify writes
    uint8_t val;
    dma.ioRead(&bus, 0xD700, &val);
    ASSERT_EQ(val, 0x12);

    // Reset
    dma.reset();

    // Verify registers cleared
    dma.ioRead(&bus, 0xD700, &val);
    ASSERT_EQ(val, 0x00);
    dma.ioRead(&bus, 0xD701, &val);
    ASSERT_EQ(val, 0x00);
}

// ============================================================================
// Test: Address Masking (Out of Range)
// ============================================================================

TEST_CASE(dma_address_out_of_range) {
    F018bDmaDevice dma(0xD700);
    MockMemoryBus bus;

    // Try to write to address outside 16-byte register range
    uint8_t val = 0xFF;
    bool result = dma.ioWrite(&bus, 0xD710, val);  // Outside range
    ASSERT(!result);  // Should return false for out-of-range

    result = dma.ioRead(&bus, 0xD720, &val);
    ASSERT(!result);  // Should return false
}

// ============================================================================
// Test: Bank Register Address Extension
// ============================================================================

TEST_CASE(dma_bank_register_address) {
    F018bDmaDevice dma(0xD700);
    MockMemoryBus bus;

    // Test that bank register doesn't affect addresses (bank = 0)
    // This just verifies the mechanism works for mixed high/low addresses
    bus.fillRegion(0x030100, 0xAA, 16);

    uint32_t jobAddr = 0x030000;
    bus.write8(jobAddr + 0, 0x00);  // Command: copy
    bus.write8(jobAddr + 1, 0x10);  // Count = 16
    bus.write8(jobAddr + 2, 0x00);
    // Source = 0x030100
    bus.write8(jobAddr + 3, 0x00);  // Source low byte
    bus.write8(jobAddr + 4, 0x01);  // Source mid byte
    bus.write8(jobAddr + 5, 0x03);  // Source high byte
    // Dest = 0x030200
    bus.write8(jobAddr + 6, 0x00);  // Dest low byte
    bus.write8(jobAddr + 7, 0x02);  // Dest mid byte
    bus.write8(jobAddr + 8, 0x03);  // Dest high byte
    bus.write8(jobAddr + 9, 0x00);

    dma.ioWrite(&bus, 0xD700, 0x00);
    dma.ioWrite(&bus, 0xD701, 0x00);
    dma.ioWrite(&bus, 0xD702, 0x03);
    dma.ioWrite(&bus, 0xD704, 0x00);  // Bank = 0

    dma.ioWrite(&bus, 0xD703, 0x01);

    // Verify copy succeeded
    ASSERT(bus.verifyRegion(0x030200, 0xAA, 16));
}

// ============================================================================
// Test: Empty Job List (Zero Count)
// ============================================================================

TEST_CASE(dma_zero_count) {
    F018bDmaDevice dma(0xD700);
    MockMemoryBus bus;

    // Create job with count = 0
    uint32_t jobAddr = 0x030000;
    bus.write8(jobAddr + 0, 0x00);  // Command: copy
    bus.write8(jobAddr + 1, 0x00);  // Count = 0 (no transfer)
    bus.write8(jobAddr + 2, 0x00);
    bus.write8(jobAddr + 3, 0x00);
    bus.write8(jobAddr + 4, 0x00);
    bus.write8(jobAddr + 5, 0x01);
    bus.write8(jobAddr + 6, 0x00);
    bus.write8(jobAddr + 7, 0x00);
    bus.write8(jobAddr + 8, 0x02);
    bus.write8(jobAddr + 9, 0x00);

    dma.ioWrite(&bus, 0xD700, 0x00);
    dma.ioWrite(&bus, 0xD701, 0x00);
    dma.ioWrite(&bus, 0xD702, 0x03);
    dma.ioWrite(&bus, 0xD704, 0x00);

    // Should not crash with zero count
    dma.ioWrite(&bus, 0xD703, 0x01);
    ASSERT(true);  // If we got here, no crash occurred
}

// ============================================================================
// Test: Enhanced DMA Jobs with Fractional Stepping
// ============================================================================

TEST_CASE(dma_enhanced_fractional_stepping_copy) {
    F018bDmaDevice dma(0xD700);
    MockMemoryBus bus;

    // Fill source region with pattern
    for (int i = 0; i < 16; ++i) {
        bus.write8(0x010000 + i, i);
    }

    // Create Enhanced DMA job list with skip rates
    // Skip rate format: high byte = integer bytes, low byte = 256ths
    // $0200 = 2.0 bytes per iteration (skip every other byte)
    uint32_t jobListAddr = 0x030000;
    uint32_t jobAddr = jobListAddr;

    // Enhanced DMA job option tokens
    bus.write8(jobAddr + 0, 0x83);  // Source skip rate (whole bytes)
    bus.write8(jobAddr + 1, 0x02);  // 2.0 bytes per iteration
    bus.write8(jobAddr + 2, 0x85);  // Destination skip rate (whole bytes)
    bus.write8(jobAddr + 3, 0x01);  // 1.0 bytes per iteration
    bus.write8(jobAddr + 4, 0x00);  // End of options

    // Standard F018B DMA job (copy 8 iterations)
    uint32_t dmaJobAddr = jobAddr + 5;
    bus.write8(dmaJobAddr + 0, 0x00);   // Command: copy
    bus.write8(dmaJobAddr + 1, 0x08);   // Count = 8 iterations
    bus.write8(dmaJobAddr + 2, 0x00);
    bus.write8(dmaJobAddr + 3, 0x00);   // Source = $010000
    bus.write8(dmaJobAddr + 4, 0x00);
    bus.write8(dmaJobAddr + 5, 0x01);
    bus.write8(dmaJobAddr + 6, 0x00);   // Dest = $020000
    bus.write8(dmaJobAddr + 7, 0x00);
    bus.write8(dmaJobAddr + 8, 0x02);
    bus.write8(dmaJobAddr + 9, 0x00);   // End of chain

    // Set DMA list address
    dma.ioWrite(&bus, 0xD700, 0x00);
    dma.ioWrite(&bus, 0xD701, 0x00);
    dma.ioWrite(&bus, 0xD702, 0x03);
    dma.ioWrite(&bus, 0xD704, 0x00);

    // Trigger Enhanced DMA (via $D705)
    dma.ioWrite(&bus, 0xD705, 0x01);

    // With skip rates of 2.0 (src) and 1.0 (dst):
    // Iteration 0: src[0]=0 -> dst[0]
    // Iteration 1: src[2]=2 -> dst[1]
    // Iteration 2: src[4]=4 -> dst[2]
    // Iteration 3: src[6]=6 -> dst[3]
    // Iteration 4: src[8]=8 -> dst[4]
    // Iteration 5: src[10]=10 -> dst[5]
    // Iteration 6: src[12]=12 -> dst[6]
    // Iteration 7: src[14]=14 -> dst[7]

    uint8_t val;
    bus.read8(0x020000);
    val = bus.read8(0x020000);
    ASSERT_EQ(val, 0);  // Should be 0 (from src[0])

    val = bus.read8(0x020001);
    ASSERT_EQ(val, 2);  // Should be 2 (from src[2])

    val = bus.read8(0x020002);
    ASSERT_EQ(val, 4);  // Should be 4 (from src[4])

    val = bus.read8(0x020003);
    ASSERT_EQ(val, 6);  // Should be 6 (from src[6])
}
