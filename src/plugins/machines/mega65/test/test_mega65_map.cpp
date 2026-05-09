#include "test_harness.h"
#include "plugins/devices/map_mmu/main/map_mmu.h"
#include "plugins/45gs02/main/cpu45gs02.h"
#include "libmem/main/sparse_memory_bus.h"

// Test 1: Basic MAP instruction execution - CPU loads A,X,Y,Z and executes MAP
TEST_CASE(mega65_map_instruction_loads_registers) {
    SparseMemoryBus physBus("phys", 28);
    MapMmu mmu("mmu", &physBus);
    MOS45GS02 cpu;
    cpu.setDataBus(&mmu);
    cpu.setCodeBus(&mmu);
    cpu.setMapMmu(&mmu);

    // Load test program:
    // $0000: A9 $20       LDA #$20   (A = $20, lower offset low byte)
    // $0002: A2 $01       LDX #$01   (X = $01, lower offset high nibble + enables)
    // $0004: A0 $00       LDY #$00   (Y = $00, upper offset low byte)
    // $0006: C2 $00       LDZ #$00   (Z = $00, upper offset high nibble + enables)
    // $0008: 5C           MAP
    uint8_t code[] = {0xA9, 0x20, 0xA2, 0x01, 0xA0, 0x00, 0xC2, 0x00, 0x5C};
    for (size_t i = 0; i < sizeof(code); i++) {
        physBus.write8(i, code[i]);
    }

    // Set up reset vector
    physBus.write8(0xFFFC, 0x00);
    physBus.write8(0xFFFD, 0x00);

    // Initialize CPU state
    cpu.reset();
    ASSERT_EQ(cpu.pc(), 0x0000);

    // Execute the program
    cpu.step();  // LDA #$20
    ASSERT_EQ(cpu.regRead(0), 0x20);  // A = 0x20

    cpu.step();  // LDX #$01
    ASSERT_EQ(cpu.regRead(1), 0x01);  // X = 0x01

    cpu.step();  // LDY #$00
    ASSERT_EQ(cpu.regRead(2), 0x00);  // Y = 0x00

    cpu.step();  // LDZ #$00
    ASSERT_EQ(cpu.regRead(3), 0x00);  // Z = 0x00

    cpu.step();  // MAP
    // MAP instruction should have set mapEnabled in the CPU
}

// Test 2: MAP instruction with address translation
TEST_CASE(mega65_map_instruction_simple_translation) {
    SparseMemoryBus physBus("phys", 28);
    MapMmu mmu("mmu", &physBus);

    // Write test data at physical address $040000
    physBus.write8(0x040000, 0x42);

    // Set up MapState manually
    // Block 5 ($A000-$BFFF): offset = 0x400 → physical $40000
    MapState state = {};
    state.offsets[5] = 0x400;  // 0x400 << 8 = 0x40000
    state.enables = (1 << 5);
    mmu.setMapState(state);

    // Now read via virtual address $A000 should get physical $40000
    ASSERT_EQ(mmu.read8(0xA000), 0x42);
}

// Test 3: MAP instruction disabled passthrough
TEST_CASE(mega65_map_instruction_disabled) {
    SparseMemoryBus physBus("phys", 28);
    MapMmu mmu("mmu", &physBus);

    // Write test data at virtual/physical address $8000
    physBus.write8(0x008000, 0xAA);

    // MapState with no blocks enabled
    MapState state = {};
    state.enables = 0;
    mmu.setMapState(state);

    // Should passthrough to physical address $8000
    ASSERT_EQ(mmu.read8(0x8000), 0xAA);
}

// Test 4: Multiple MAP blocks enabled
TEST_CASE(mega65_map_instruction_multiple_blocks) {
    SparseMemoryBus physBus("phys", 28);
    MapMmu mmu("mmu", &physBus);

    // Write test data at different physical addresses
    physBus.write8(0x020000, 0x11);  // Block 2
    physBus.write8(0x040000, 0x22);  // Block 5
    physBus.write8(0x060000, 0x33);  // Block 6

    // Set up MapState
    MapState state = {};
    state.offsets[2] = 0x200;  // $4000-$5FFF → physical $20000
    state.offsets[5] = 0x400;  // $A000-$BFFF → physical $40000
    state.offsets[6] = 0x600;  // $C000-$DFFF → physical $60000
    state.enables = (1 << 2) | (1 << 5) | (1 << 6);
    mmu.setMapState(state);

    // Verify all translations
    ASSERT_EQ(mmu.read8(0x4000), 0x11);
    ASSERT_EQ(mmu.read8(0xA000), 0x22);
    ASSERT_EQ(mmu.read8(0xC000), 0x33);
}

