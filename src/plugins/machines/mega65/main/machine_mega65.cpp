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
#include "plugins/devices/mega65_hypervisor/main/hypervisor_regs.h"
#include "plugins/devices/mega65_hypervisor/main/hdos_handler.h"
#include "plugins/devices/sdcard/main/sdcard.h"
// #include "plugins/devices/mega65_rtc/main/mega65_rtc.h"  // DISABLED (issue #109)
#include "plugins/devices/mega65_io/main/mega65_io_stub.h"
#include <fstream>
#include "plugins/devices/sid_pair/main/sid_pair.h"
#include "plugins/devices/cia6526/main/cia6526.h"
#include "libdevices/main/shared_signal_line.h"
#include "libdevices/main/shared_irq_manager.h"
#include "libdevices/main/joystick.h"
#include "libdevices/main/combined_port_device.h"
#include "util/path_util.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <memory>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// MEGA65 keyboard buffer: map key names to ASCII + PETSCII codes
// ---------------------------------------------------------------------------

// Modifier bit positions for $D60A/$D611
static constexpr uint8_t MOD_LSHIFT = 0x01;
static constexpr uint8_t MOD_RSHIFT = 0x02;
static constexpr uint8_t MOD_CTRL   = 0x04;
static constexpr uint8_t MOD_MEGA   = 0x08;
static constexpr uint8_t MOD_ALT    = 0x10;
static constexpr uint8_t MOD_NOSCRL = 0x20;
static constexpr uint8_t MOD_CAPS   = 0x40;

struct Mega65KeyCode {
    uint8_t ascii;
    uint8_t petscii;
};

