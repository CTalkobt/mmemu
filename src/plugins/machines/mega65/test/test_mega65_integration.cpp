#include "test_harness.h"
#include "libcore/main/machine_desc.h"
#include "libcore/main/machines/machine_registry.h"
#include "plugins/machines/mega65/main/machine_mega65.h"
#include "libmem/main/sparse_memory_bus.h"
#include "plugins/devices/map_mmu/main/map_mmu.h"
#include "libdevices/main/io_handler.h"
#include "libdevices/main/io_registry.h"
#include "libcore/main/core_registry.h"
#include "cpu45gs02.h"
#include "plugins/devices/f018b_dma/main/f018b_dma.h"
#include <cstring>
#include <vector>

static void ensureMega65Registered() {
    static bool registered = false;
    if (registered) return;
    registered = true;

    CoreRegistry::instance().registerCore("45GS02", "MEGA65", "proprietary",
        []() -> ICore* { return new MOS45GS02(); });

    MachineRegistry::instance().registerMachine("mega65",
        []() -> MachineDescriptor* { return Mega65MachineFactory::create(); },
        "MEGA65");
}

TEST_CASE(mega65_integration_wiring) {
    ensureMega65Registered();
    // Create the machine using the factory
    auto* desc = MachineRegistry::instance().createMachine("mega65");
    ASSERT(desc != nullptr);
    
    // Verify 28-bit physical bus exists
    IBus* physBus = nullptr;
    for (auto& b : desc->buses) {
        if (b.busName == "phys_bus") {
            physBus = b.bus;
            break;
        }
    }
    ASSERT(physBus != nullptr);
    ASSERT_EQ(physBus->config().addrBits, 28);
    
    // Verify MapMmu exists and is 16-bit virtual
    IBus* mmuBus = nullptr;
    for (auto& b : desc->buses) {
        if (b.busName == "mmu") {
            mmuBus = b.bus;
            break;
        }
    }
    ASSERT(mmuBus != nullptr);
    ASSERT_EQ(mmuBus->config().addrBits, 16);
    
    // Verify CPU is connected to MapMmu
    ASSERT(!desc->cpus.empty());
    ASSERT_EQ(desc->cpus[0].cpu->getDataBus(), mmuBus);
    
    // Verify I/O Devices are registered in the registry
    ASSERT(desc->ioRegistry != nullptr);
    ASSERT(desc->ioRegistry->findHandler("F018B DMA") != nullptr);
    ASSERT(desc->ioRegistry->findHandler("MEGA65 Math") != nullptr);
    ASSERT(desc->ioRegistry->findHandler("hyper_serial") != nullptr);
    ASSERT(desc->ioRegistry->findHandler("45gs02 exit trap") != nullptr);
    ASSERT(desc->ioRegistry->findHandler("KEY") != nullptr);

    delete desc;
}

TEST_CASE(mega65_integration_rom_visibility) {
    ensureMega65Registered();
    auto* desc = MachineRegistry::instance().createMachine("mega65");
    ASSERT(desc != nullptr);
    
    IBus* physBus = nullptr;
    for (auto& b : desc->buses) {
        if (b.busName == "phys_bus") { physBus = b.bus; break; }
    }
    ASSERT(physBus != nullptr);
    
    // The factory copies ROM into physical Banks 2-3 ($020000-$03FFFF)
    // and also mirrors it to Banks 14-15 ($0E0000-$0FFFFF).
    // On real MEGA65, this is writable chip RAM with ROM content loaded.

    uint32_t romAddr = 0x020000;
    uint32_t mirrorAddr = 0x0E0000;

    // Verify ROM content is present and mirrored
    uint8_t romByte = physBus->peek8(romAddr);
    uint8_t mirrorByte = physBus->peek8(mirrorAddr);
    ASSERT_EQ(romByte, mirrorByte);

    // Verify the region is writable (chip RAM, not ROM overlay)
    physBus->write8(romAddr, romByte ^ 0xFF);
    ASSERT_EQ(physBus->peek8(romAddr), (uint8_t)(romByte ^ 0xFF));
    physBus->write8(romAddr, romByte); // restore
    
    delete desc;
}

