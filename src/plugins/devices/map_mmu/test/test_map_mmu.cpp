#include "test_harness.h"
#include "plugins/devices/map_mmu/main/map_mmu.h"
#include "libmem/main/sparse_memory_bus.h"

TEST_CASE(map_mmu_basic_translation) {
    SparseMemoryBus physBus("phys", 28);
    MapMmu mmu("mmu", &physBus);

    physBus.write8(0x001000, 0x42);
    ASSERT(mmu.read8(0x001000) == 0x42);
}

TEST_CASE(map_mmu_map_enabled) {
    SparseMemoryBus physBus("phys", 28);
    MapMmu mmu("mmu", &physBus);

    physBus.write8(0x040000, 0x55);

    MapState state = {};
    // Block 5 spans virtual 0xA000-0xBFFF
    // For vaddr 0xA000 (vaddrHigh=0xA0) to map to physical 0x040000:
    // sum12bit = 0x400 = (offsetHigh12 + 0xA0) & 0xFFF
    // Therefore offsetHigh12 = 0x360, so offset bits[19:8] = 0x360
    // offset = 0x360 << 8 = 0x36000
    state.offsets[5] = 0x36000;
    state.enables = (1 << 5);
    mmu.setMapState(state);

    ASSERT(mmu.read8(0xA000) == 0x55);
}

TEST_CASE(map_mmu_map_disabled_passthrough) {
    SparseMemoryBus physBus("phys", 28);
    MapMmu mmu("mmu", &physBus);

    physBus.write8(0x008000, 0xAA);

    MapState state = {};
    state.enables = 0;  // No blocks mapped
    mmu.setMapState(state);

    ASSERT(mmu.read8(0x8000) == 0xAA);
}

TEST_CASE(map_mmu_multiple_blocks) {
    SparseMemoryBus physBus("phys", 28);
    MapMmu mmu("mmu", &physBus);

    physBus.write8(0x020000, 0x11);
    physBus.write8(0x040000, 0x22);
    physBus.write8(0x060000, 0x33);

    MapState state = {};
    // Block 2 (0x4000-0x5FFF): vaddrHigh=0x40, sum12bit=0x200 → offsetHigh12=0x1C0
    state.offsets[2] = 0x1C000;
    // Block 5 (0xA000-0xBFFF): vaddrHigh=0xA0, sum12bit=0x400 → offsetHigh12=0x360
    state.offsets[5] = 0x36000;
    // Block 6 (0xC000-0xDFFF): vaddrHigh=0xC0, sum12bit=0x600 → offsetHigh12=0x540
    state.offsets[6] = 0x54000;
    state.enables = (1 << 2) | (1 << 5) | (1 << 6);
    mmu.setMapState(state);

    ASSERT(mmu.read8(0x4000) == 0x11);
    ASSERT(mmu.read8(0xA000) == 0x22);
    ASSERT(mmu.read8(0xC000) == 0x33);
}

TEST_CASE(map_mmu_c64_mode_passthrough) {
    SparseMemoryBus physBus("phys", 28);
    MapMmu mmu("mmu", &physBus);

    physBus.write8(0x0001, 0x37);
    ASSERT(mmu.read8(0x0001) == 0x37);

    physBus.write8(0xD02F, 0x00);
    ASSERT(mmu.read8(0xD02F) == 0x00);
}

TEST_CASE(map_mmu_reset) {
    SparseMemoryBus physBus("phys", 28);
    MapMmu mmu("mmu", &physBus);

    MapState state = {};
    state.offsets[5] = 0x36000;
    state.enables = (1 << 5);
    mmu.setMapState(state);

    mmu.reset();

    ASSERT(mmu.getMapState().enables == 0);
    for (int i = 0; i < 8; ++i) {
        ASSERT(mmu.getMapState().offsets[i] == 0);
    }
}

