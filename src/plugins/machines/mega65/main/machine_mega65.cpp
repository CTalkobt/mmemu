#include "machine_mega65.h"
#include "libcore/main/machine_desc.h"
#include "libcore/main/core_registry.h"
#include "libcore/main/rom_loader.h"
#include "libdevices/main/io_registry.h"
#include "libmem/main/sparse_memory_bus.h"
#include "plugins/devices/map_mmu/main/map_mmu.h"
#include "plugins/devices/map_mmu/main/c64_bank_controller.h"
#include "plugins/devices/map_mmu/main/key_register.h"
#include "plugins/devices/f018b_dma/main/f018b_dma.h"
#include "plugins/devices/mega65_math/main/mega65_math.h"
#include "plugins/devices/hyper_serial/main/hyper_serial.h"
#include "plugins/devices/exit_trap/main/exit_trap.h"
#include "util/path_util.h"
#include <cstring>

MachineDescriptor* Mega65MachineFactory::create() {
    auto* desc = new MachineDescriptor();

    desc->machineId    = "mega65";
    desc->displayName  = "MEGA65";
    desc->licenseClass = "proprietary";

    // -----------------------------------------------------------------------
    // Create 28-bit SparseMemoryBus (physical address space)
    // -----------------------------------------------------------------------
    auto* physBus = new SparseMemoryBus("phys_bus", 28);
    desc->buses.push_back({"phys_bus", physBus});

    // -----------------------------------------------------------------------
    // Load MEGA65 ROM (128 KB) into physical Banks 2-3 ($020000-$03FFFF)
    // -----------------------------------------------------------------------
    uint8_t* romBuf = new uint8_t[128 * 1024];
    if (romLoad("roms/mega65/mega65.rom", romBuf, 128 * 1024)) {
        physBus->addRomOverlay(0x020000, 128 * 1024, romBuf);
    } else {
        // Fallback: Fill with $FF if ROM is missing so emulator doesn't crash
        std::memset(romBuf, 0xFF, 128 * 1024);
        physBus->addRomOverlay(0x020000, 128 * 1024, romBuf);
    }
    desc->deleters.push_back([romBuf]() { delete[] romBuf; });

    // -----------------------------------------------------------------------
    // Create MapMmu (virtual address translator)
    // -----------------------------------------------------------------------
    auto* mmu = new MapMmu("mmu", physBus);
    desc->buses.push_back({"mmu", mmu});

    // -----------------------------------------------------------------------
    // Create C64BankController (ROM overlay banking for C64 compatibility)
    // -----------------------------------------------------------------------
    auto* bankCtrl = new C64BankController(physBus);
    bankCtrl->setMapMmu(mmu);

    // -----------------------------------------------------------------------
    // Create I/O Devices
    // -----------------------------------------------------------------------
    auto* keyReg   = new KeyRegister();
    auto* dma      = new F018bDmaDevice(0xD700);
    auto* math     = new Mega65MathDevice(0xD700);
    auto* serial   = new HyperSerialLogger();
    auto* exitTrap = new ExitTrapDevice(0xD6CF);

    dma->setDmaBus(physBus);

    // -----------------------------------------------------------------------
    // Create IORegistry and register handlers
    // -----------------------------------------------------------------------
    auto* io = new IORegistry();
    io->registerHandler(bankCtrl);
    io->registerHandler(keyReg);
    io->registerHandler(dma);
    io->registerHandler(math);
    io->registerHandler(serial);
    io->registerHandler(exitTrap);
    desc->ioRegistry = io;

    desc->deleters.push_back([bankCtrl]() { delete bankCtrl; });
    desc->deleters.push_back([keyReg]() { delete keyReg; });
    desc->deleters.push_back([dma]() { delete dma; });
    desc->deleters.push_back([math]() { delete math; });
    desc->deleters.push_back([serial]() { delete serial; });
    desc->deleters.push_back([exitTrap]() { delete exitTrap; });

    // -----------------------------------------------------------------------
    // Create 45GS02 CPU
    // -----------------------------------------------------------------------
    CoreRegistry& reg = CoreRegistry::instance();
    ICore* cpu = reg.createCore("45GS02");
    if (!cpu) {
        delete desc;
        return nullptr;
    }

    cpu->setDataBus(mmu);
    cpu->setCodeBus(mmu);

    // Wire MapMmu to CPU so MAP instruction can update mapping state
    cpu->setMapMmu(static_cast<IMapController*>(mmu));

    desc->cpus.push_back({"main", cpu, mmu, mmu, nullptr, true, 1});

    return desc;
}
