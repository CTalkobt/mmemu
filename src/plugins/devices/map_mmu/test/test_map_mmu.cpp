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