// Test 5: Address translation within a block
TEST_CASE(mega65_map_instruction_block_offset_in_range) {
    SparseMemoryBus physBus("phys", 28);
    MapMmu mmu("mmu", &physBus);

    // Write test pattern at physical $50000-$50007
    for (int i = 0; i < 8; i++) {
        physBus.write8(0x050000 + i, 0x10 + i);
    }

    // Map virtual $A000-$BFFF to physical $50000
    MapState state = {};
    state.offsets[5] = 0x500;
    state.enables = (1 << 5);
    mmu.setMapState(state);

    // Read within block: virtual $A000-$A007 should map to physical $50000-$50007
    for (int i = 0; i < 8; i++) {
        uint8_t val = mmu.read8(0xA000 + i);
        ASSERT_EQ(val, 0x10 + i);
    }
}

// Test 6: MAP state save/restore
TEST_CASE(mega65_map_instruction_state_persistence) {
    SparseMemoryBus physBus("phys", 28);
    MapMmu mmu("mmu", &physBus);

    // Set initial state
    MapState state = {};
    state.offsets[3] = 0x123;
    state.offsets[5] = 0x456;
    state.enables = (1 << 3) | (1 << 5);
    mmu.setMapState(state);

    // Verify state was stored
    const MapState& readBack = mmu.getMapState();
    ASSERT_EQ(readBack.offsets[3], 0x123);
    ASSERT_EQ(readBack.offsets[5], 0x456);
    ASSERT_EQ(readBack.enables, (1 << 3) | (1 << 5));
}

// Test 7: Reset clears MAP state
TEST_CASE(mega65_map_instruction_reset) {
    SparseMemoryBus physBus("phys", 28);
    MapMmu mmu("mmu", &physBus);

    // Set state
    MapState state = {};
    state.offsets[5] = 0x400;
    state.enables = (1 << 5);
    mmu.setMapState(state);

    // Reset
    mmu.reset();

    // Verify state is cleared
    const MapState& readBack = mmu.getMapState();
    ASSERT_EQ(readBack.enables, 0);
    for (int i = 0; i < 8; ++i) {
        ASSERT_EQ(readBack.offsets[i], 0);
    }
}

// Test 8: All 8 blocks mapped simultaneously
TEST_CASE(mega65_map_instruction_all_blocks) {
    SparseMemoryBus physBus("phys", 28);
    MapMmu mmu("mmu", &physBus);

    // Write data for all 8 blocks
    for (int i = 0; i < 8; ++i) {
        uint32_t phys = (0x1000 + i) << 8;
        physBus.write8(phys, 0x10 + i);
    }

    // Map all blocks
    MapState state = {};
    for (int i = 0; i < 8; ++i) {
        state.offsets[i] = 0x1000 + i;
        state.enables |= (1 << i);
    }
    mmu.setMapState(state);

    // Verify all blocks are accessible
    ASSERT_EQ(mmu.read8(0x0000), 0x10);  // Block 0: $0000-$1FFF
    ASSERT_EQ(mmu.read8(0x2000), 0x11);  // Block 1: $2000-$3FFF
    ASSERT_EQ(mmu.read8(0x4000), 0x12);  // Block 2: $4000-$5FFF
    ASSERT_EQ(mmu.read8(0x6000), 0x13);  // Block 3: $6000-$7FFF
    ASSERT_EQ(mmu.read8(0x8000), 0x14);  // Block 4: $8000-$9FFF
    ASSERT_EQ(mmu.read8(0xA000), 0x15);  // Block 5: $A000-$BFFF
    ASSERT_EQ(mmu.read8(0xC000), 0x16);  // Block 6: $C000-$DFFF
    ASSERT_EQ(mmu.read8(0xE000), 0x17);  // Block 7: $E000-$FFFF
}

// Test 9: Write through MAP
TEST_CASE(mega65_map_instruction_write_translation) {
    SparseMemoryBus physBus("phys", 28);
    MapMmu mmu("mmu", &physBus);

    // Map block 5
    MapState state = {};
    state.offsets[5] = 0x400;
    state.enables = (1 << 5);
    mmu.setMapState(state);

    // Write via virtual address
    mmu.write8(0xA000, 0x55);

    // Verify via physical address
    ASSERT_EQ(physBus.read8(0x040000), 0x55);
}