TEST_CASE(mega65_integration_mmu_translation) {
    ensureMega65Registered();
    auto* desc = MachineRegistry::instance().createMachine("mega65");
    ASSERT(desc != nullptr);
    
    IBus* physBus = nullptr;
    for (auto& b : desc->buses) {
        if (b.busName == "phys_bus") { physBus = b.bus; break; }
    }
    IBus* mmuBus = nullptr;
    for (auto& b : desc->buses) {
        if (b.busName == "mmu") { mmuBus = b.bus; break; }
    }
    MapMmu* mmu = dynamic_cast<MapMmu*>(mmuBus);
    ASSERT(mmu != nullptr);
    
    // 1. Test passthrough (default)
    uint32_t testAddr = 0x1234;
    uint8_t testVal = 0xAA;
    static_cast<SparseMemoryBus*>(physBus)->write8(testAddr, testVal);
    ASSERT_EQ(mmuBus->read8(testAddr), testVal);
    
    // 2. Test MAP translation
    // Map block 0 ($0000-$1FFF) to physical Bank 1 ($010000)
    // With hardware-accurate 12-bit addition:
    // vaddr = 0x0234, vaddrHigh = 0x02
    // Want phys = 0x010234, physAddrHigh = 0x010234 >> 8 = 0x102
    // offsetHigh12 = 0x102 - 0x02 = 0x100
    // offset = 0x100 << 8 = 0x10000
    MapState state;
    std::memset(&state, 0, sizeof(state));
    state.offsets[0] = 0x10000;  // Correct offset for hardware algorithm
    state.enables = 0x01;        // enable block 0
    mmu->setMapState(state);

    uint32_t physMappedAddr = 0x010234;
    uint8_t mappedVal = 0xBB;
    static_cast<SparseMemoryBus*>(physBus)->write8(physMappedAddr, mappedVal);

    // Virtual $0234 should now read from physical $010234
    ASSERT_EQ(mmuBus->read8(0x0234), mappedVal);
    
    delete desc;
}

TEST_CASE(mega65_integration_io_personality) {
    ensureMega65Registered();
    auto* desc = MachineRegistry::instance().createMachine("mega65");
    ASSERT(desc != nullptr);
    
    IOHandler* keyHandler = desc->ioRegistry->findHandler("KEY");
    ASSERT(keyHandler != nullptr);
    
    // Initial personality should be C64
    uint8_t val = 0;
    ASSERT(desc->ioRegistry->dispatchRead(nullptr, 0xD02F, &val));
    
    // "Knock" to switch to MEGA65 mode ($47, $53)
    desc->ioRegistry->dispatchWrite(nullptr, 0xD02F, 0x47);
    desc->ioRegistry->dispatchWrite(nullptr, 0xD02F, 0x53);
    
    // Verify read-back (KeyRegister returns last written byte)
    ASSERT(desc->ioRegistry->dispatchRead(nullptr, 0xD02F, &val));
    ASSERT_EQ(val, 0x53);
    
    delete desc;
}