TEST_CASE(map_mmu_all_blocks_mapped) {
    SparseMemoryBus physBus("phys", 28);
    MapMmu mmu("mmu", &physBus);

    // For hardware-accurate mapping: physical = megabyte + ((offsetHigh12 + vaddrHigh) & 0xFFF) << 8 | vaddrLow8
    // Map all 8 blocks to non-overlapping physical regions
    // Use megabyte = 0 and calculate proper offset values
    for (int i = 0; i < 8; ++i) {
        // Block i starts at virtual i*8KB (vaddr = i << 13)
        // For this test, map to physical i*0x10000 (each block gets 64KB of space)
        // vaddr >>8 for block i = (i << 13) >> 8 = i << 5 = i * 0x20
        // We want physAddr >> 8 = i * 0x80
        // sum12bit = i * 0x80, offsetHigh12 = sum12bit - i*0x20 = i*0x60
        // offset bits[19:8] = i*0x60, so offset = (i*0x60) << 8
        physBus.write8(i * 0x10000, 0x10 + i);
    }

    MapState state = {};
    for (int i = 0; i < 8; ++i) {
        // For block i at vaddr = i*8KB, sum12bit must be i*0x100 to get physAddr = i*0x10000
        // sum12bit = (offsetHigh12 + i*0x20) & 0xFFF
        // offsetHigh12 = i*0xE0, so offset = (i*0xE0) << 8
        state.offsets[i] = (i * 0xE0) << 8;
        state.enables |= (1 << i);
    }
    mmu.setMapState(state);

    for (int i = 0; i < 8; ++i) {
        uint32_t vaddr = i << 13;
        ASSERT(mmu.read8(vaddr) == 0x10 + i);
    }
}

TEST_CASE(map_mmu_block_0_lower_half) {
    SparseMemoryBus physBus("phys", 28);
    MapMmu mmu("mmu", &physBus);

    // Map block 0 (0x0000-0x1FFF) to physical 0x20000
    // vaddr = 0x0000, vaddrHigh = 0x00
    // We want physAddr = 0x20000, so sum12bit = 0x20000 >> 8 = 0x200
    // offsetHigh12 = sum12bit - vaddrHigh = 0x200 - 0x00 = 0x200
    // offset = 0x200 << 8 = 0x20000
    physBus.write8(0x020000, 0xBB);
    physBus.write8(0x020123, 0xCC);

    MapState state = {};
    state.offsets[0] = 0x20000;
    state.enables = (1 << 0);
    mmu.setMapState(state);

    ASSERT(mmu.read8(0x0000) == 0xBB);
    ASSERT(mmu.read8(0x0123) == 0xCC);
}

TEST_CASE(map_mmu_block_7_upper_half) {
    SparseMemoryBus physBus("phys", 28);
    MapMmu mmu("mmu", &physBus);

    // Map block 7 (0xE000-0xFFFF) to physical 0x50000
    // vaddr = 0xE000, vaddrHigh = 0xE0
    // We want physAddr = 0x50000, so sum12bit = 0x50000 >> 8 = 0x500
    // offsetHigh12 = sum12bit - vaddrHigh = 0x500 - 0xE0 = 0x420
    // offset = 0x420 << 8 = 0x42000
    physBus.write8(0x050000, 0xDD);

    MapState state = {};
    state.offsets[7] = 0x42000;
    state.enables = (1 << 7);
    mmu.setMapState(state);

    ASSERT(mmu.read8(0xE000) == 0xDD);
}

TEST_CASE(map_mmu_write_through_mapping) {
    SparseMemoryBus physBus("phys", 28);
    MapMmu mmu("mmu", &physBus);

    // Map block 4 (0x8000-0x9FFF) to physical 0x30000
    // vaddr = 0x8000, vaddrHigh = 0x80
    // We want physAddr = 0x30000, so sum12bit = 0x30000 >> 8 = 0x300
    // offsetHigh12 = sum12bit - vaddrHigh = 0x300 - 0x80 = 0x280
    // offset = 0x280 << 8 = 0x28000
    MapState state = {};
    state.offsets[4] = 0x28000;
    state.enables = (1 << 4);
    mmu.setMapState(state);

    mmu.write8(0x8000, 0x99);
    ASSERT(physBus.read8(0x30000) == 0x99);
    ASSERT(mmu.read8(0x8000) == 0x99);
}