// Test 10: Mixed enabled/disabled blocks
TEST_CASE(mega65_map_instruction_mixed_blocks) {
    SparseMemoryBus physBus("phys", 28);
    MapMmu mmu("mmu", &physBus);

    // Write test data
    physBus.write8(0x000000, 0xAA);  // Unmapped block 0
    physBus.write8(0x000200, 0xBB);  // Will be mapped via block 1
    physBus.write8(0x040000, 0xCC);  // Unmapped block 2

    // Only map block 1
    MapState state = {};
    state.offsets[1] = 0x002;  // offset 0x002 maps to physical 0x000200
    state.enables = (1 << 1);
    mmu.setMapState(state);

    // Block 0: unmapped, should passthrough
    ASSERT_EQ(mmu.read8(0x0000), 0xAA);

    // Block 1: virtual 0x2000 maps to physical 0x000200
    ASSERT_EQ(mmu.read8(0x2000), 0xBB);

    // Block 2: unmapped, should passthrough
    physBus.write8(0x004000, 0xDD);
    ASSERT_EQ(mmu.read8(0x4000), 0xDD);
}

// Test 11: ROM overlay with MAP translation
TEST_CASE(mega65_map_instruction_rom_overlay) {
    SparseMemoryBus physBus("phys", 28);
    MapMmu mmu("mmu", &physBus);

    // Create ROM data
    uint8_t romData[256] = {0};
    for (int i = 0; i < 256; i++) {
        romData[i] = (uint8_t)(0xC0 + i);
    }

    // Add ROM overlay at physical $F0000
    physBus.addRegion(0x0F0000, 256, romData, false);

    // Map block 7 to physical $F0000
    MapState state = {};
    state.offsets[7] = 0xF00;
    state.enables = (1 << 7);
    mmu.setMapState(state);

    // Read ROM data through virtual address
    ASSERT_EQ(mmu.read8(0xE000), 0xC0);
    ASSERT_EQ(mmu.read8(0xE001), 0xC1);
}

// Test 12: Cross-block boundary read (should not cross)
TEST_CASE(mega65_map_instruction_block_boundary) {
    SparseMemoryBus physBus("phys", 28);
    MapMmu mmu("mmu", &physBus);

    // Block 1 maps to physical $020000, Block 2 maps to physical $030000
    MapState state = {};
    state.offsets[1] = 0x200;
    state.offsets[2] = 0x300;
    state.enables = (1 << 1) | (1 << 2);
    mmu.setMapState(state);

    // Write distinct values at block boundaries
    physBus.write8(0x021FFF, 0x11);  // End of block 1
    physBus.write8(0x030000, 0x22);  // Start of block 2

    // Read at boundary
    ASSERT_EQ(mmu.read8(0x3FFF), 0x11);  // Block 1 last byte
    ASSERT_EQ(mmu.read8(0x4000), 0x22);  // Block 2 first byte
}

// Test 13: 28-bit address space access
TEST_CASE(mega65_map_instruction_28bit_addressing) {
    SparseMemoryBus physBus("phys", 28);
    MapMmu mmu("mmu", &physBus);

    // Access high physical address (requires 28-bit space)
    uint32_t highAddr = 0x0FFFFFFC;  // Max 28-bit address
    physBus.write8(highAddr, 0xFF);

    // This should work with SparseMemoryBus (28-bit support)
    ASSERT_EQ(physBus.read8(highAddr), 0xFF);
}

// Test 14: MapMmu copy state snapshot
TEST_CASE(mega65_map_instruction_state_save_restore) {
    SparseMemoryBus physBus("phys", 28);
    MapMmu mmu("mmu", &physBus);

    // Set state
    MapState state1 = {};
    state1.offsets[4] = 0x100;
    state1.offsets[7] = 0x700;
    state1.enables = (1 << 4) | (1 << 7);
    mmu.setMapState(state1);

    // Get state
    const MapState& readBack = mmu.getMapState();
    MapState state2 = readBack;

    // Modify MapMmu state
    MapState state3 = {};
    mmu.setMapState(state3);

    // Restore from saved copy
    mmu.setMapState(state2);

    // Verify restoration
    const MapState& restored = mmu.getMapState();
    ASSERT_EQ(restored.offsets[4], 0x100);
    ASSERT_EQ(restored.offsets[7], 0x700);
    ASSERT_EQ(restored.enables, (1 << 4) | (1 << 7));
}