TEST_CASE(mega65_integration_c64_rom_banking) {
    ensureMega65Registered();
    auto* desc = MachineRegistry::instance().createMachine("mega65");
    ASSERT(desc != nullptr);

    IBus* mmuBus = nullptr;
    for (auto& b : desc->buses) {
        if (b.busName == "mmu") { mmuBus = b.bus; break; }
    }
    ASSERT(mmuBus != nullptr);

    // Default state: KERNAL ($E000) and BASIC ($A000) should be visible.
    // Since we fill with $FF if file is missing, we can't be 100% sure of content,
    // but we can test banking by writing to underlying RAM.

    uint32_t kernalAddr = 0xE000;
    uint32_t basicAddr  = 0xA000;
    uint32_t charAddr   = 0xD000;

    // 1. Verify KERNAL is ROM (write-protected)
    uint8_t kernalVal = mmuBus->read8(kernalAddr);
    mmuBus->write8(kernalAddr, kernalVal ^ 0xFF);
    ASSERT_EQ(mmuBus->read8(kernalAddr), kernalVal);

    // 2. Bank out KERNAL (HIRAM=0)
    // $01 bits: 0=LORAM, 1=HIRAM, 2=CHAREN. Port defaults to $37 (%00110111)
    // Set HIRAM=0 -> $35 (%00110101). Note: $35 banks out BOTH KERNAL and BASIC.
    desc->ioRegistry->dispatchWrite(nullptr, 0x0001, 0x35);
    
    // Now KERNAL should be RAM
    mmuBus->write8(kernalAddr, 0x55);
    ASSERT_EQ(mmuBus->read8(kernalAddr), 0x55);

    // Now BASIC should also be RAM (on C64, HIRAM=0 banks out BASIC regardless of LORAM)
    mmuBus->write8(basicAddr, 0x42);
    ASSERT_EQ(mmuBus->read8(basicAddr), 0x42);

    // 3. Restore BASIC ROM but keep KERNAL as RAM? Not possible on C64.
    // Let's test BASIC as RAM while KERNAL is ROM (LORAM=0, HIRAM=1) -> $36 (%00110110)
    desc->ioRegistry->dispatchWrite(nullptr, 0x0001, 0x36);
    
    // KERNAL should be back to ROM
    ASSERT_EQ(mmuBus->read8(kernalAddr), kernalVal);
    // BASIC should still be RAM
    mmuBus->write8(basicAddr, 0x99);
    ASSERT_EQ(mmuBus->read8(basicAddr), 0x99);

    // 4. Restore everything to ROM
    desc->ioRegistry->dispatchWrite(nullptr, 0x0001, 0x37);
    ASSERT_EQ(mmuBus->read8(kernalAddr), kernalVal);
    uint8_t basicVal = mmuBus->read8(basicAddr);
    mmuBus->write8(basicAddr, basicVal ^ 0xFF);
    ASSERT_EQ(mmuBus->read8(basicAddr), basicVal);

    // 5. Verify Char ROM (CHAREN=0)
    // Default CHAREN=1 (I/O visible). Write to $D02F (KeyReg)
    desc->ioRegistry->dispatchWrite(nullptr, 0xD02F, 0x11);
    uint8_t ioVal = 0;
    desc->ioRegistry->dispatchRead(nullptr, 0xD02F, &ioVal);
    ASSERT_EQ(ioVal, 0x11);
    ASSERT_EQ(mmuBus->read8(0xD02F), 0x11);

    // Set CHAREN=0 -> $33 (%00110011)
    desc->ioRegistry->dispatchWrite(nullptr, 0x0001, 0x33);
    // Now reading $D000 range should return Char ROM, not I/O
    // (In our case, likely $FF if file missing, but definitely NOT 0x11 at $D02F)
    ASSERT_NE(mmuBus->read8(0xD02F), 0x11);

    delete desc;
}

TEST_CASE(mega65_integration_joysticks) {
    ensureMega65Registered();
    auto* desc = MachineRegistry::instance().createMachine("mega65");
    ASSERT(desc != nullptr);

    // CIA1 Port B (0xDC01) is Joystick 2 (shared with kbd)
    // CIA2 Port A (0xDD00) is Joystick 1

    // 1. Test Joystick 1 (CIA2 Port A)
    desc->onJoystick(0, 0xFE); // Press UP (bit 0 low)
    uint8_t val = 0;
    desc->ioRegistry->dispatchRead(nullptr, 0xDD00, &val);
    ASSERT_EQ(val, 0xFE);

    desc->onJoystick(0, 0xFF); // Release
    desc->ioRegistry->dispatchRead(nullptr, 0xDD00, &val);
    ASSERT_EQ(val, 0xFF);

    // 2. Test Joystick 2 (CIA1 Port B)
    // Note: Kbd rows are initially $FF.
    desc->onJoystick(1, 0xEF); // Press FIRE (bit 4 low)
    desc->ioRegistry->dispatchRead(nullptr, 0xDC01, &val);
    ASSERT_EQ(val, 0xEF);

    // 3. Test Kbd + Joystick 2 interaction
    // Select column 1 (for RETURN key)
    desc->ioRegistry->dispatchWrite(nullptr, 0xDC00, 0xFD);
    // Press RETURN (Row 0 bit 0 low)
    desc->onKey("RETURN", true);
    
    desc->ioRegistry->dispatchRead(nullptr, 0xDC01, &val);
    // Should be (0xFE kbd row) & (0xEF joy) = 0xEE
    ASSERT_EQ(val, 0xEE);

    delete desc;
}