TEST_CASE(map_mmu_partial_mapping) {
    SparseMemoryBus physBus("phys", 28);
    MapMmu mmu("mmu", &physBus);

    // Map block 2 (0x4000-0x5FFF) to physical 0x10000, leave other blocks unmapped
    // vaddr = 0x4000, vaddrHigh = 0x40
    // We want physAddr = 0x10000, so sum12bit = 0x10000 >> 8 = 0x100
    // offsetHigh12 = sum12bit - vaddrHigh = 0x100 - 0x40 = 0xC0
    // offset = 0xC0 << 8 = 0x0C000
    physBus.write8(0x010000, 0x11);
    physBus.write8(0x008000, 0x22);

    MapState state = {};
    state.offsets[2] = 0x0C000;
    state.enables = (1 << 2);
    mmu.setMapState(state);

    ASSERT(mmu.read8(0x4000) == 0x11);
    ASSERT(mmu.read8(0x8000) == 0x22);  // Block 4 is unmapped, reads from phys = vaddr
}

TEST_CASE(map_mmu_offset_masking) {
    SparseMemoryBus physBus("phys", 28);
    MapMmu mmu("mmu", &physBus);

    // Test maximum offset value (20-bit)
    // Block 3 (0x6000-0x7FFF): vaddrHigh = 0x60
    // offset = 0xFFFFF, offsetHigh12 = (0xFFFFF >> 8) & 0xFFF = 0xFFF
    // sum12bit = (0xFFF + 0x60) & 0xFFF = 0x05F (wraps around at 12 bits)
    // physAddr = (0x05F << 8) | 0x00 = 0x5F00
    uint32_t offset = 0xFFFFF;
    uint32_t offsetHigh12 = (offset >> 8) & 0xFFF;  // 0xFFF
    uint32_t vaddrHigh = 0x60;
    uint32_t sum12bit = (offsetHigh12 + vaddrHigh) & 0xFFF;  // 0x05F
    uint32_t phys = (sum12bit << 8) | 0x00;  // 0x5F00

    physBus.write8(phys, 0xEE);

    MapState state = {};
    state.offsets[3] = offset;
    state.enables = (1 << 3);
    mmu.setMapState(state);

    ASSERT(mmu.read8(0x6000) == 0xEE);
}

TEST_CASE(map_mmu_state_save_load) {
    SparseMemoryBus physBus("phys", 28);
    MapMmu mmu("mmu", &physBus);

    MapState original = {};
    original.offsets[1] = 0x111;
    original.offsets[3] = 0x333;
    original.offsets[5] = 0x555;
    original.enables = (1 << 1) | (1 << 3) | (1 << 5);
    mmu.setMapState(original);

    size_t size = mmu.stateSize();
    std::vector<uint8_t> buffer(size);
    mmu.saveState(buffer.data());

    MapState reset = {};
    mmu.setMapState(reset);
    ASSERT(mmu.getMapState().enables == 0);

    mmu.loadState(buffer.data());
    ASSERT(mmu.getMapState().enables == original.enables);
    ASSERT(mmu.getMapState().offsets[1] == original.offsets[1]);
    ASSERT(mmu.getMapState().offsets[3] == original.offsets[3]);
    ASSERT(mmu.getMapState().offsets[5] == original.offsets[5]);
}

