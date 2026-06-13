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
#include "plugins/devices/hypervisor/main/hypervisor_regs.h"
#include "plugins/devices/sdcard/main/sdcard.h"
#include "plugins/devices/mega65_io/main/mega65_io_stub.h"
#include <fstream>
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
    // Load BRAM contents (flash menu, freezer, onboarding) into physical RAM.
    // On real hardware these live in FPGA Block RAM, pre-loaded at synthesis.
    // We load from files at the same physical addresses HYPPO expects:
    //   $012000  Freezer       (FREEZER.M65)
    //   $040000  Onboarding    (not currently used)
    //   $050000  Flash utility (mflash.prg, loaded without PRG header)
    // Persistent BRAM: saved to roms/mega65/bram.bin on exit if modified.
    // -----------------------------------------------------------------------
    {
        struct BramEntry { uint32_t addr; uint32_t maxSize; const char* name; };
        BramEntry bramFiles[] = {
            { 0x012000, 56*1024, "FREEZER.M65" },
            { 0x050000, 64*1024, "mflash.prg" },
        };

        // Try persistent BRAM file first
        bool bramLoaded = false;
        std::string bramPath = "roms/mega65/bram.bin";
        {
            std::ifstream bf(bramPath, std::ios::binary);
            if (bf.good()) {
                // BRAM file format: [addr32][size32][data...] repeated
                while (bf.good() && !bf.eof()) {
                    uint32_t addr, size;
                    bf.read((char*)&addr, 4);
                    bf.read((char*)&size, 4);
                    if (!bf.good() || size == 0 || size > 256*1024) break;
                    std::vector<uint8_t> tmp(size);
                    bf.read((char*)tmp.data(), size);
                    if (bf.good()) {
                        for (uint32_t i = 0; i < size; i++)
                            physBus->write8(addr + i, tmp[i]);
                        bramLoaded = true;
                    }
                }
                if (bramLoaded)
                    fprintf(stderr, "[MEGA65] Loaded persistent BRAM from: %s\n", bramPath.c_str());
            }
        }

        // If no persistent BRAM, load from individual files
        if (!bramLoaded) {
            std::vector<std::string> searchDirs = { "roms/mega65/", "" };
            const char* home = std::getenv("HOME");
            if (home) {
                searchDirs.push_back(std::string(home) + "/.local/share/xemu-lgb/mega65/");
                searchDirs.push_back(std::string(home) + "/.local/share/mmsim/");
            }

            for (auto& entry : bramFiles) {
                for (const auto& dir : searchDirs) {
                    std::string path = dir + entry.name;
                    std::ifstream f(path, std::ios::binary);
                    if (!f.good()) continue;

                    f.seekg(0, std::ios::end);
                    size_t fileSize = f.tellg();
                    f.seekg(0);

                    // PRG files: skip 2-byte load address header
                    size_t skip = 0;
                    std::string ext = path.substr(path.rfind('.') + 1);
                    if (ext == "prg" || ext == "PRG") skip = 2;

                    if (skip) f.seekg(skip);
                    size_t dataSize = fileSize - skip;
                    if (dataSize > entry.maxSize) dataSize = entry.maxSize;

                    std::vector<uint8_t> tmp(dataSize);
                    f.read((char*)tmp.data(), dataSize);
                    for (size_t i = 0; i < dataSize; i++)
                        physBus->write8(entry.addr + i, tmp[i]);

                    fprintf(stderr, "[MEGA65] Loaded BRAM %s (%zuB) at $%06X from: %s\n",
                            entry.name, dataSize, entry.addr, path.c_str());
                    break;
                }
            }
        }
    }

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
    bankCtrl->setFullRom(romBuf, 128 * 1024);
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
    auto* sdcard   = new SdCardDevice(0xD680);
    auto* ioStub   = new Mega65IoStub();
    auto* kbd      = new KbdMega65();
    auto* cia1     = new CIA6526("CIA1", 0xDC00);
    auto* cia2     = new CIA6526("CIA2", 0xDD00);
    auto* vic4     = new VIC4();
    auto* joy1     = new Joystick();
    auto* joy2     = new Joystick();

    // Wire $D030 ROM banking query from VIC3 to bank controller
    bankCtrl->setD030Query([vic4]() -> uint8_t {
        uint8_t val = 0;
        vic4->ioRead(nullptr, 0xD030, &val);
        return val;
    });

    // Hypervisor query wired after CPU creation (see below)

    dma->setDmaBus(physBus);
    vic4->setDmaBus(physBus);
    vic4->setCharRom(romBuf + 0xD000, 4096);

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
    io->registerHandler(sdcard);
    io->registerHandler(ioStub);  // Catch-all for $D600-$D6FF and colour RAM $D800-$DBFF
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

    // -----------------------------------------------------------------------
    // Load HYPPO Hypervisor ROM (16 KB)
    // -----------------------------------------------------------------------
    auto* cpu45 = static_cast<MOS45GS02*>(cpu);

    // KEY register: block personality changes while in hypervisor mode
    keyReg->setHypervisorCheck([cpu45]() { return cpu45->isHypervisor(); });

    // Wire hypervisor mode query (ROM banking disabled in hypervisor mode)
    bankCtrl->setHypervisorQuery([cpu45]() -> bool {
        return cpu45->isHypervisor();
    });

    uint8_t* hyperRom = nullptr;
    {
        hyperRom = new uint8_t[16384];
        bool hyperLoaded = false;

        // Search paths for HICKUP.M65
        std::vector<std::string> hyperPaths = {
            "roms/mega65/HICKUP.M65",
            "HICKUP.M65",
        };
        // Also check xemu standard location
        const char* home = std::getenv("HOME");
        if (home) {
            hyperPaths.push_back(std::string(home) + "/.local/share/xemu-lgb/mega65/HICKUP.M65");
        }

        for (const auto& path : hyperPaths) {
            std::ifstream f(path, std::ios::binary);
            if (f.good()) {
                f.read((char*)hyperRom, 16384);
                if (f.gcount() == 16384) {
                    hyperLoaded = true;
                    fprintf(stderr, "[MEGA65] Loaded HYPPO from: %s\n", path.c_str());
                    break;
                }
            }
        }
        if (!hyperLoaded) {
            fprintf(stderr, "[MEGA65] HYPPO not found, falling back to C64 reset vector\n");
        }

        if (hyperLoaded) {
            cpu45->setHypervisorRom(hyperRom, 16384);
            // Also map hypervisor RAM into physical bus at $0FFF8000-$0FFFBFFF
            // so DMA can read the job lists embedded in HYPPO code.
            for (uint32_t i = 0; i < 16384; i++)
                physBus->write8(0x0FFF8000 + i, hyperRom[i]);
            desc->deleters.push_back([hyperRom]() { delete[] hyperRom; });

            // Wire hypervisor overlay into MapMmu so debugger reads see
            // HYPPO code at $8000-$BFFF when CPU is in hypervisor mode
            mmu->setHypervisorOverlay(
                [cpu45]() { return cpu45->isHypervisor(); },
                cpu45->hyperRam(), 0x8000, 16384);
        } else {
            // No HYPPO — fall back to standard 6502 reset vector
            delete[] hyperRom;
            hyperRom = nullptr;
        }

        // Wire hypervisor registers ($D640-$D67F)
        auto* hyperRegs = new HypervisorRegs(cpu45);
        io->registerHandler(hyperRegs);
        desc->deleters.push_back([hyperRegs]() { delete hyperRegs; });
    }

    desc->cpus.push_back({"main", cpu, mmu, mmu, nullptr, true, 1});

    // Try to auto-mount SD card image
    {
        std::vector<std::string> sdPaths = {
            "roms/mega65/mega65.img",
            "roms/mega65/sdcard.img",
        };
        const char* home = std::getenv("HOME");
        if (home) {
            sdPaths.push_back(std::string(home) + "/.local/share/xemu-lgb/mega65/mega65.img");
            sdPaths.push_back(std::string(home) + "/.local/share/xemu-lgb/mega65/mega65_sd.img");
            sdPaths.push_back(std::string(home) + "/.local/share/mmsim/mega65.img");
        }
        bool mounted = false;
        for (const auto& path : sdPaths) {
            if (sdcard->mountImage(path)) {
                fprintf(stderr, "[MEGA65] Mounted SD card image: %s\n", path.c_str());
                mounted = true;
                break;
            }
        }
        if (!mounted) {
            fprintf(stderr, "[MEGA65] WARNING: No SD card image found. HYPPO will not load C65 ROMs.\n");
            fprintf(stderr, "[MEGA65]   Searched: roms/mega65/mega65.img, ~/.local/share/xemu-lgb/mega65/mega65.img\n");
            fprintf(stderr, "[MEGA65]   Copy or symlink a MEGA65 SD card image to one of these locations.\n");
        }
    }

    // Workaround: our mflash.prg doesn't use the $CF80 HYPPO callback.
    // It exits via RTS which pops from $0200/$0201.  Place the
    // return_from_flashmenu address ($A4D9-1 = $A4D8) there so the
    // RTS returns to HYPPO's boot continuation.
    physBus->write8(0x0200, 0xD8);  // return_from_flashmenu - 1, low byte
    physBus->write8(0x0201, 0xA4);  // high byte

    // Scheduler: tick I/O devices after each CPU step (needed for cycle-by-cycle DMA).
    // When DMA is active, tick devices without stepping the CPU (CPU is halted).
    desc->schedulerStep = [dma](MachineDescriptor& d) -> int {
        if (dma->isHaltRequested()) {
            // DMA active: tick devices (processes one DMA byte), CPU stays halted
            if (d.ioRegistry) d.ioRegistry->tickAll(1);
            return 1;
        }
        auto* cpu = d.cpus[0].cpu;
        int cycles = cpu->step();
        if (d.ioRegistry) d.ioRegistry->tickAll(1);
        return cycles;
    };

    // Reset callback: reset all I/O devices, then CPU (so CPU reads
    // the reset vector after bank controller overlays are in place)
    desc->onReset = [](MachineDescriptor& d) {
        if (d.ioRegistry) d.ioRegistry->resetAll();
        for (auto& slot : d.cpus)
            if (slot.cpu) slot.cpu->reset();
    };

    return desc;
}
