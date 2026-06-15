#include "test_harness.h"
#include "plugins/devices/map_mmu/main/c64_bank_controller.h"
#include "plugins/devices/map_mmu/main/map_mmu.h"
#include "libmem/main/sparse_memory_bus.h"

// Helper: create ROM data filled with a pattern
static void fillRom(uint8_t* buf, uint32_t size, uint8_t base) {
    for (uint32_t i = 0; i < size; i++)
        buf[i] = base + (uint8_t)(i & 0xFF);
}

// -----------------------------------------------------------------------
// CPU I/O port tests ($00/$01)
// -----------------------------------------------------------------------

TEST_CASE(bank_ctrl_port_read_default) {
    SparseMemoryBus bus("phys", 28);
    C64BankController bc(&bus);
    bc.reset();

    uint8_t val = 0;
    // $00 = DDR defaults to $2F
    ASSERT(bc.ioRead(nullptr, 0x0000, &val));
    ASSERT_EQ(val, 0x2F);

    // $01 = effective port: output | ~ddr = 0x37 | ~0x2F = 0x37 | 0xD0 = 0xF7
    ASSERT(bc.ioRead(nullptr, 0x0001, &val));
    ASSERT_EQ(val, 0xF7);
}

TEST_CASE(bank_ctrl_port_write_read) {
    SparseMemoryBus bus("phys", 28);
    C64BankController bc(&bus);
    bc.reset();

    // Write DDR = $3F (all bits 0-5 output)
    ASSERT(bc.ioWrite(nullptr, 0x0000, 0x3F));
    // Write port = $30 (LORAM=0, HIRAM=0, CHAREN=0, bits 4-5 = 1)
    ASSERT(bc.ioWrite(nullptr, 0x0001, 0x30));

    uint8_t val = 0;
    ASSERT(bc.ioRead(nullptr, 0x0001, &val));
    // effective = 0x30 | ~0x3F = 0x30 | 0xC0 = 0xF0
    ASSERT_EQ(val, 0xF0);
}

TEST_CASE(bank_ctrl_ignores_other_addrs) {
    SparseMemoryBus bus("phys", 28);
    C64BankController bc(&bus);
    bc.reset();

    uint8_t val = 0;
    ASSERT(!bc.ioRead(nullptr, 0x0002, &val));
    ASSERT(!bc.ioRead(nullptr, 0x1000, &val));
    ASSERT(!bc.ioWrite(nullptr, 0x0002, 0x00));
}

// -----------------------------------------------------------------------
// KERNAL ROM overlay ($E000-$FFFF)
// -----------------------------------------------------------------------

TEST_CASE(bank_ctrl_kernal_visible_default) {
    SparseMemoryBus bus("phys", 28);
    C64BankController bc(&bus);

    uint8_t kernal[8192];
    fillRom(kernal, 8192, 0xE0);
    bc.setKernalRom(kernal, 8192);
    bc.reset();  // HIRAM=1 by default

    // KERNAL visible via ioRead (not bus overlay — banking is dynamic)
    uint8_t val = 0;
    ASSERT(bc.ioRead(nullptr, 0xE000, &val));
    ASSERT_EQ(val, 0xE0);
    ASSERT(bc.ioRead(nullptr, 0xE001, &val));
    ASSERT_EQ(val, 0xE1);
}

TEST_CASE(bank_ctrl_kernal_hidden_hiram0) {
    SparseMemoryBus bus("phys", 28);
    C64BankController bc(&bus);

    // Write RAM before overlay is installed
    bus.write8(0x00E000, 0xAA);

    uint8_t kernal[8192];
    fillRom(kernal, 8192, 0xE0);
    bc.setKernalRom(kernal, 8192);
    bc.reset();

    // Overlay should be active, hiding RAM
    ASSERT_EQ(bus.read8(0x00E000), 0xE0);

    // Set HIRAM=0: port = 0x35 (LORAM=1, HIRAM=0, CHAREN=1)
    bc.ioWrite(nullptr, 0x0001, 0x35);

    // KERNAL overlay should be removed; RAM visible
    ASSERT_EQ(bus.read8(0x00E000), 0xAA);
}

// -----------------------------------------------------------------------
// BASIC ROM overlay ($A000-$BFFF)
// -----------------------------------------------------------------------

TEST_CASE(bank_ctrl_basic_visible_default) {
    SparseMemoryBus bus("phys", 28);
    C64BankController bc(&bus);

    uint8_t basic[8192];
    fillRom(basic, 8192, 0xA0);
    bc.setBasicRom(basic, 8192);
    bc.reset();  // HIRAM=1, LORAM=1

    ASSERT_EQ(bus.read8(0x00A000), 0xA0);
    ASSERT_EQ(bus.read8(0x00A001), 0xA1);
}

TEST_CASE(bank_ctrl_basic_hidden_loram0) {
    SparseMemoryBus bus("phys", 28);
    C64BankController bc(&bus);

    // Write RAM before overlay
    bus.write8(0x00A000, 0xBB);

    uint8_t basic[8192];
    fillRom(basic, 8192, 0xA0);
    bc.setBasicRom(basic, 8192);
    bc.reset();

    // Overlay active
    ASSERT_EQ(bus.read8(0x00A000), 0xA0);

    // Set LORAM=0: port = 0x36 (LORAM=0, HIRAM=1, CHAREN=1)
    bc.ioWrite(nullptr, 0x0001, 0x36);

    ASSERT_EQ(bus.read8(0x00A000), 0xBB);
}