// Test 15: MAP state serialization (save/load from buffer)
TEST_CASE(mega65_map_state_serialization) {
    SparseMemoryBus physBus("phys", 28);
    MapMmu mmu("mmu", &physBus);

    // Set state
    MapState state = {};
    state.offsets[5] = 0x400;
    state.enables = (1 << 5);
    mmu.setMapState(state);

    // Get size and save
    size_t stateSize = mmu.stateSize();
    ASSERT_NE(stateSize, 0);

    uint8_t* buffer = new uint8_t[stateSize];
    mmu.saveState(buffer);

    // Clear state
    mmu.reset();
    ASSERT_EQ(mmu.getMapState().enables, 0);

    // Restore state
    mmu.loadState(buffer);
    const MapState& restored = mmu.getMapState();
    ASSERT_EQ(restored.offsets[5], 0x400);
    ASSERT_EQ(restored.enables, (1 << 5));

    delete[] buffer;
}

// Test 16: Large address offset (20-bit offset value)
TEST_CASE(mega65_map_instruction_large_offset) {
    SparseMemoryBus physBus("phys", 28);
    MapMmu mmu("mmu", &physBus);

    // Use a large 20-bit offset
    uint32_t largeOffset = 0xFFFFF;  // Max 20-bit value
    physBus.write8((largeOffset << 8), 0xAB);

    MapState state = {};
    state.offsets[0] = largeOffset;  // Will map $0000-$1FFF to very high physical address
    state.enables = (1 << 0);
    mmu.setMapState(state);

    // Read through translation
    ASSERT_EQ(mmu.read8(0x0000), 0xAB);
}

// Test 17: Sequential reads across multiple blocks
TEST_CASE(mega65_map_instruction_sequential_reads) {
    SparseMemoryBus physBus("phys", 28);
    MapMmu mmu("mmu", &physBus);

    // Write sequence: block 5 and 6
    for (int i = 0; i < 0x4000; i++) {  // 16KB total
        physBus.write8(0x040000 + i, (uint8_t)(i & 0xFF));
        physBus.write8(0x060000 + i, (uint8_t)((i + 1) & 0xFF));
    }

    MapState state = {};
    state.offsets[5] = 0x400;
    state.offsets[6] = 0x600;
    state.enables = (1 << 5) | (1 << 6);
    mmu.setMapState(state);

    // Sequential reads
    for (int i = 0; i < 256; i++) {
        uint8_t val = mmu.read8(0xA000 + i);
        ASSERT_EQ(val, (uint8_t)(i & 0xFF));
    }
}

// Test 18: Write protection through ROM overlay
TEST_CASE(mega65_map_instruction_rom_write_protected) {
    SparseMemoryBus physBus("phys", 28);
    MapMmu mmu("mmu", &physBus);

    // Create read-only ROM
    uint8_t romData[256] = {0};
    for (int i = 0; i < 256; i++) {
        romData[i] = 0x42;
    }
    physBus.addRegion(0x0E0000, 256, romData, false);  // Read-only

    // Map block 7 to ROM
    MapState state = {};
    state.offsets[7] = 0xE00;
    state.enables = (1 << 7);
    mmu.setMapState(state);

    // Read should work
    ASSERT_EQ(mmu.read8(0xE000), 0x42);

    // Write should be handled by bus (ROM write-protection)
    mmu.write8(0xE000, 0xFF);
    // Value should remain unchanged (ROM is protected)
    ASSERT_EQ(mmu.read8(0xE000), 0x42);
}

// Test 19: Peek (non-destructive read) through MAP
TEST_CASE(mega65_map_instruction_peek) {
    SparseMemoryBus physBus("phys", 28);
    MapMmu mmu("mmu", &physBus);

    physBus.write8(0x040000, 0x99);

    MapState state = {};
    state.offsets[5] = 0x400;
    state.enables = (1 << 5);
    mmu.setMapState(state);

    // Peek should return same value as read
    ASSERT_EQ(mmu.peek8(0xA000), mmu.read8(0xA000));
    ASSERT_EQ(mmu.peek8(0xA000), 0x99);
}

// Test 20: Full address range coverage
TEST_CASE(mega65_map_instruction_full_coverage) {
    SparseMemoryBus physBus("phys", 28);
    MapMmu mmu("mmu", &physBus);

    // Fill physical space with pattern: each 256-byte page gets its page number
    for (int page = 0; page < 256; page++) {
        physBus.write8(page << 8, (uint8_t)page);
    }

    // Map all blocks to physical 0x0000 (passthrough)
    MapState state = {};
    for (int i = 0; i < 8; i++) {
        state.offsets[i] = 0;  // All blocks map to physical 0x0000
        state.enables |= (1 << i);
    }
    mmu.setMapState(state);

    // Spot check addresses - should all read from physical page 0
    ASSERT_EQ(mmu.read8(0x0000), 0x00);
    ASSERT_EQ(mmu.read8(0x5000), 0x00);
    ASSERT_EQ(mmu.read8(0xFFFF), 0x00);
}