TEST_CASE(map_mmu_address_boundaries) {
    SparseMemoryBus physBus("phys", 28);
    MapMmu mmu("mmu", &physBus);

    // Map block 0 (0x0000-0x1FFF) to physical 0x20000
    // vaddr = 0x0000: vaddrHigh = 0x00, physAddr = 0x20000
    // vaddr = 0x1FFF: vaddrHigh = 0x1F, physAddr should be 0x21FFF
    // sum12bit for 0x0000 = 0x20000 >> 8 = 0x200
    // offsetHigh12 = 0x200 - 0x00 = 0x200, offset = 0x200 << 8 = 0x20000
    // Verify with 0x1FFF: sum12bit = (0x200 + 0x1F) & 0xFFF = 0x21F
    // physAddr = (0x21F << 8) | 0xFF = 0x21FFF ✓
    physBus.write8(0x020000, 0x01);
    physBus.write8(0x021FFF, 0x02);

    MapState state = {};
    state.offsets[0] = 0x20000;
    state.enables = (1 << 0);
    mmu.setMapState(state);

    ASSERT(mmu.read8(0x0000) == 0x01);
    ASSERT(mmu.read8(0x1FFF) == 0x02);
}

TEST_CASE(map_mmu_block_transitions) {
    SparseMemoryBus physBus("phys", 28);
    MapMmu mmu("mmu", &physBus);

    // Test reading across block boundaries with different offsets
    // Block 1 (0x2000-0x3FFF): vaddrHigh = 0x20
    // Block 2 (0x4000-0x5FFF): vaddrHigh = 0x40
    // Both map with offset bits[19:8] = 0x2E0 to get:
    // Block 1: sum12bit = (0x2E0 + 0x20) = 0x300 → physAddr = 0x30000
    // Block 2: sum12bit = (0x2E0 + 0x40) = 0x320 → physAddr = 0x32000
    physBus.write8(0x030000, 0xA0);
    physBus.write8(0x032000, 0xA1);

    MapState state = {};
    state.offsets[1] = 0x2E000;
    state.offsets[2] = 0x2E000;
    state.enables = (1 << 1) | (1 << 2);
    mmu.setMapState(state);

    ASSERT(mmu.read8(0x2000) == 0xA0);
    ASSERT(mmu.read8(0x4000) == 0xA1);
}

TEST_CASE(map_mmu_peek_no_observers) {
    SparseMemoryBus physBus("phys", 28);
    MapMmu mmu("mmu", &physBus);

    // Block 5 (0xA000-0xBFFF): vaddrHigh = 0xA0
    // Map to physical 0x40000
    // sum12bit = 0x40000 >> 8 = 0x400
    // offsetHigh12 = 0x400 - 0xA0 = 0x360
    // offset = 0x360 << 8 = 0x36000
    physBus.write8(0x040000, 0x55);

    MapState state = {};
    state.offsets[5] = 0x36000;
    state.enables = (1 << 5);
    mmu.setMapState(state);

    uint8_t val = mmu.peek8(0xA000);
    ASSERT(val == 0x55);
}

TEST_CASE(map_mmu_mixed_enabled_disabled) {
    SparseMemoryBus physBus("phys", 28);
    MapMmu mmu("mmu", &physBus);

    // Block 2 (0x4000-0x5FFF) mapped to physical 0x10000
    // Block 3 (0x6000-0x7FFF) unmapped (passthrough)
    // vaddr = 0x4000, vaddrHigh = 0x40
    // sum12bit = 0x10000 >> 8 = 0x100
    // offsetHigh12 = 0x100 - 0x40 = 0xC0
    // offset = 0xC0 << 8 = 0x0C000
    physBus.write8(0x010000, 0x10);
    physBus.write8(0x006000, 0x40);

    MapState state = {};
    state.offsets[2] = 0x0C000;
    state.enables = (1 << 2);
    mmu.setMapState(state);

    ASSERT(mmu.read8(0x4000) == 0x10);  // Block 2: mapped
    ASSERT(mmu.read8(0x6000) == 0x40);  // Block 3: unmapped (passthrough to physical 0x6000)
}

TEST_CASE(map_mmu_config_validity) {
    SparseMemoryBus physBus("phys", 28);
    MapMmu mmu("mmu", &physBus);

    const BusConfig& config = mmu.config();
    ASSERT(config.addrBits == 16);
    ASSERT(config.dataBits == 8);
    ASSERT(config.addrMask == 0xFFFF);
}
