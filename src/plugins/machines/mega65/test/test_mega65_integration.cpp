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
    
    // The factory maps ROM to physical Banks 2-3 ($020000-$03FFFF)
    // Even if the file is missing, it should have a 128KB overlay of $FFs
    // which is write-protected.
    
    uint32_t romAddr = 0x020000;
    uint8_t original = physBus->peek8(romAddr);
    
    // Attempt to write to ROM
    physBus->write8(romAddr, original ^ 0xFF);
    
    // Verify it didn't change
    ASSERT_EQ(physBus->peek8(romAddr), original);
    
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
    MapState state;
    std::memset(&state, 0, sizeof(state));
    state.offsets[0] = 0x0100; // physical $010000 >> 8
    state.enables = 0x01;      // enable block 0
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