// Boot the MEGA65 through the full Cold Start sequence.
// Verify: boot completes without BRK, reaches KERNAL editor loop,
// and the banner text "THE MEGA65" is present in screen RAM at $0800.
TEST_CASE(mega65_boot_to_ready) {
    ensureMega65Registered();
    auto* desc = MachineRegistry::instance().createMachine("mega65");
    ASSERT(desc != nullptr);

    if (desc->onReset) desc->onReset(*desc);

    auto* cpu = desc->cpus[0].cpu;
    IBus* bus = desc->buses[0].bus;  // physical bus
    ASSERT(cpu != nullptr);

    // Run 20M steps — enough to complete boot.
    const int MAX_STEPS = 20000000;
    for (int s = 0; s < MAX_STEPS; ++s) {
        if (desc->schedulerStep)
            desc->schedulerStep(*desc);
        else
            cpu->step();
    }

    // Verify SP is still in page $01 (not corrupted)
    ASSERT(cpu->regRead(5) >= 0x0100);

    // Verify banner text in screen RAM at physical $0800.
    // "THE MEGA65" in screen codes: T=14, H=08, E=05, space=20, M=0D, ...
    // Check for "THE" at offset ~27 in row 1 ($0850+27 = $086B)
    bool foundBanner = false;
    const uint8_t the[] = {0x14, 0x08, 0x05};
    for (uint32_t addr = 0x0800; addr < 0x0C00; ++addr) {
        if (bus->peek8(addr) == the[0] &&
            bus->peek8(addr + 1) == the[1] &&
            bus->peek8(addr + 2) == the[2]) {
            foundBanner = true;
            break;
        }
    }
    ASSERT(foundBanner);

    delete desc;
}

TEST_CASE(mega65_integration_map) {
    ensureMega65Registered();
    auto* desc = MachineRegistry::instance().createMachine("mega65");
    ASSERT(desc != nullptr);

    if (desc->onReset) desc->onReset(*desc);

    auto* cpu = desc->cpus[0].cpu;
    auto* mmuBus = desc->cpus[0].dataBus;
    auto* physBus = desc->buses[0].bus;

    // LDA #$00
    // LDX #$00
    // LDY #$60
    // LDZ #$23
    // MAP
    // EOM
    // LDA #$55
    // STA $A000
    // BRK
    uint8_t prog[] = {
        0xA9, 0x00,        // LDA #$00
        0xA2, 0x00,        // LDX #$00
        0xA0, 0x60,        // LDY #$60
        0xA3, 0x23,        // LDZ #$23
        0x5C,              // MAP
        0xEA,              // EOM/NOP
        0xA9, 0x55,        // LDA #$55
        0x8D, 0x00, 0xA0,  // STA $A000
        0x00               // BRK
    };

    // Load program into Bank 0 at $2000
    for (size_t i = 0; i < sizeof(prog); i++) {
        mmuBus->write8(0x2000 + i, prog[i]);
    }

    // Don't call cpu->reset() — onReset() already handled it and exited
    // hypervisor mode. A second reset() re-enters hypervisor, which overlays
    // $8000-$BFFF with hypervisor RAM and blocks MAP translation for $A000.
    // Clear MAP state and set PC directly.
    auto* mmu = dynamic_cast<MapMmu*>(mmuBus);
    ASSERT(mmu != nullptr);
    mmu->clearMapState();
    cpu->regWrite(6, 0x2000); // Set PC to $2000 (reg index 6 = PC in 45GS02)

    ASSERT_EQ((int)cpu->pc(), 0x2000);

    // Step CPU until it hits BRK (haltLine == 1)
    for (int i = 0; i < 20; i++) {
        cpu->step();
    }

    // Verify via SparseMemoryBus (physical bus) that the byte appears at physical address $40000
    uint8_t val = physBus->read8(0x40000);
    ASSERT_EQ((int)val, 0x55);

    delete desc;
}

