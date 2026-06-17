#include "test_harness.h"
#include "../main/f018b_dma.h"
#include "libmem/main/ibus.h"
#include <cstring>
#include <iomanip>
#include <iostream>
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

    // Set up a fill job at $030000 (cmd=0x03 for fill)
    writeJobF018(bus, 0x030000, 0x03, 8, 0x0000AA, 0x020000);
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

    writeJobF018(bus, 0x030000, 0x03, 8, 0x0000AA, 0x020000);
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

    // Fill mode (cmd=0x03): source low byte = fill value
    writeJobF018(bus, 0x030000, 0x03, 32, 0x000055, 0x020000);

    triggerDma(dma, bus, 0x030000);

    ASSERT(bus.verifyRegion(0x020000, 0x55, 32));
}

// ============================================================================
// Test: DMA Swap Operation
// ============================================================================

TEST_CASE(dma_swap_aborts) {
    // Swap (cmd=0x02) is unimplemented on real hardware; DMA should abort
    F018bDmaDevice dma(0xD700);
    MockMemoryBus bus;

    bus.fillRegion(0x010000, 0xAA, 8);
    bus.fillRegion(0x020000, 0xBB, 8);

    writeJobF018(bus, 0x030000, 0x02, 8, 0x010000, 0x020000);

    triggerDma(dma, bus, 0x030000);

    // Both regions should be unchanged — swap aborted
    ASSERT(bus.verifyRegion(0x010000, 0xAA, 8));
    ASSERT(bus.verifyRegion(0x020000, 0xBB, 8));
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
// Test: Fill-by-copy trick — overlapping forward copy repeats pattern (#57)
// ============================================================================

TEST_CASE(dma_fill_by_copy_trick) {
    F018bDmaDevice dma(0xD700);
    MockMemoryBus bus;

    // Write a single byte at $010000
    bus.write8(0x010000, 0x42);
    // Rest is zeros
    bus.fillRegion(0x010001, 0x00, 15);

    // Copy 16 bytes from $010000 to $010001 (overlapping forward, no direction bits)
    // With auto-reverse this would copy backward, preserving zeros.
    // Without auto-reverse (correct), each byte reads the previously-written value,
    // producing: $42 $42 $42 $42 ... (fill-by-copy pattern)
    writeJobF018(bus, 0x030000, 0x00, 16, 0x010000, 0x010001);

    triggerDma(dma, bus, 0x030000);

    // All 17 bytes ($010000-$010010) should now be $42
    for (int i = 0; i < 17; ++i) {
        ASSERT_EQ(bus.read8(0x010000 + i), 0x42);
    }
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
    // Fill cmd=0x03, chain bit=0x04 → 0x07
    writeJobF018B(bus, 0x030000, 0x07, 8, 0x0000AA, 0x020000);

    // Job 2 at $03000C (offset 12): fill 8 bytes at $020100 with $BB
    writeJobF018B(bus, 0x03000C, 0x03, 8, 0x0000BB, 0x020100);

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


// ============================================================================
// Test: DMA list address register combination
// ============================================================================

TEST_CASE(dma_check_list_address_register_combination) {
    F018bDmaDevice dma(0xD700);
    MockMemoryBus bus;

    uint32_t test_addr = 0x12345678;
    const uint32_t EXPECTED_ADDR = 0;
    
    dma.ioWrite(&bus, 0xD702, (test_addr >> 16) & 0xFF);
    dma.ioWrite(&bus, 0xD704, (test_addr >> 24) & 0xFF);
    dma.ioWrite(&bus, 0xD701, (test_addr >> 8) & 0xFF);
    // Don't write $D700 since this isn't testing if it actually triggers yet

    uint32_t dma_list_addr = 0x00000000;
    
    const auto get_dma_list_addr = [&dma, &bus]() -> uint32_t {
	uint8_t read_value = 0;
	uint32_t list_addr = 0x00000000;
	dma.ioRead(&bus, 0xD700, &read_value);
	list_addr |= read_value;
	dma.ioRead(&bus, 0xD701, &read_value);
	list_addr |= read_value << 8;
	dma.ioRead(&bus, 0xD702, &read_value);
        list_addr |= read_value << 16;
	dma.ioRead(&bus, 0xD704, &read_value);
	list_addr |= read_value << 24;
	return list_addr;
    };

    dma_list_addr = get_dma_list_addr();
    std::cerr << std::hex << std::setw(8) << dma_list_addr << '\n';
    dma.ioWrite(&bus, 0xD705, 0x00);

    dma.ioWrite(&bus, 0xD702, 0xAA);
    dma_list_addr = get_dma_list_addr();
    std::cerr << std::hex << std::setw(8) << dma_list_addr << '\n';
    dma.ioWrite(&bus, 0xD705, 0x00);
    
    dma.ioWrite(&bus, 0xD704, 0x55);
    dma_list_addr = get_dma_list_addr();
    std::cerr << std::hex << std::setw(8) << dma_list_addr << '\n';
    dma.ioWrite(&bus, 0xD705, 0x00);
    dma.ioWrite(&bus, 0xD700, 0x00);
}

// ============================================================================
// Test: List address advances after DMA job (#54)
// ============================================================================

TEST_CASE(dma_list_addr_advances_f018) {
    F018bDmaDevice dma(0xD700);
    MockMemoryBus bus;

    // F018 11-byte copy job at $030000
    bus.fillRegion(0x010000, 0xAA, 4);
    writeJobF018(bus, 0x030000, 0x00, 4, 0x010000, 0x020000);

    triggerDma(dma, bus, 0x030000);

    // After DMA, list address should point past the 11-byte job
    uint8_t val;
    uint32_t readAddr = 0;
    dma.ioRead(&bus, 0xD700, &val); readAddr |= val;
    dma.ioRead(&bus, 0xD701, &val); readAddr |= (uint32_t)val << 8;
    dma.ioRead(&bus, 0xD702, &val); readAddr |= (uint32_t)(val & 0x7F) << 16;
    dma.ioRead(&bus, 0xD704, &val); readAddr |= (uint32_t)val << 20;

    // List started at $030000, F018 job is 11 bytes → should be $03000B
    ASSERT_EQ(readAddr, 0x03000B);
}

TEST_CASE(dma_list_addr_advances_f018b) {
    F018bDmaDevice dma(0xD700);
    MockMemoryBus bus;

    // F018B 12-byte copy job at $030000
    bus.fillRegion(0x010000, 0xBB, 4);
    writeJobF018B(bus, 0x030000, 0x00, 4, 0x010000, 0x020000);

    dma.ioWrite(&bus, 0xD703, 0x01); // EN018B=1
    triggerDma(dma, bus, 0x030000);

    uint8_t val;
    uint32_t readAddr = 0;
    dma.ioRead(&bus, 0xD700, &val); readAddr |= val;
    dma.ioRead(&bus, 0xD701, &val); readAddr |= (uint32_t)val << 8;
    dma.ioRead(&bus, 0xD702, &val); readAddr |= (uint32_t)(val & 0x7F) << 16;
    dma.ioRead(&bus, 0xD704, &val); readAddr |= (uint32_t)val << 20;

    // List started at $030000, F018B job is 12 bytes → should be $03000C
    ASSERT_EQ(readAddr, 0x03000C);
}

TEST_CASE(dma_list_addr_advances_chained) {
    F018bDmaDevice dma(0xD700);
    MockMemoryBus bus;

    bus.fillRegion(0x010000, 0x11, 4);
    bus.fillRegion(0x010100, 0x22, 4);

    // Job 1: copy with chain (11 bytes)
    writeJobF018(bus, 0x030000, 0x04, 4, 0x010000, 0x020000);
    // Job 2: copy without chain (starts at offset 11)
    writeJobF018(bus, 0x03000B, 0x00, 4, 0x010100, 0x040000);

    triggerDma(dma, bus, 0x030000);

    uint8_t val;
    uint32_t readAddr = 0;
    dma.ioRead(&bus, 0xD700, &val); readAddr |= val;
    dma.ioRead(&bus, 0xD701, &val); readAddr |= (uint32_t)val << 8;
    dma.ioRead(&bus, 0xD702, &val); readAddr |= (uint32_t)(val & 0x7F) << 16;
    dma.ioRead(&bus, 0xD704, &val); readAddr |= (uint32_t)val << 20;

    // Two F018 jobs: 11 + 11 = 22 bytes → should be $030016
    ASSERT_EQ(readAddr, 0x030016);
}

// ============================================================================
// Test: Self-modifying DMA chain — job 1 modifies job 2's list data (#55)
// ============================================================================

TEST_CASE(dma_chain_self_modify) {
    F018bDmaDevice dma(0xD700);
    MockMemoryBus bus;

    // Job 1 at $030000: copy 1 byte from $040000 to $03000E (job 2's fill value)
    // Job 2 at $03000B: fill 4 bytes at $020000 with whatever fill byte is set
    //
    // Job 2's source LSB (fill byte) is at $03000E (offset 3 in job 2).
    // Initially set to $00, but job 1 copies $42 there before job 2 executes.

    bus.write8(0x040000, 0x42);  // Source data for job 1

    // Job 1: copy 1 byte from $040000 to $03000E, with chain bit
    writeJobF018(bus, 0x030000, 0x04, 1, 0x040000, 0x03000E);

    // Job 2: fill 4 bytes at $020000. Fill byte = src LSB = initially $00.
    writeJobF018(bus, 0x03000B, 0x03, 4, 0x000000, 0x020000);

    // Clear destination
    bus.fillRegion(0x020000, 0xFF, 4);

    triggerDma(dma, bus, 0x030000);

    // With read-all-first: job 2 would have been parsed before job 1 ran,
    // so fill byte = $00. With read→execute→chain: job 1 writes $42 to
    // $03000E first, then job 2 reads it, so fill byte = $42.
    ASSERT(bus.verifyRegion(0x020000, 0x42, 4));
}

// ============================================================================
// Test: F018B bank byte bits 6:4 add to megabyte (#59)
// ============================================================================

TEST_CASE(dma_f018b_bank_bits_add_to_mb) {
    F018bDmaDevice dma(0xD700);
    MockMemoryBus bus;

    // Source at physical $0120_0000 (MB=1, bank nibble=2, addr=$0000)
    // In F018B, bank byte bits 6:4 are ADDED to the megabyte register.
    // With MB option $80=$01 and bank byte bits 6:4 = 0, bank 3:0 = 2:
    //   addr(27:20) = $01 + 0 = $01, addr(19:16) = 2 → physical $0120000
    // But with bank byte = $22 (bits 6:4 = 2, bits 3:0 = 2):
    //   addr(27:20) = $01 + 2 = $03, addr(19:16) = 2 → physical $0320000

    bus.fillRegion(0x0100000, 0xAA, 4);  // MB=1, bank=0 → $0100000
    bus.fillRegion(0x0300000, 0xBB, 4);  // MB=3, bank=0 → $0300000 (MB=1 + bank bits 2)

    // Enhanced DMA job with option $80 $01 (src MB=1), $81 $00 (dst MB=0)
    uint32_t ja = 0x030000;
    bus.write8(ja++, 0x80); bus.write8(ja++, 0x01);  // src MB = 1
    bus.write8(ja++, 0x81); bus.write8(ja++, 0x00);  // dst MB = 0
    bus.write8(ja++, 0x00);                           // end of options

    // F018B 12-byte job: copy 4 bytes
    // src: lo=0, mid=0, bank_flags=$20 (bits 6:4=2, bits 3:0=0)
    //   → MB = 1+2 = 3, bank = 0 → physical $0300000
    // dst: lo=0, mid=0, bank_flags=$02 (bits 6:4=0, bits 3:0=2)
    //   → MB = 0+0 = 0, bank = 2 → physical $0020000
    bus.write8(ja++, 0x00);  // cmd: copy
    bus.write8(ja++, 0x04);  // count lo
    bus.write8(ja++, 0x00);  // count hi
    bus.write8(ja++, 0x00);  // src lo
    bus.write8(ja++, 0x00);  // src mid
    bus.write8(ja++, 0x20);  // src bank: bits 6:4=2, bits 3:0=0
    bus.write8(ja++, 0x00);  // dst lo
    bus.write8(ja++, 0x00);  // dst mid
    bus.write8(ja++, 0x02);  // dst bank: bits 6:4=0, bits 3:0=2
    bus.write8(ja++, 0x00);  // cmd MSB
    bus.write8(ja++, 0x00);  // modulo lo
    bus.write8(ja++, 0x00);  // modulo hi

    // Clear destination
    bus.fillRegion(0x020000, 0x00, 4);

    dma.ioWrite(&bus, 0xD703, 0x01);  // EN018B=1
    triggerEnhancedDma(dma, bus, 0x030000);

    // src address = MB(1+2=3):bank(0):$0000 = $0300000
    // dst address = MB(0+0=0):bank(2):$0000 = $0020000
    ASSERT(bus.verifyRegion(0x020000, 0xBB, 4));
}

// ============================================================================
// Test: F018A bank byte bits 6:4 are flags, not address (#59)
// ============================================================================

TEST_CASE(dma_f018a_bank_bits_are_flags) {
    F018bDmaDevice dma(0xD700);
    MockMemoryBus bus;

    // In F018A mode, bank byte bits 6:4 are direction/modulo/hold flags.
    // They should NOT be added to the megabyte address.
    // Source at bank 1 ($010000) with direction bit set (bit 6 = 1).
    // Bank byte = $41 (bit 6=1 direction, bits 3:0=1)

    bus.fillRegion(0x010000, 0xCC, 4);

    uint32_t ja = 0x030000;
    // F018 11-byte job (EN018B=0): copy 4 bytes
    bus.write8(ja + 0, 0x00);  // cmd: copy
    bus.write8(ja + 1, 0x04);  // count lo
    bus.write8(ja + 2, 0x00);  // count hi
    bus.write8(ja + 3, 0x00);  // src lo
    bus.write8(ja + 4, 0x00);  // src mid
    bus.write8(ja + 5, 0x41);  // src bank: bit 6=1(dir), bits 3:0=1 (bank 1)
    bus.write8(ja + 6, 0x00);  // dst lo
    bus.write8(ja + 7, 0x00);  // dst mid
    bus.write8(ja + 8, 0x02);  // dst bank: bits 3:0=2
    bus.write8(ja + 9, 0x00);  // modulo lo
    bus.write8(ja + 10, 0x00); // modulo hi

    bus.fillRegion(0x020000, 0x00, 4);

    // EN018B=0 (F018A mode, default)
    triggerDma(dma, bus, 0x030000);

    // Source: MB=0, bank=1 → $010000 (NOT $010000 + bit6 added to MB)
    // But direction=1 means backward: reads $010000, $00FFFF, $00FFFE, $00FFFD
    // First byte should still be $CC from $010000
    ASSERT_EQ(bus.read8(0x020000), 0xCC);
}

// ============================================================================
// Test: Count of 0 means 65536 bytes (#60 item 1)
// ============================================================================

TEST_CASE(dma_count_zero_means_64k) {
    F018bDmaDevice dma(0xD700);
    MockMemoryBus bus;

    // Fill source with pattern
    for (int i = 0; i < 256; ++i)
        bus.write8(0x010000 + i, (uint8_t)i);

    // F018 fill job with count=0 (should mean 65536)
    writeJobF018(bus, 0x030000, 0x03, 0, 0x0000AA, 0x020000);

    // Clear first few bytes of destination to verify they get filled
    bus.fillRegion(0x020000, 0x00, 256);

    triggerDma(dma, bus, 0x030000);

    // Verify fill happened (0xAA across the range)
    ASSERT_EQ(bus.read8(0x020000), 0xAA);
    ASSERT_EQ(bus.read8(0x02FFFF), 0xAA);
    // Byte just past 64KB should NOT be filled
    ASSERT_EQ(bus.read8(0x030000 + 11), bus.read8(0x030000 + 11));  // unchanged (list data)
}

// ============================================================================
// Test: Source hold flag — address doesn't advance (#60 item 4)
// ============================================================================

TEST_CASE(dma_source_hold) {
    F018bDmaDevice dma(0xD700);
    MockMemoryBus bus;

    // Write different values at sequential source addresses
    bus.write8(0x010000, 0x42);
    bus.write8(0x010001, 0x43);
    bus.write8(0x010002, 0x44);
    bus.write8(0x010003, 0x45);

    // F018B copy with source hold: Command MSB bits 0-1 = src addressing mode
    // %10 = hold → Command MSB = 0x02
    uint32_t ja = 0x030000;
    bus.write8(ja + 0, 0x00);  // cmd: copy
    bus.write8(ja + 1, 0x04);  // count=4
    bus.write8(ja + 2, 0x00);
    bus.write8(ja + 3, 0x00);  // src lo
    bus.write8(ja + 4, 0x00);  // src mid
    bus.write8(ja + 5, 0x01);  // src bank=1
    bus.write8(ja + 6, 0x00);  // dst lo
    bus.write8(ja + 7, 0x00);  // dst mid
    bus.write8(ja + 8, 0x02);  // dst bank=2
    bus.write8(ja + 9, 0x02);  // Command MSB: src mode=%10 (hold)
    bus.write8(ja + 10, 0x00); // modulo lo
    bus.write8(ja + 11, 0x00); // modulo hi

    bus.fillRegion(0x020000, 0x00, 4);

    dma.ioWrite(&bus, 0xD703, 0x01);  // EN018B=1
    triggerDma(dma, bus, 0x030000);

    // With src_hold, all 4 bytes should read from the same address ($010000 = 0x42)
    ASSERT_EQ(bus.read8(0x020000), 0x42);
    ASSERT_EQ(bus.read8(0x020001), 0x42);
    ASSERT_EQ(bus.read8(0x020002), 0x42);
    ASSERT_EQ(bus.read8(0x020003), 0x42);
}

// ============================================================================
// Test: Dest hold flag — writes to same address (#60 item 4)
// ============================================================================

TEST_CASE(dma_dest_hold) {
    F018bDmaDevice dma(0xD700);
    MockMemoryBus bus;

    // Source: sequential values
    bus.write8(0x010000, 0x10);
    bus.write8(0x010001, 0x20);
    bus.write8(0x010002, 0x30);
    bus.write8(0x010003, 0x40);

    // F018B copy with dest hold: Command MSB bits 2-3 = dst addressing mode
    // %10 = hold → bits 2-3 = %10 → Command MSB = 0x08
    uint32_t ja = 0x030000;
    bus.write8(ja + 0, 0x00);  // copy
    bus.write8(ja + 1, 0x04);  // count=4
    bus.write8(ja + 2, 0x00);
    bus.write8(ja + 3, 0x00);  // src lo
    bus.write8(ja + 4, 0x00);  // src mid
    bus.write8(ja + 5, 0x01);  // src bank=1
    bus.write8(ja + 6, 0x00);  // dst lo
    bus.write8(ja + 7, 0x00);  // dst mid
    bus.write8(ja + 8, 0x02);  // dst bank=2
    bus.write8(ja + 9, 0x08);  // Command MSB: dst mode=%10 (hold)
    bus.write8(ja + 10, 0x00);
    bus.write8(ja + 11, 0x00);

    bus.fillRegion(0x020000, 0x00, 4);

    dma.ioWrite(&bus, 0xD703, 0x01);
    triggerDma(dma, bus, 0x030000);

    // With dst_hold, all writes go to $020000.
    // Last written value (from $010003 = 0x40) should be at $020000.
    ASSERT_EQ(bus.read8(0x020000), 0x40);
    // Adjacent bytes should be untouched
    ASSERT_EQ(bus.read8(0x020001), 0x00);
    ASSERT_EQ(bus.read8(0x020002), 0x00);
}
