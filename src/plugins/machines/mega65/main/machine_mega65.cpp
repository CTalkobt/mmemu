#include "machine_mega65.h"
#include "libcore/main/machine_desc.h"
#include "libcore/main/core_registry.h"
#include "libdevices/main/io_registry.h"
#include "libmem/main/sparse_memory_bus.h"
#include "plugins/devices/map_mmu/main/map_mmu.h"
#include "plugins/45gs02/main/cpu45gs02.h"
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
    // Create MapMmu (virtual address translator)
    // -----------------------------------------------------------------------
    auto* mmu = new MapMmu("mmu", physBus);
    desc->buses.push_back({"mmu", mmu});

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
    // Use IMapController interface to avoid cross-plugin symbol visibility issues
    dynamic_cast<MOS45GS02*>(cpu)->setMapMmu(static_cast<IMapController*>(mmu));

    desc->cpus.push_back({"main", cpu, mmu, mmu, nullptr, true, 1});

    // -----------------------------------------------------------------------
    // Create IORegistry
    // -----------------------------------------------------------------------
    auto* io = new IORegistry();
    desc->ioRegistry = io;

    // -----------------------------------------------------------------------
    // Phase 21.1 COMPLETED:
    // [x] Wire MapMmu as CPU's bus pointer for reads/writes
    // [x] Wire MapMmu to CPU so it can execute MAP instruction
    // [x] Implement IMapController interface (cross-plugin visibility)
    // [x] Implement MAP instruction (0x5C) parameter parsing
    // [x] Fix translate() to avoid double-mapping when MapMmu is in use
    //
    // Phase 21.2 IN PROGRESS:
    // [ ] Load MEGA65 ROMs (KERNAL, BASIC, CHARROM)
    // [ ] Add ROM regions to SparseMemoryBus via addRegion()
    // [ ] Create IORegistry hooks for device integration
    //
    // Phase 21.3 TODO:
    // [ ] Create I/O personality handler for $D02F switching
    // [ ] Integrate with C64BankController for C64 mode
    // [ ] Create devices (VIC-IV, SID, CIA, F018B DMA, etc.)
    // [ ] Add signal wiring (IRQ, NMI, etc.)
    // [ ] Write integration tests
    // -----------------------------------------------------------------------

    return desc;
}
