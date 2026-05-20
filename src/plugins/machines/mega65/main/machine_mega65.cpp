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
#include "plugins/devices/keyboard/main/keyboard_matrix_mega65.h"
#include "plugins/devices/virtual_iec/main/virtual_iec.h"
#include "plugins/devices/vic4/main/vic4.h"
#include "plugins/devices/sid_pair/main/sid_pair.h"
#include "plugins/devices/cia6526/main/cia6526.h"
#include "libdevices/main/shared_signal_line.h"
#include "libdevices/main/joystick.h"
#include "libdevices/main/combined_port_device.h"
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

    // MEGA65 ROM is a flat 128KB image mapped to physical $020000-$03FFFF.
    // C64-compatible regions mirror the standard C64 memory map within it:
    //   CPU $A000-$BFFF (BASIC)  = phys $02A000 = file offset $A000
    //   CPU $D000-$DFFF (Char)   = phys $02D000 = file offset $D000
    //   CPU $E000-$FFFF (KERNAL) = phys $02E000 = file offset $E000
    bankCtrl->setBasicRom (romBuf + 0xA000, 8192);
    bankCtrl->setKernalRom(romBuf + 0xE000, 8192);
    bankCtrl->setCharRom  (romBuf + 0xD000, 4096);

    // -----------------------------------------------------------------------
    // Create I/O Devices
    // -----------------------------------------------------------------------
    auto* keyReg   = new KeyRegister();
    auto* dma      = new F018bDmaDevice(0xD700);
    auto* math     = new Mega65MathDevice(0xD700);
    auto* serial   = new HyperSerialLogger();
    auto* exitTrap = new ExitTrapDevice(0xD6CF);
    auto* kbd      = new KbdMega65();
    auto* cia1     = new CIA6526("CIA1", 0xDC00);
    auto* cia2     = new CIA6526("CIA2", 0xDD00);
    auto* vic4     = new VIC4();
    auto* joy1     = new Joystick();
    auto* joy2     = new Joystick();

    dma->setDmaBus(physBus);
    vic4->setDmaBus(physBus);
    vic4->setCharRom(romBuf + 0xD000, 4096);

    // Allocate 1KB colour RAM and wire to VIC4
    // (VIC4 also has 32KB internal colour RAM, but the VIC-II compatible
    //  $D800 window reads from this pointer for C64 mode rendering)
    uint8_t* colorRam = new uint8_t[1024];
    std::memset(colorRam, 0, 1024);
    vic4->setColorRam(colorRam);
    desc->deleters.push_back([colorRam]() { delete[] colorRam; });
    
    // Wire keyboard to CIA1
    cia1->setPortADevice(kbd->getPort(0)); // CIA1 Port A drives columns
    
    // CIA1 Port B is shared between Keyboard rows and Joystick 2
    auto* combined1B = new CombinedPortDevice();
    combined1B->addDevice(kbd->getPort(1));
    combined1B->addDevice(joy2);
    cia1->setPortBDevice(combined1B);
    
    // CIA2 Port A is Joystick 1
    cia2->setPortADevice(joy1);
    
    // Wire signals
    auto* sigIrq = new SharedSignalLine("sigIrq");
    cia1->setIrqLine(sigIrq);
    vic4->setIrqLine(sigIrq);
    
    auto* sigNmi = new SharedSignalLine("sigNmi");
    cia2->setIrqLine(sigNmi); // CIA2 IRQ is wired to CPU NMI on Commodore
    kbd->setRestoreLine(sigNmi); // RESTORE key also triggers NMI

    // Personality switch callback
    keyReg->setPersonalityChangeCallback([vic4](IopersonalityMode mode) {
        vic4->setLocked(mode != IopersonalityMode::MEGA65);
    });

    // -----------------------------------------------------------------------
    // Create IORegistry and register handlers
    // -----------------------------------------------------------------------
    auto* io = new IORegistry();
    io->registerHandler(bankCtrl);
    io->registerHandler(keyReg);
    io->registerHandler(dma);
    io->registerHandler(math);
    io->registerHandler(serial);
    io->registerHandler(vic4);
    io->registerHandler(cia1);
    io->registerHandler(cia2);
    io->registerHandler(exitTrap);
    io->registerHandler(kbd); // For discovery via IKeyboardMatrix interface
    desc->ioRegistry = io;

    // Wire I/O hooks to MapMmu so virtual space accesses to $D000 etc. are dispatched
    mmu->setIoHooks(
        [io](IBus* b, uint32_t a, uint8_t* v) { return io->dispatchRead(b, a, v); },
        [io](IBus* b, uint32_t a, uint8_t v) { return io->dispatchWrite(b, a, v); }
    );

    // Joystick callbacks
    desc->onJoystick = [joy1, joy2](int port, uint8_t bits) {
        if (port == 0) joy1->setState(bits);
        if (port == 1) joy2->setState(bits);
    };

    desc->onKey = [kbd](const std::string& keyName, bool down) {
        return kbd->pressKeyByName(keyName, down);
    };

    desc->deleters.push_back([bankCtrl]() { delete bankCtrl; });
    desc->deleters.push_back([keyReg]() { delete keyReg; });
    desc->deleters.push_back([dma]() { delete dma; });
    desc->deleters.push_back([math]() { delete math; });
    desc->deleters.push_back([serial]() { delete serial; });
    desc->deleters.push_back([exitTrap]() { delete exitTrap; });
    desc->deleters.push_back([kbd]() { delete kbd; });
    desc->deleters.push_back([vic4]() { delete vic4; });
    desc->deleters.push_back([cia1]() { delete cia1; });
    desc->deleters.push_back([cia2]() { delete cia2; });
    desc->deleters.push_back([joy1]() { delete joy1; });
    desc->deleters.push_back([joy2]() { delete joy2; });
    desc->deleters.push_back([combined1B]() { delete combined1B; });
    desc->deleters.push_back([sigIrq]() { delete sigIrq; });
    desc->deleters.push_back([sigNmi]() { delete sigNmi; });

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

    // Reset callback: reset all I/O devices, then CPU (so CPU reads
    // the reset vector after bank controller overlays are in place)
    desc->onReset = [](MachineDescriptor& d) {
        if (d.ioRegistry) d.ioRegistry->resetAll();
        for (auto& slot : d.cpus)
            if (slot.cpu) slot.cpu->reset();
    };

    return desc;
}
