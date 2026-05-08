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
    state.offsets[5] = 0x400;  // Block 5: offset = $400 → physical $40000
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
    state.offsets[2] = 0x200;  // Block 2: $4000-$5FFF → physical $20000
    state.offsets[5] = 0x400;  // Block 5: $A000-$BFFF → physical $40000
    state.offsets[6] = 0x600;  // Block 6: $C000-$DFFF → physical $60000
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
    state.offsets[5] = 0x400;
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

    for (int i = 0; i < 8; ++i) {
        uint32_t phys = (0x1000 + i) << 8;
        physBus.write8(phys, 0x10 + i);
    }

    MapState state = {};
    for (int i = 0; i < 8; ++i) {
        state.offsets[i] = 0x1000 + i;
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

    physBus.write8(0x200000, 0xBB);
    physBus.write8(0x200123, 0xCC);

    MapState state = {};
    state.offsets[0] = 0x2000;
    state.enables = (1 << 0);
    mmu.setMapState(state);

    ASSERT(mmu.read8(0x0000) == 0xBB);
    ASSERT(mmu.read8(0x0123) == 0xCC);
}

TEST_CASE(map_mmu_block_7_upper_half) {
    SparseMemoryBus physBus("phys", 28);
    MapMmu mmu("mmu", &physBus);

    physBus.write8(0x500000, 0xDD);

    MapState state = {};
    state.offsets[7] = 0x5000;
    state.enables = (1 << 7);
    mmu.setMapState(state);

    ASSERT(mmu.read8(0xE000) == 0xDD);
}

TEST_CASE(map_mmu_write_through_mapping) {
    SparseMemoryBus physBus("phys", 28);
    MapMmu mmu("mmu", &physBus);

    MapState state = {};
    state.offsets[4] = 0x300;
    state.enables = (1 << 4);
    mmu.setMapState(state);

    mmu.write8(0x8000, 0x99);
    ASSERT(physBus.read8(0x30000) == 0x99);
    ASSERT(mmu.read8(0x8000) == 0x99);
}

TEST_CASE(map_mmu_partial_mapping) {
    SparseMemoryBus physBus("phys", 28);
    MapMmu mmu("mmu", &physBus);

    physBus.write8(0x100000, 0x11);
    physBus.write8(0x8000, 0x22);

    MapState state = {};
    state.offsets[2] = 0x1000;
    state.enables = (1 << 2);
    mmu.setMapState(state);

    ASSERT(mmu.read8(0x4000) == 0x11);
    ASSERT(mmu.read8(0x8000) == 0x22);
}

TEST_CASE(map_mmu_offset_masking) {
    SparseMemoryBus physBus("phys", 28);
    MapMmu mmu("mmu", &physBus);

    uint32_t offset = 0xFFFFF;
    uint32_t phys = (offset << 8) | (0x6000 & 0x1FFF);
    physBus.write8(phys, 0xEE);

    MapState state = {};
    state.offsets[3] = 0xFFFFF;
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

    physBus.write8(0x200000, 0x01);
    physBus.write8(0x201FFF, 0x02);

    MapState state = {};
    state.offsets[0] = 0x2000;
    state.enables = (1 << 0);
    mmu.setMapState(state);

    ASSERT(mmu.read8(0x0000) == 0x01);
    ASSERT(mmu.read8(0x1FFF) == 0x02);
}

TEST_CASE(map_mmu_block_transitions) {
    SparseMemoryBus physBus("phys", 28);
    MapMmu mmu("mmu", &physBus);

    physBus.write8(0x300000, 0xA0);
    physBus.write8(0x320000, 0xA1);

    MapState state = {};
    state.offsets[1] = 0x3000;
    state.offsets[2] = 0x3200;
    state.enables = (1 << 1) | (1 << 2);
    mmu.setMapState(state);

    ASSERT(mmu.read8(0x2000) == 0xA0);
    ASSERT(mmu.read8(0x4000) == 0xA1);
}

TEST_CASE(map_mmu_peek_no_observers) {
    SparseMemoryBus physBus("phys", 28);
    MapMmu mmu("mmu", &physBus);

    physBus.write8(0x100000, 0x55);

    MapState state = {};
    state.offsets[5] = 0x1000;
    state.enables = (1 << 5);
    mmu.setMapState(state);

    uint8_t val = mmu.peek8(0xA000);
    ASSERT(val == 0x55);
}

TEST_CASE(map_mmu_mixed_enabled_disabled) {
    SparseMemoryBus physBus("phys", 28);
    MapMmu mmu("mmu", &physBus);

    physBus.write8(0x100000, 0x10);
    physBus.write8(0x004000, 0x40);

    MapState state = {};
    state.offsets[2] = 0x1000;
    state.enables = (1 << 2);
    mmu.setMapState(state);

    ASSERT(mmu.read8(0x4000) == 0x10);
    ASSERT(mmu.read8(0x6000) == 0xFF);
}

TEST_CASE(map_mmu_config_validity) {
    SparseMemoryBus physBus("phys", 28);
    MapMmu mmu("mmu", &physBus);

    const BusConfig& config = mmu.config();
    ASSERT(config.addrBits == 16);
    ASSERT(config.dataBits == 8);
    ASSERT(config.addrMask == 0xFFFF);
}