/// Map a key name (uppercase, as from pressKeyByName) + current modifier
/// state to ASCII and PETSCII codes.  Returns {0,0} if the key should not
/// produce a buffer event (pure modifier keys, unknown keys).
static Mega65KeyCode mega65KeyCode(const std::string& name, uint8_t mods) {
    bool shifted = (mods & (MOD_LSHIFT | MOD_RSHIFT)) != 0;

    // Letters: ASCII is case-sensitive; PETSCII unshifted = uppercase
    if (name.size() == 1 && name[0] >= 'A' && name[0] <= 'Z') {
        char ch = name[0];
        uint8_t ascii   = shifted ? ch : (ch + 32); // A→a unshifted, A shifted
        uint8_t petscii = shifted ? (uint8_t)(ch + 32) : (uint8_t)ch; // PETSCII: unshifted=upper
        return {ascii, petscii};
    }

    // Digits
    if (name.size() == 1 && name[0] >= '0' && name[0] <= '9') {
        static const char shiftDigit[] = ")!\"#$%&'("; // shifted 0-9 → )!"#$%&'(
        uint8_t ch = (uint8_t)name[0];
        if (shifted) {
            uint8_t sc = (uint8_t)shiftDigit[ch - '0'];
            return {sc, sc};
        }
        return {ch, ch};
    }

    // Function keys — user-specified PC→MEGA65 mapping:
    // PC F5→MEGA65 F1 (PETSCII $85), PC F6→F3 ($86), PC F7→F5 ($87), PC F8→F7 ($88)
    // PC F9→F9 ($89), PC F10→F11 ($8A), PC F11→F13 ($8B)
    // Shifted versions: F2 ($89→no, per C64 convention: F2=$89, F4=$8A, etc.)
    // MEGA65 PETSCII: F1=$85, F2=$89, F3=$86, F4=$8A, F5=$87, F6=$8B, F7=$88, F8=$8C
    //                 F9=$85+8=$8D, F10=$8E, F11=$8F (extended)
    if (name == "F1") {
        // PC F5 maps here via wxKeyToVic20Name. MEGA65 F1/F2.
        return {0, shifted ? (uint8_t)0x89 : (uint8_t)0x85};
    }
    if (name == "F3") {
        return {0, shifted ? (uint8_t)0x8A : (uint8_t)0x86};
    }
    if (name == "F5") {
        return {0, shifted ? (uint8_t)0x8B : (uint8_t)0x87};
    }
    if (name == "F7") {
        return {0, shifted ? (uint8_t)0x8C : (uint8_t)0x88};
    }

    // Extended function keys (F9-F14, MEGA65-only)
    if (name == "F9")  return {0, shifted ? (uint8_t)0x8E : (uint8_t)0x8D};
    if (name == "F10") return {0, shifted ? (uint8_t)0x8E : (uint8_t)0x8D}; // alias
    if (name == "F11") return {0, shifted ? (uint8_t)0x90 : (uint8_t)0x8F};
    if (name == "F12") return {0, shifted ? (uint8_t)0x90 : (uint8_t)0x8F}; // alias
    if (name == "F13") return {0, (uint8_t)0x8F};
    if (name == "F14") return {0, (uint8_t)0x90};

    // MEGA65-specific keys mapped from PC keyboard
    // PC ESC → RUN/STOP (PETSCII $03)
    if (name == "RUNSTOP" || name == "RUN_STOP")
        return {0x1B, 0x03};

    // MEGA65 ESC key (row 7, col 1 on matrix)
    if (name == "ESC")
        return {0x1B, 0x1B};

    // Standard keys
    if (name == "RETURN" || name == "ENTER")
        return {0x0D, 0x0D};
    if (name == "SPACE")
        return {0x20, 0x20};
    if (name == "DEL" || name == "BACKSPACE" || name == "DELETE")
        return {0x14, 0x14}; // PETSCII DEL
    if (name == "HOME")
        return {0x13, shifted ? (uint8_t)0x93 : (uint8_t)0x13};
    if (name == "TAB")
        return {0x09, 0x09};

    // Cursor keys (PETSCII codes)
    if (name == "UP")
        return {0, 0x91};
    if (name == "DOWN")
        return {0, 0x11};
    if (name == "LEFT")
        return {0, 0x9D};
    if (name == "RIGHT")
        return {0, 0x1D};

    // Punctuation
    if (name == "+")       return {'+', '+'};
    if (name == "-")       return {'-', '-'};
    if (name == "*")       return {'*', '*'};
    if (name == "/")       return {'/', '/'};
    if (name == "=")       return {'=', '='};
    if (name == ".")       return {'.', '.'};
    if (name == ",")       return {',', ','};
    if (name == ":")       return {':', ':'};
    if (name == ";")       return {';', ';'};
    if (name == "@")       return {'@', '@'};
    if (name == "^")       return {'^', '^'};
    if (name == "POUND")   return {0x5C, 0x5C}; // backslash position

    // Shifted punctuation combos (these arrive as their own key names)
    if (name == "!")       return {'!', '!'};
    if (name == "\"")      return {'"', '"'};
    if (name == "#")       return {'#', '#'};
    if (name == "$")       return {'$', '$'};
    if (name == "%")       return {'%', '%'};
    if (name == "&")       return {'&', '&'};
    if (name == "'")       return {'\'', '\''};
    if (name == "(")       return {'(', '('};
    if (name == ")")       return {')', ')'};

    // Pure modifier keys — update state but don't generate a buffer event
    if (name == "SHIFT_L" || name == "LSHIFT" ||
        name == "LEFT_SHIFT" || name == "SHIFT_R" || name == "RSHIFT" ||
        name == "CTRL" || name == "CONTROL" ||
        name == "COMMODORE" || name == "MEGA" ||
        name == "ALT" || name == "CAPS" || name == "CAPS_LOCK" ||
        name == "NOSCRL" || name == "RESTORE")
        return {0, 0}; // no buffer event

    return {0, 0};
}

/// Map a key name to the modifier bit it represents (0 if not a modifier).
static uint8_t mega65ModBit(const std::string& name) {
    if (name == "SHIFT_L" || name == "LSHIFT" || name == "LEFT_SHIFT") return MOD_LSHIFT;
    if (name == "SHIFT_R" || name == "RSHIFT")                        return MOD_RSHIFT;
    if (name == "CTRL" || name == "CONTROL")                          return MOD_CTRL;
    if (name == "COMMODORE" || name == "MEGA")                        return MOD_MEGA;
    if (name == "ALT")                                                return MOD_ALT;
    if (name == "NOSCRL")                                             return MOD_NOSCRL;
    if (name == "CAPS" || name == "CAPS_LOCK")                        return MOD_CAPS;
    return 0;
}