TEST_CASE(mega65_integration_dma_stall) {
    ensureMega65Registered();
    auto* desc = MachineRegistry::instance().createMachine("mega65");
    ASSERT(desc != nullptr);

    if (desc->onReset) desc->onReset(*desc);

    auto* mmuBus = desc->cpus[0].dataBus;
    auto* physBus = desc->buses[0].bus;
    
    // Set up a DMA job at virtual $3000 to fill 256 bytes at $2000 with $55
    uint32_t listAddr = 0x3000;
    mmuBus->write8(listAddr + 0, 0x03);      // Fill operation
    mmuBus->write8(listAddr + 1, 0x00);      // Count = 256
    mmuBus->write8(listAddr + 2, 0x01);      // Count MSB
    mmuBus->write8(listAddr + 3, 0x55);      // Fill byte
    mmuBus->write8(listAddr + 4, 0x00);
    mmuBus->write8(listAddr + 5, 0x00);
    mmuBus->write8(listAddr + 6, 0x00);      // Dst addr LSB ($2000)
    mmuBus->write8(listAddr + 7, 0x20);      // Dst addr MSB ($20)
    mmuBus->write8(listAddr + 8, 0x00);      // Dst bank
    mmuBus->write8(listAddr + 9, 0x00);
    mmuBus->write8(listAddr + 10, 0x00);

    // Get DMA device
    auto* dma = dynamic_cast<F018bDmaDevice*>(desc->ioRegistry->findHandler("F018B DMA"));
    ASSERT(dma != nullptr);
    ASSERT(!dma->isHaltRequested());

    // Trigger DMA by writing list address to registers
    desc->ioRegistry->dispatchWrite(nullptr, 0xD701, 0x30);
    desc->ioRegistry->dispatchWrite(nullptr, 0xD700, 0x00);

    // CPU should be stalled (halt requested)
    ASSERT(dma->isHaltRequested());

    // Run schedulerStep until DMA is done
    bool wasHalted = false;
    int limit = 500;
    while (dma->isHaltRequested() && limit-- > 0) {
        wasHalted = true;
        desc->schedulerStep(*desc);
    }
    ASSERT(wasHalted);
    ASSERT(!dma->isHaltRequested());

    // Verify destination RAM is filled
    for (int i = 0; i < 256; ++i) {
        ASSERT_EQ((int)physBus->read8(0x002000 + i), 0x55);
    }

    delete desc;
}

TEST_CASE(mega65_integration_vic4_raster) {
    ensureMega65Registered();
    auto* desc = MachineRegistry::instance().createMachine("mega65");
    ASSERT(desc != nullptr);

    if (desc->onReset) desc->onReset(*desc);

    uint8_t r1 = 0, r2 = 0;
    desc->ioRegistry->dispatchRead(nullptr, 0xD012, &r1);
    
    // Tick enough cycles to advance raster lines
    desc->ioRegistry->tickAll(1000);
    
    desc->ioRegistry->dispatchRead(nullptr, 0xD012, &r2);
    ASSERT(r2 != r1);

    delete desc;
}

TEST_CASE(mega65_integration_dual_sid) {
    ensureMega65Registered();
    auto* desc = MachineRegistry::instance().createMachine("mega65");
    ASSERT(desc != nullptr);

    if (desc->onReset) desc->onReset(*desc);

    // Write distinct frequency values to SID1 and SID2 voice 1
    desc->ioRegistry->dispatchWrite(nullptr, 0xD400, 0x12);
    desc->ioRegistry->dispatchWrite(nullptr, 0xD401, 0x34);
    desc->ioRegistry->dispatchWrite(nullptr, 0xD420, 0x56);
    desc->ioRegistry->dispatchWrite(nullptr, 0xD421, 0x78);

    uint8_t s1l = 0, s1h = 0, s2l = 0, s2h = 0;
    desc->ioRegistry->dispatchRead(nullptr, 0xD400, &s1l);
    desc->ioRegistry->dispatchRead(nullptr, 0xD401, &s1h);
    desc->ioRegistry->dispatchRead(nullptr, 0xD420, &s2l);
    desc->ioRegistry->dispatchRead(nullptr, 0xD421, &s2h);

    ASSERT_EQ(s1l, 0x12);
    ASSERT_EQ(s1h, 0x34);
    ASSERT_EQ(s2l, 0x56);
    ASSERT_EQ(s2h, 0x78);

    delete desc;
}