// -----------------------------------------------------------------------
// Character ROM ($D000-$DFFF via ioRead)
// -----------------------------------------------------------------------

TEST_CASE(bank_ctrl_charrom_visible_charen0) {
    SparseMemoryBus bus("phys", 28);
    C64BankController bc(&bus);

    uint8_t charRom[4096];
    fillRom(charRom, 4096, 0xD0);
    bc.setCharRom(charRom, 4096);
    bc.reset();

    // Set CHAREN=0: port = 0x33 (LORAM=1, HIRAM=1, CHAREN=0)
    bc.ioWrite(nullptr, 0x0001, 0x33);

    uint8_t val = 0;
    ASSERT(bc.ioRead(nullptr, 0xD000, &val));
    ASSERT_EQ(val, 0xD0);
    ASSERT(bc.ioRead(nullptr, 0xD001, &val));
    ASSERT_EQ(val, 0xD1);
}

TEST_CASE(bank_ctrl_charrom_hidden_charen1) {
    SparseMemoryBus bus("phys", 28);
    C64BankController bc(&bus);

    uint8_t charRom[4096];
    fillRom(charRom, 4096, 0xD0);
    bc.setCharRom(charRom, 4096);
    bc.reset();  // CHAREN=1 by default

    uint8_t val = 0;
    // Should NOT claim $D000 when CHAREN=1 (I/O handlers take over)
    ASSERT(!bc.ioRead(nullptr, 0xD000, &val));
}

// -----------------------------------------------------------------------
// MAP interaction: overlays disabled when blocks are MAP'd
// -----------------------------------------------------------------------

TEST_CASE(bank_ctrl_kernal_bypassed_when_mapped) {
    SparseMemoryBus bus("phys", 28);
    MapMmu mmu("mmu", &bus);
    C64BankController bc(&bus);
    bc.setMapMmu(&mmu);

    uint8_t kernal[8192];
    fillRom(kernal, 8192, 0xE0);
    bc.setKernalRom(kernal, 8192);
    bc.reset();

    // KERNAL should be visible initially
    ASSERT_EQ(bus.read8(0x00E000), 0xE0);

    // Enable MAP for block 7 ($E000-$FFFF)
    MapState state = {};
    state.offsets[7] = 0x500;
    state.enables = (1 << 7);
    mmu.setMapState(state);

    // Trigger overlay update (as if port was written)
    bc.ioWrite(nullptr, 0x0001, 0x37);

    // KERNAL overlay should be removed (MAP takes priority)
    // The raw bus at $E000 should now show RAM, not ROM
    ASSERT_NE(bus.read8(0x00E000), 0xE0);
}

TEST_CASE(bank_ctrl_basic_bypassed_when_mapped) {
    SparseMemoryBus bus("phys", 28);
    MapMmu mmu("mmu", &bus);
    C64BankController bc(&bus);
    bc.setMapMmu(&mmu);

    uint8_t basic[8192];
    fillRom(basic, 8192, 0xA0);
    bc.setBasicRom(basic, 8192);
    bc.reset();

    ASSERT_EQ(bus.read8(0x00A000), 0xA0);

    // Enable MAP for block 5 ($A000-$BFFF)
    MapState state = {};
    state.offsets[5] = 0x300;
    state.enables = (1 << 5);
    mmu.setMapState(state);

    bc.ioWrite(nullptr, 0x0001, 0x37);

    ASSERT_NE(bus.read8(0x00A000), 0xA0);
}

TEST_CASE(bank_ctrl_charrom_bypassed_when_mapped) {
    SparseMemoryBus bus("phys", 28);
    MapMmu mmu("mmu", &bus);
    C64BankController bc(&bus);
    bc.setMapMmu(&mmu);

    uint8_t charRom[4096];
    fillRom(charRom, 4096, 0xD0);
    bc.setCharRom(charRom, 4096);
    bc.reset();

    // Set CHAREN=0
    bc.ioWrite(nullptr, 0x0001, 0x33);

    // Char ROM should be visible via ioRead
    uint8_t val = 0;
    ASSERT(bc.ioRead(nullptr, 0xD000, &val));
    ASSERT_EQ(val, 0xD0);

    // Enable MAP for block 6 ($C000-$DFFF)
    MapState state = {};
    state.offsets[6] = 0x400;
    state.enables = (1 << 6);
    mmu.setMapState(state);

    // Char ROM should NOT be served when block is MAP'd
    ASSERT(!bc.ioRead(nullptr, 0xD000, &val));
}

// -----------------------------------------------------------------------
// Reset behavior
// -----------------------------------------------------------------------

TEST_CASE(bank_ctrl_reset_restores_defaults) {
    SparseMemoryBus bus("phys", 28);
    C64BankController bc(&bus);

    // Write RAM before overlay
    bus.write8(0x00E000, 0xCC);

    uint8_t kernal[8192];
    fillRom(kernal, 8192, 0xE0);
    bc.setKernalRom(kernal, 8192);
    bc.reset();

    // KERNAL visible
    ASSERT_EQ(bus.read8(0x00E000), 0xE0);

    // Bank out KERNAL
    bc.ioWrite(nullptr, 0x0001, 0x35);
    ASSERT_EQ(bus.read8(0x00E000), 0xCC);

    // Reset should restore defaults (HIRAM=1, KERNAL visible)
    bc.reset();
    ASSERT_EQ(bus.read8(0x00E000), 0xE0);
}