// ---------------------------------------------------------------------------

// Load mega65.json config, return empty object on failure
static json loadMega65Config() {
    std::vector<std::string> paths = {
        "machines/mega65.json",
        PathUtil::findResource("machines/mega65.json"),
    };
    for (const auto& p : paths) {
        std::ifstream f(p);
        if (!f.good()) continue;
        try {
            json doc = json::parse(f);
            if (doc.contains("machines") && doc["machines"].is_array() && !doc["machines"].empty())
                return doc["machines"][0];
        } catch (...) {}
    }
    return json::object();
}

// Search for a file in a list of paths, expanding ~ to $HOME
static std::string findFile(const json& paths, const std::string& fallback = "") {
    const char* home = std::getenv("HOME");
    for (const auto& p : paths) {
        std::string path = p.get<std::string>();
        if (path.size() > 1 && path[0] == '~' && path[1] == '/') {
            if (home) path = std::string(home) + path.substr(1);
            else continue;
        }
        std::ifstream f(path, std::ios::binary);
        if (f.good()) return path;
    }
    return fallback;
}

MachineDescriptor* Mega65MachineFactory::create() {
    json cfg = loadMega65Config();
    auto* desc = new MachineDescriptor();

    desc->machineId    = "mega65";
    desc->displayName  = "MEGA65";
    desc->licenseClass = "proprietary";

    // -----------------------------------------------------------------------
    // Create 28-bit SparseMemoryBus (physical address space)
    // -----------------------------------------------------------------------
    auto* physBus = new SparseMemoryBus("phys_bus", 28);
    desc->buses.push_back({"phys_bus", physBus});

    // Map 8 MB of physical RAM
    uint8_t* ram = new uint8_t[8 * 1024 * 1024];
    std::memset(ram, 0, 8 * 1024 * 1024);
    physBus->addRegion(0, 8 * 1024 * 1024, ram, true);
    desc->deleters.push_back([ram]() { delete[] ram; });

    // -----------------------------------------------------------------------
    // Load MEGA65 ROM (128 KB) into physical Banks 2-3 ($020000-$03FFFF)
    // -----------------------------------------------------------------------
    uint8_t* romBuf = new uint8_t[128 * 1024];
    std::memset(romBuf, 0xFF, 128 * 1024);
    {
        // Find mega65.rom from JSON config or fallback paths
        json romPaths = json::array({"roms/mega65/mega65.rom"});
        if (cfg.contains("roms")) {
            for (const auto& r : cfg["roms"]) {
                if (r.value("label", "") == "mega65rom" && r.contains("paths"))
                    romPaths = r["paths"];
            }
        }
        std::string romPath = findFile(romPaths, "roms/mega65/mega65.rom");
        if (!romPath.empty())
            romLoad(romPath.c_str(), romBuf, 128 * 1024);
    }
    for (uint32_t i = 0; i < 128 * 1024; i++) {
        physBus->write8(0x020000 + i, romBuf[i]);
        physBus->write8(0x0E0000 + i, romBuf[i]);
    }
    physBus->clearWriteLog();
    desc->deleters.push_back([romBuf]() { delete[] romBuf; });

    // -----------------------------------------------------------------------
    // Load BRAM contents (flash menu, freezer, onboarding) into physical RAM.
    // On real hardware these live in FPGA Block RAM, pre-loaded at synthesis.
    // Not required for boot (HYPPO is skipped), but loaded for completeness
    // and potential future freezer/flash menu support.
    // Persistent BRAM: saved to roms/mega65/bram.bin on exit if modified.
    // -----------------------------------------------------------------------
    {
        struct BramEntry { uint32_t addr; uint32_t maxSize; std::string name;
                           json searchPaths; };
        std::vector<BramEntry> bramFiles;

        // Load BRAM entries from JSON config or use defaults
        if (cfg.contains("bram") && cfg["bram"].is_array()) {
            for (const auto& b : cfg["bram"]) {
                BramEntry e;
                e.addr = b.contains("physAddr") ? (uint32_t)std::stoul(b["physAddr"].get<std::string>(), nullptr, 0) : 0;
                e.maxSize = b.value("maxSize", 65536);
                e.name = b.value("name", "");
                e.searchPaths = b.contains("paths") ? b["paths"] : json::array();
                bramFiles.push_back(e);
            }
        } else {
            bramFiles.push_back({0x012000, 56*1024, "FREEZER.M65", json::array()});
            bramFiles.push_back({0x050000, 64*1024, "mflash.prg", json::array()});
        }

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
            for (auto& entry : bramFiles) {
                // Build search paths: JSON paths first, then fallback dirs
                json paths = entry.searchPaths;
                if (paths.empty()) {
                    paths.push_back("roms/mega65/" + entry.name);
                    paths.push_back("~/.local/share/xemu-lgb/mega65/" + entry.name);
                    paths.push_back("~/.local/share/mmsim/" + entry.name);
                }
                std::string path = findFile(paths);
                if (path.empty()) continue;
                std::ifstream f(path, std::ios::binary);
                if (!f.good()) continue;

                f.seekg(0, std::ios::end);
                size_t fileSize = f.tellg();
                f.seekg(0);

                // Note: Do not skip the 2-byte PRG header for BRAM files (like mflash.prg),
                // as HYPPO expects it to be present at $050000.
                size_t skip = 0;
                size_t dataSize = fileSize - skip;
                if (dataSize > entry.maxSize) dataSize = entry.maxSize;

                std::vector<uint8_t> tmp(dataSize);
                f.read((char*)tmp.data(), dataSize);
                for (size_t i = 0; i < dataSize; i++)
                    physBus->write8(entry.addr + i, tmp[i]);

                fprintf(stderr, "[MEGA65] Loaded BRAM %s (%zuB) at $%06X from: %s\n",
                        entry.name.c_str(), dataSize, entry.addr, path.c_str());
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
    // NOTE: RTC/NVRAM accessed via I2C on real hardware, not direct memory mapping.
    // Fake RTC at $D710-$D77F was incorrectly colliding with CPU speed control ($D710)
    // and Audio DMA registers ($D711-$D73F). Disabled to fix issue #109.
    // auto* rtc      = new Mega65Rtc(0xD700);  // DISABLED
    auto* ioStub   = new Mega65IoStub();
    auto* kbd      = new KbdMega65();
    auto* cia1     = new CIA6526("CIA1", 0xDC00);
    auto* cia2     = new CIA6526("CIA2", 0xDD00);
    auto* vic4     = new VIC4();
    auto* sidPair  = new SidPair();
    auto* joy1     = new Joystick();
    auto* joy2     = new Joystick();

    vic4->setPal(true);  // MEGA65 defaults to PAL timing
    sidPair->setClockHz(985248);  // PAL clock for SID

    // Wire $D030 ROM banking query from VIC3 to bank controller.
    // When VIC-III is locked (C64 mode), $D030 reads as $FF but the ROM
    // banking bits are not functional — return $00 so only $01 port controls banking.
    bankCtrl->setD030Query([vic4]() -> uint8_t {
        if (vic4->isLocked()) return 0x00;
        uint8_t val = 0;
        vic4->ioRead(nullptr, 0xD030, &val);
        return val;
    });

    // Hypervisor query wired after CPU creation (see below)

    dma->setDmaBus(physBus);
    vic4->setDmaBus(physBus);
    vic4->setCharRom(romBuf + 0xD000, 4096);

    // Share colour RAM between VIC4 and Mega65IoStub.
    // Colour RAM: 32KB buffer shared between IOStub ($D800 CPU access),
    // VIC4 (rendering), and physical bus.  On real hardware, colour RAM is
    // at $FF80000 (I/O space).  The KERNAL's cint uses DMA with bank $08
    // ($080000) to fill attribute/colour RAM.  Map at both addresses.
    vic4->setColorRam(ioStub->colorRam());
    physBus->addRegion(0x0FF80000, 32768, ioStub->colorRam(), true);
    physBus->addRegion(0x00080000, 32768, ioStub->colorRam(), true);
    
    // Wire keyboard to CIA1
    cia1->setPortADevice(kbd->getPort(0)); // CIA1 Port A drives columns
    
    // CIA1 Port B is shared between Keyboard rows and Joystick 2
    auto* combined1B = new CombinedPortDevice();
    combined1B->addDevice(kbd->getPort(1));
    combined1B->addDevice(joy2);
    cia1->setPortBDevice(combined1B);
    
    // CIA2 Port A is Joystick 1
    cia2->setPortADevice(joy1);
    
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
    io->registerHandler(sidPair);
    io->registerHandler(cia1);
    io->registerHandler(cia2);
    io->registerHandler(exitTrap);
    io->registerHandler(sdcard);
    // rtc registration disabled (issue #109: RTC accessed via I2C on real hardware)
    io->registerHandler(ioStub);  // Catch-all for $D600-$D6FF and colour RAM $D800-$DBFF
    io->registerHandler(kbd); // For discovery via IKeyboardMatrix interface
    dma->setIoRegistry(io);  // DMA needs I/O dispatch for bank byte bit 7
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

    // Keyboard: drive both the CIA matrix (for C64-compatible scanning)
    // and the MEGA65 ASCII/PETSCII keyboard buffer ($D610/$D619).
    // A shared modifier byte tracks current state for $D611.
    //
    // PC→MEGA65 function key mapping:
    //   PC ESC → RUN/STOP      PC F1 → ESC         PC F2 → ALT
    //   PC F3  → CAPS LOCK     PC F4 → NO SCROLL
    //   PC F5  → F1/F2         PC F6 → F3/F4       PC F7 → F5/F6
    //   PC F8  → F7/F8         PC F9 → F9/F10      PC F10→ F11/F12
    //   PC F11 → F13/F14
    auto modState = std::make_shared<uint8_t>(0);
    desc->onKey = [kbd, ioStub, modState](const std::string& keyName, bool down) {
        // Normalise key name to uppercase
        std::string name = keyName;
        std::transform(name.begin(), name.end(), name.begin(), ::toupper);

        // PC→MEGA65 function key remapping
        if      (name == "F1") name = "ESC";
        else if (name == "F2") name = "ALT";
        else if (name == "F3") name = "CAPS";
        else if (name == "F4") name = "NOSCRL";
        else if (name == "F5") name = "F1";
        else if (name == "F6") name = "F3";
        else if (name == "F7") name = "F5";
        else if (name == "F8") name = "F7";
        // F9-F12: no C64 matrix equivalent, buffer-only below

        // Map PC ALT to MEGA65 Commodore/MEGA key
        if (name == "ALT") name = "COMMODORE";

        // GUI sends "CONTROL" for Ctrl key
        if (name == "CONTROL") name = "CTRL";

        // GUI sends "RUN_STOP" for ESC key
        if (name == "RUN_STOP") name = "RUNSTOP";

        // Update the CIA matrix (for C64-compatible keyboard scanning)
        bool ok = kbd->pressKeyByName(name, down);

        // Track modifier state for $D611
        uint8_t modBit = mega65ModBit(name);
        if (modBit) {
            if (down) *modState |= modBit;
            else      *modState &= ~modBit;
            ioStub->setModifiers(*modState);
            return ok;
        }

        // On key-down, push an event into the ASCII/PETSCII buffer
        if (down) {
            Mega65KeyCode kc = mega65KeyCode(name, *modState);
            if (kc.ascii || kc.petscii) {
                ioStub->pushKey(kc.ascii, kc.petscii, *modState);
            }
        }
        return ok;
    };

    desc->deleters.push_back([bankCtrl]() { delete bankCtrl; });
    desc->deleters.push_back([keyReg]() { delete keyReg; });
    desc->deleters.push_back([dma]() { delete dma; });
    desc->deleters.push_back([math]() { delete math; });
    desc->deleters.push_back([serial]() { delete serial; });
    desc->deleters.push_back([exitTrap]() { delete exitTrap; });
    // rtc deleter removed (issue #109: RTC device disabled)
    desc->deleters.push_back([kbd]() { delete kbd; });
    desc->deleters.push_back([vic4]() { delete vic4; });
    desc->deleters.push_back([sidPair]() { delete sidPair; });
    desc->deleters.push_back([cia1]() { delete cia1; });
    desc->deleters.push_back([cia2]() { delete cia2; });
    desc->deleters.push_back([joy1]() { delete joy1; });
    desc->deleters.push_back([joy2]() { delete joy2; });
    desc->deleters.push_back([combined1B]() { delete combined1B; });

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

    // Wire signals — use SharedIrqManager for proper wired-OR that drives
    // the CPU's IRQ/NMI pins.  SharedSignalLine doesn't push to the CPU.
    auto* irqMgr = new SharedIrqManager(cpu45);
    cia1->setIrqLine(irqMgr->createLine());
    vic4->setIrqLine(irqMgr->createLine());
    desc->deleters.push_back([irqMgr]() { delete irqMgr; });

    // NMI: CIA2 IRQ output is wired to CPU NMI on Commodore.
    // RESTORE key also triggers NMI.  Use SharedNmiManager for wired-OR.
    auto* nmiMgr = new SharedNmiManager(cpu45);
    cia2->setIrqLine(nmiMgr->createLine()); // CIA2 IRQ → CPU NMI
    kbd->setRestoreLine(nmiMgr->createLine());
    desc->deleters.push_back([nmiMgr]() { delete nmiMgr; });

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

        // Search paths for HICKUP.M65 — from JSON config or defaults
        json hyperPathsJson = json::array({"roms/mega65/HICKUP.M65", "HICKUP.M65",
                                           "~/.local/share/xemu-lgb/mega65/HICKUP.M65"});
        if (cfg.contains("roms")) {
            for (const auto& r : cfg["roms"]) {
                if (r.value("label", "") == "hyppo" && r.contains("paths"))
                    hyperPathsJson = r["paths"];
            }
        }
        std::string hyperPath = findFile(hyperPathsJson);
        if (!hyperPath.empty()) {
            std::ifstream f(hyperPath, std::ios::binary);
            if (f.good()) {
                f.read((char*)hyperRom, 16384);
                if (f.gcount() == 16384) {
                    hyperLoaded = true;
                    fprintf(stderr, "[MEGA65] Loaded HYPPO from: %s\n", hyperPath.c_str());
                }
            }
        }
        if (!hyperLoaded) {
            fprintf(stderr, "[MEGA65] HYPPO not found, falling back to C64 reset vector\n");
        }

        if (hyperLoaded) {
            cpu45->setHypervisorRom(hyperRom, 16384);
            // Map hypervisor RAM as a writable region on the physical bus at
            // $0FFF8000-$0FFFBFFF. This uses the SAME buffer as m_hyperRam,
            // so DMA writes to $0FFF8000+ (e.g. longpeek results) are visible
            // to the CPU via the hypervisor overlay at $8000-$BFFF.
            physBus->addRegion(0x0FFF8000, 16384, cpu45->hyperRam(), true);
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

        // HDOS trap virtualization — intercept DOS traps for host filesystem access
        auto* hdos = new HdosHandler();
        hdos->setPhysBus(physBus);

        // Set HDOS root directory from JSON config or defaults
        {
            json hdosPaths = json::array({
                "roms/mega65/sdcard/",
                "~/.local/share/xemu-lgb/mega65/hdos/",
                "."
            });
            if (cfg.contains("hdos") && cfg["hdos"].contains("paths"))
                hdosPaths = cfg["hdos"]["paths"];

            std::string hdosRoot = ".";
            const char* home = std::getenv("HOME");
            for (const auto& p : hdosPaths) {
                std::string path = p.get<std::string>();
                if (path.size() > 1 && path[0] == '~' && path[1] == '/' && home)
                    path = std::string(home) + path.substr(1);
                if (!path.empty() && std::filesystem::is_directory(path)) {
                    hdosRoot = path;
                    break;
                }
            }
            hdos->setRootDir(hdosRoot);
        }
        desc->deleters.push_back([hdos]() { delete hdos; });

        hyperRegs->setHdosTrapHandler([hdos](uint8_t func, MOS45GS02* cpu) -> bool {
            return hdos->handleTrap(func, cpu);
        });
    }

    desc->cpus.push_back({"main", cpu, mmu, mmu, nullptr, true, 1});

    // Auto-mount SD card image — paths from JSON config (#50)
    {
        json sdPaths = json::array();
        if (cfg.contains("sdcard") && cfg["sdcard"].contains("paths"))
            sdPaths = cfg["sdcard"]["paths"];

        std::string sdPath = findFile(sdPaths);
        if (!sdPath.empty() && sdcard->mountImage(sdPath)) {
            fprintf(stderr, "[MEGA65] Mounted SD card image: %s\n", sdPath.c_str());
        } else {
            fprintf(stderr, "[MEGA65] WARNING: No SD card image found. HDOS file operations may fail.\n");
            fprintf(stderr, "[MEGA65]   Configure paths in machines/mega65.json\n");
        }
    }


    // Bus contention model (#20): shared phi_backlog counter.
    // VIC-IV adds badline stall cycles; DMA halt and backlog stall the CPU.
    // Matches VHDL gs4510.vhdl phi_pause / phi_backlog mechanism.
    auto phiBacklog = std::make_shared<int>(0);
    vic4->setStallBacklog(phiBacklog.get());

    // Scheduler: tick I/O devices after each CPU step (needed for cycle-by-cycle DMA).
    // Three stall sources: DMA halt, badline backlog, and I/O wait states.
    desc->schedulerStep = [dma, phiBacklog](MachineDescriptor& d) -> int {
        if (dma->isHaltRequested()) {
            // DMA active: tick devices (processes one DMA byte), CPU stays halted
            if (d.ioRegistry) d.ioRegistry->tickAll(1);
            return 1;
        }
        if (*phiBacklog > 0) {
            // Badline or I/O stall: CPU paused, tick devices to count down
            (*phiBacklog)--;
            if (d.ioRegistry) d.ioRegistry->tickAll(1);
            return 1;
        }
        auto* cpu = d.cpus[0].cpu;
        int cycles = cpu->step();
        if (d.ioRegistry) d.ioRegistry->tickAll(cycles);
        return cycles;
    };

    // Reset callback: reset all I/O devices, then CPU.
    // Skip HYPPO boot — go directly to the C65 KERNAL reset vector.
    // HYPPO remains loaded for runtime HDOS trap handling ($D640 writes).
    desc->onReset = [physBus](MachineDescriptor& d) {
        if (d.ioRegistry) d.ioRegistry->resetAll();
        for (auto& slot : d.cpus)
            if (slot.cpu) slot.cpu->reset();

        // The 45GS02 reset() enters hypervisor mode when HYPPO is loaded.
        // Override: leave hypervisor and start at the C65 KERNAL reset vector
        // ($FFFC/$FFFD in the ROM at physical $02FFFC/$02FFFD).
        // Don't use exitHypervisor() — it restores from zeroed hyperState,
        // corrupting SP, P, and MAP. Instead, directly clear hypervisor flag
        // and set the correct initial CPU state.
        auto* cpu = static_cast<MOS45GS02*>(d.cpus[0].cpu);
        if (cpu->isHypervisor()) {
            uint8_t lo = physBus->read8(0x02FFFC);
            uint8_t hi = physBus->read8(0x02FFFD);
            uint16_t resetVec = lo | ((uint16_t)hi << 8);

            // Clear hypervisor mode flag directly
            cpu->setHypervisorMode(false);

            // Set initial CPU state matching Cold Start spec (Step 1-2):
            cpu->regWrite(0, 0);            // A = 0
            cpu->regWrite(1, 0);            // X = 0
            cpu->regWrite(2, 0);            // Y = 0
            cpu->regWrite(3, 0);            // Z = 0
            cpu->regWrite(4, 0);            // B = 0 (direct page at $0000)
            cpu->regWrite(5, 0x01FF);       // SP = $01FF
            cpu->regWrite(6, resetVec);     // PC = reset vector
            cpu->regWrite(7, 0x24);         // P = I flag set, E flag set

            // Clear MAP state (no block mappings at reset)
            auto* mc = cpu->getMapMmu();
            if (mc) {
                MapState ms;
                memset(&ms, 0, sizeof(ms));
                mc->setMapState(ms);
            }

            // Default I/O channels
            IBus* mb = d.cpus[0].dataBus;
            mb->write8(0x9A, 0x03);    // default output = screen
            mb->write8(0x99, 0x00);    // default input = keyboard


            fprintf(stderr, "[MEGA65] Skipping HYPPO, booting C65 KERNAL at $%04X\n", resetVec);
        }
    };

    return desc;
}