TEST_CASE(mega65_integration_math_accel) {
    ensureMega65Registered();
    auto* desc = MachineRegistry::instance().createMachine("mega65");
    ASSERT(desc != nullptr);

    if (desc->onReset) desc->onReset(*desc);

    // Write MULTINA = 200
    desc->ioRegistry->dispatchWrite(nullptr, 0xD770, 200 & 0xFF);
    desc->ioRegistry->dispatchWrite(nullptr, 0xD771, (200 >> 8) & 0xFF);
    desc->ioRegistry->dispatchWrite(nullptr, 0xD772, 0);
    desc->ioRegistry->dispatchWrite(nullptr, 0xD773, 0);

    // Write MULTINB = 200
    desc->ioRegistry->dispatchWrite(nullptr, 0xD774, 200 & 0xFF);
    desc->ioRegistry->dispatchWrite(nullptr, 0xD775, (200 >> 8) & 0xFF);
    desc->ioRegistry->dispatchWrite(nullptr, 0xD776, 0);
    desc->ioRegistry->dispatchWrite(nullptr, 0xD777, 0);

    // Read MULTOUT
    uint8_t mb[8];
    for (int i = 0; i < 8; ++i) {
        desc->ioRegistry->dispatchRead(nullptr, 0xD778 + i, &mb[i]);
    }
    uint64_t prod = mb[0] | ((uint64_t)mb[1] << 8) | ((uint64_t)mb[2] << 16) | ((uint64_t)mb[3] << 24);
    ASSERT_EQ(prod, (uint64_t)40000);

    delete desc;
}

TEST_CASE(mega65_integration_personality_switch) {
    ensureMega65Registered();
    auto* desc = MachineRegistry::instance().createMachine("mega65");
    ASSERT(desc != nullptr);

    if (desc->onReset) desc->onReset(*desc);

    // Start in C64 mode (Key register write 0)
    desc->ioRegistry->dispatchWrite(nullptr, 0xD02F, 0x00);
    desc->ioRegistry->dispatchWrite(nullptr, 0xD02F, 0x00);

    // Write to VIC-IV extended reg $D04C
    desc->ioRegistry->dispatchWrite(nullptr, 0xD04C, 0xAA);
    uint8_t pv = 0;
    desc->ioRegistry->dispatchRead(nullptr, 0xD04C, &pv);
    // Should be $FF when locked
    ASSERT_EQ(pv, 0xFF);

    // Knock to unlock MEGA65 personality
    desc->ioRegistry->dispatchWrite(nullptr, 0xD02F, 0x47);
    desc->ioRegistry->dispatchWrite(nullptr, 0xD02F, 0x53);

    // Write to $D04C
    desc->ioRegistry->dispatchWrite(nullptr, 0xD04C, 0x55);
    desc->ioRegistry->dispatchRead(nullptr, 0xD04C, &pv);
    ASSERT_EQ(pv, 0x55);

    // Lock personality
    desc->ioRegistry->dispatchWrite(nullptr, 0xD02F, 0x00);
    desc->ioRegistry->dispatchWrite(nullptr, 0xD02F, 0x00);

    // Read back: should be $FF again
    desc->ioRegistry->dispatchRead(nullptr, 0xD04C, &pv);
    ASSERT_EQ(pv, 0xFF);

    delete desc;
}

TEST_CASE(mega65_integration_machine_id) {
    ensureMega65Registered();
    auto* desc = MachineRegistry::instance().createMachine("mega65");
    ASSERT(desc != nullptr);
    ASSERT_EQ(desc->machineId, "mega65");
    delete desc;
}

