#include "test_harness.h"
#include "plugins/devices/map_mmu/main/c64_bank_controller.h"
#include "plugins/devices/map_mmu/main/map_mmu.h"
#include "libmem/main/sparse_memory_bus.h"

// Helper: create ROM data filled with a pattern
static void fillRom(uint8_t* buf, uint32_t size, uint8_t base) {
    for (uint32_t i = 0; i < size; i++)
        buf[i] = base + (uint8_t)(i & 0xFF);
}

// Helper: read through bank controller, return value or 0xFF if not intercepted
static uint8_t bcRead(C64BankController& bc, uint32_t addr) {
    uint8_t val = 0xFF;
    bc.ioRead(nullptr, addr, &val);
    return val;
}

// Helper: check if bank controller intercepts a read
static bool bcClaims(C64BankController& bc, uint32_t addr) {
    uint8_t val = 0;
    return bc.ioRead(nullptr, addr, &val);
}

// -----------------------------------------------------------------------
// CPU I/O port tests ($00/$01)
// -----------------------------------------------------------------------

TEST_CASE(bank_ctrl_port_read_default) {
    SparseMemoryBus bus("phys", 28);
    C64BankController bc(&bus);
    bc.reset();

    uint8_t val = 0;
    ASSERT(bc.ioRead(nullptr, 0x0000, &val));
    ASSERT_EQ(val, 0x2F);

    ASSERT(bc.ioRead(nullptr, 0x0001, &val));
    ASSERT_EQ(val, 0xF7);
}

TEST_CASE(bank_ctrl_port_write_read) {
    SparseMemoryBus bus("phys", 28);
    C64BankController bc(&bus);
    bc.reset();

    ASSERT(bc.ioWrite(nullptr, 0x0000, 0x3F));
    ASSERT(bc.ioWrite(nullptr, 0x0001, 0x30));

    uint8_t val = 0;
    ASSERT(bc.ioRead(nullptr, 0x0001, &val));
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
// KERNAL ROM ($E000-$FFFF) via ioRead
// -----------------------------------------------------------------------

TEST_CASE(bank_ctrl_kernal_visible_default) {
    SparseMemoryBus bus("phys", 28);
    C64BankController bc(&bus);

    uint8_t kernal[8192];
    fillRom(kernal, 8192, 0xE0);
    bc.setKernalRom(kernal, 8192);
    bc.reset();  // HIRAM=1 by default

    ASSERT_EQ(bcRead(bc, 0xE000), 0xE0);
    ASSERT_EQ(bcRead(bc, 0xE001), 0xE1);
}

TEST_CASE(bank_ctrl_kernal_hidden_hiram0) {
    SparseMemoryBus bus("phys", 28);
    C64BankController bc(&bus);

    uint8_t kernal[8192];
    fillRom(kernal, 8192, 0xE0);
    bc.setKernalRom(kernal, 8192);
    bc.reset();

    // KERNAL visible
    ASSERT(bcClaims(bc, 0xE000));
    ASSERT_EQ(bcRead(bc, 0xE000), 0xE0);

    // Set HIRAM=0
    bc.ioWrite(nullptr, 0x0001, 0x35);

    // KERNAL should NOT be intercepted (falls through to RAM)
    ASSERT(!bcClaims(bc, 0xE000));
}

// -----------------------------------------------------------------------
// BASIC ROM ($A000-$BFFF) via ioRead
// -----------------------------------------------------------------------

TEST_CASE(bank_ctrl_basic_visible_default) {
    SparseMemoryBus bus("phys", 28);
    C64BankController bc(&bus);

    uint8_t basic[8192];
    fillRom(basic, 8192, 0xA0);
    bc.setBasicRom(basic, 8192);
    bc.reset();

    ASSERT_EQ(bcRead(bc, 0xA000), 0xA0);
    ASSERT_EQ(bcRead(bc, 0xA001), 0xA1);
}

TEST_CASE(bank_ctrl_basic_hidden_loram0) {
    SparseMemoryBus bus("phys", 28);
    C64BankController bc(&bus);

    uint8_t basic[8192];
    fillRom(basic, 8192, 0xA0);
    bc.setBasicRom(basic, 8192);
    bc.reset();

    ASSERT(bcClaims(bc, 0xA000));

    // Set LORAM=0
    bc.ioWrite(nullptr, 0x0001, 0x36);

    ASSERT(!bcClaims(bc, 0xA000));
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

    // Set CHAREN=0
    bc.ioWrite(nullptr, 0x0001, 0x33);

    ASSERT_EQ(bcRead(bc, 0xD000), 0xD0);
    ASSERT_EQ(bcRead(bc, 0xD001), 0xD1);
}

TEST_CASE(bank_ctrl_charrom_hidden_charen1) {
    SparseMemoryBus bus("phys", 28);
    C64BankController bc(&bus);

    uint8_t charRom[4096];
    fillRom(charRom, 4096, 0xD0);
    bc.setCharRom(charRom, 4096);
    bc.reset();  // CHAREN=1 by default

    // Should NOT claim $D000 when CHAREN=1
    ASSERT(!bcClaims(bc, 0xD000));
}

// -----------------------------------------------------------------------
// MAP interaction: banking disabled when blocks are MAP'd
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

    // KERNAL visible initially
    ASSERT(bcClaims(bc, 0xE000));

    // Enable MAP for block 7 ($E000-$FFFF)
    MapState state = {};
    state.offsets[7] = 0x500;
    state.enables = (1 << 7);
    mmu.setMapState(state);

    // KERNAL should NOT be served when block is MAP'd
    ASSERT(!bcClaims(bc, 0xE000));
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

    ASSERT(bcClaims(bc, 0xA000));

    MapState state = {};
    state.offsets[5] = 0x300;
    state.enables = (1 << 5);
    mmu.setMapState(state);

    ASSERT(!bcClaims(bc, 0xA000));
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

    bc.ioWrite(nullptr, 0x0001, 0x33);
    ASSERT(bcClaims(bc, 0xD000));

    MapState state = {};
    state.offsets[6] = 0x400;
    state.enables = (1 << 6);
    mmu.setMapState(state);

    ASSERT(!bcClaims(bc, 0xD000));
}

// -----------------------------------------------------------------------
// Reset behavior
// -----------------------------------------------------------------------

TEST_CASE(bank_ctrl_reset_restores_defaults) {
    SparseMemoryBus bus("phys", 28);
    C64BankController bc(&bus);

    uint8_t kernal[8192];
    fillRom(kernal, 8192, 0xE0);
    bc.setKernalRom(kernal, 8192);
    bc.reset();

    // KERNAL visible
    ASSERT(bcClaims(bc, 0xE000));
    ASSERT_EQ(bcRead(bc, 0xE000), 0xE0);

    // Bank out KERNAL
    bc.ioWrite(nullptr, 0x0001, 0x35);
    ASSERT(!bcClaims(bc, 0xE000));

    // Reset should restore defaults (HIRAM=1, KERNAL visible)
    bc.reset();
    ASSERT(bcClaims(bc, 0xE000));
    ASSERT_EQ(bcRead(bc, 0xE000), 0xE0);
}
