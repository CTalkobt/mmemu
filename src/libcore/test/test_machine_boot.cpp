#include "plugins/devices/iec_drive/main/iec_drive.h"
#include "test_harness.h"
#include "libcore/main/machine_desc.h"
#include "libcore/main/machines/machine_registry.h"
#include "libcore/main/core_registry.h"
#include "libcore/main/json_machine_loader.h"
#include "libdebug/main/gfx_view.h"
#include "libdevices/main/device_registry.h"
#include "libdevices/main/ivideo_output.h"
#include "libmem/main/memory_bus.h"
#include "libdevices/main/ieee488.h"
#include "cpu6502.h"
#include "cpu6510.h"
#include "pia6520.h"
#include "via6522.h"
#include "crtc6545.h"
#include "pet_video.h"
#include "vic2.h"
#include "vic6560.h"
#include "c64_pla.h"
#include "cia6526.h"
#include "sid6581.h"
#include "kbd_c64.h"
#include "kbd_vic20.h"
#include "keyboard_matrix_pet.h"
#include "plugins/devices/datasette/main/datasette.h"
#include "plugins/devices/reu/main/reu.h"
#include "plugins/devices/georam/main/georam.h"
#include "plugins/devices/acia6551/main/acia6551.h"
#include "plugins/devices/mouse1351/main/mouse1351.h"
#include "libdevices/main/debug_cart.h"
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <cerrno>
#include <unistd.h>
#include <sys/stat.h>
#include <string>
#include <cstring>

// Minimal keyboard wrapper for PET tests
struct KbdPetBootTestWrapper : public IOHandler, public IKeyboardMatrix {
    explicit KbdPetBootTestWrapper(PetKeyboardMatrix::Layout l) : m_kbd(l) {}
    const char* name()     const override { return "kbd_pet"; }
    uint32_t    baseAddr() const override { return 0; }
    uint32_t    addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t)  override { return false; }
    void reset() override {}
    void tick(uint64_t) override {}
    void keyDown(int, int) override {}
    void keyUp(int, int)   override {}
    void clearKeys() override {}
    bool pressKeyByName(const std::string& n, bool d) override {
        if (!m_kbd.hasKey(n)) return false;
        if (d) m_kbd.keyDown(n); else m_kbd.keyUp(n); return true;
    }
    void enqueueText(const std::string&) override {}
    IPortDevice* getPort(int i) override { (void)i; return &m_kbd; }
    PetKeyboardMatrix m_kbd;
};

static bool s_bootTestRegistriesReady = false;
static void ensureBootTestRegistriesReady() {
    if (s_bootTestRegistriesReady) return;
    s_bootTestRegistriesReady = true;

    // Cores
    CoreRegistry::instance().registerCore("6502", "NMOS", "open",
        []() -> ICore* { return new MOS6502(); });
    CoreRegistry::instance().registerCore("6510", "MOS6510", "open",
        []() -> ICore* { return new MOS6510(); });

    // PET Devices
    DeviceRegistry::instance().registerDevice("6520",
        []() -> IOHandler* { return new PIA6520(); });
    DeviceRegistry::instance().registerDevice("6522",
        []() -> IOHandler* { return new VIA6522("VIA", 0); });
    DeviceRegistry::instance().registerDevice("6545",
        []() -> IOHandler* { return new CRTC6545(); });
    DeviceRegistry::instance().registerDevice("pet_video_2001",
        []() -> IOHandler* { return new PetVideo(PetVideo::Model::PET_2001); });
    DeviceRegistry::instance().registerDevice("pet_video_4000",
        []() -> IOHandler* { return new PetVideo(PetVideo::Model::PET_4000); });
    DeviceRegistry::instance().registerDevice("pet_video_8000",
        []() -> IOHandler* { return new PetVideo(PetVideo::Model::PET_8000); });
    DeviceRegistry::instance().registerDevice("kbd_pet_graphics",
        []() -> IOHandler* {
            return new KbdPetBootTestWrapper(PetKeyboardMatrix::Layout::GRAPHICS);
        });
    DeviceRegistry::instance().registerDevice("kbd_pet_business",
        []() -> IOHandler* {
            return new KbdPetBootTestWrapper(PetKeyboardMatrix::Layout::BUSINESS);
        });
    DeviceRegistry::instance().registerDevice("ieee488",
        []() -> IOHandler* { return new SimpleIEEE488Bus(); });

    // C64 Devices
    DeviceRegistry::instance().registerDevice("c64_pla",
        []() -> IOHandler* { return new C64PLA(); });
    DeviceRegistry::instance().registerDevice("6567",
        []() -> IOHandler* { return new VIC2("VIC-II", 0xD000); });
    DeviceRegistry::instance().registerDevice("6581",
        []() -> IOHandler* { return new SID6581("SID", 0xD400); });
    DeviceRegistry::instance().registerDevice("6526",
        []() -> IOHandler* { return new CIA6526("CIA", 0); });
    DeviceRegistry::instance().registerDevice("kbd_c64",
        []() -> IOHandler* { return new KbdC64(); });

    DeviceRegistry::instance().registerDevice("swiftlink", []() -> IOHandler* {
        auto* a = new Acia6551(0xDE00); a->setMode(Acia6551::Mode::SwiftLink); return a; });
    DeviceRegistry::instance().registerDevice("turbo232", []() -> IOHandler* {
        auto* a = new Acia6551(0xDE00); a->setMode(Acia6551::Mode::Turbo232); return a; });
    DeviceRegistry::instance().registerDevice("mouse1351", []() -> IOHandler* { return new Mouse1351(); });
    DeviceRegistry::instance().registerDevice("georam",
        []() -> IOHandler* { return new GeoRam(); });
    DeviceRegistry::instance().registerDevice("reu",
        []() -> IOHandler* { return new Reu(); });
    DeviceRegistry::instance().registerDevice("datasette",
        []() -> IOHandler* { return new Datasette(); });
    DeviceRegistry::instance().registerDevice("iec_drive",        // the C64's drive 8
        []() -> IOHandler* { return new IecDrive(8); });

    // VIC-20 Devices
    DeviceRegistry::instance().registerDevice("6560",
        []() -> IOHandler* { return new VIC6560("VIC-I", 0x9000); });
    DeviceRegistry::instance().registerDevice("kbd_vic20",
        []() -> IOHandler* {
            class KbdVic20Wrapper : public IOHandler, public IKeyboardMatrix {
            public:
                KbdVic20Wrapper() { m_kbd = new KbdVic20(); }
                ~KbdVic20Wrapper() { delete m_kbd; }
                const char* name() const override { return "kbd_vic20"; }
                uint32_t baseAddr() const override { return 0; }
                uint32_t addrMask() const override { return 0; }
                bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
                bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
                void reset() override {}
                void tick(uint64_t) override {}
                void keyDown(int r, int c) override { m_kbd->keyDown(r, c); }
                void keyUp(int r, int c) override { m_kbd->keyUp(r, c); }
                void clearKeys() override { m_kbd->clearKeys(); }
                bool pressKeyByName(const std::string& n, bool d) override { return m_kbd->pressKeyByName(n, d); }
                void enqueueText(const std::string& text) override { m_kbd->enqueueText(text); }
                IPortDevice* getPort(int i) override { return m_kbd->getPort(i); }
            private:
                KbdVic20* m_kbd;
            };
            return new KbdVic20Wrapper();
        });

    // Load JSONs
    JsonMachineLoader().loadFile("machines/pet.json");
    JsonMachineLoader().loadFile("machines/c64.json");
    JsonMachineLoader().loadFile("machines/vic20.json");
}

static void ensureRegistriesAndLoad() {
    ensureBootTestRegistriesReady();
}

/**
 * Generic machine boot verification.
 * Replicates the logic of loading a machine, resetting it, and running
 * for a specific number of cycles, then checking for video output.
 */
static void verifyMachineBoot(const std::string& machineId, uint64_t cyclesToRun) {
    ensureRegistriesAndLoad();
    
    auto* desc = MachineRegistry::instance().createMachine(machineId);
    ASSERT(desc != nullptr);
    ASSERT(desc->ioRegistry != nullptr);
    ASSERT(desc->schedulerStep != nullptr);
    ASSERT(desc->onReset != nullptr);

    // Initial reset
    desc->onReset(*desc);

    // Run scheduler
    uint64_t cyclesDone = 0;
    while (cyclesDone < cyclesToRun) {
        cyclesDone += desc->schedulerStep(*desc);
    }

    // Find Video Output
    IVideoOutput* video = nullptr;
    IOHandler* videoHandler = nullptr;
    std::vector<IOHandler*> handlers;
    desc->ioRegistry->enumerate(handlers);
    for (auto* h : handlers) {
        if (dynamic_cast<IVideoOutput*>(h)) {
            video = dynamic_cast<IVideoOutput*>(h);
            videoHandler = h;
            break;
        }
    }
    
    ASSERT(video != nullptr);

    // Diagnostic: check if Char ROM is null (using peek8 won't work, we need internal access or rely on rendering)
    // We can't easily check m_charRom from here without making it public or adding an accessor.
    // However, if we suspect it's null, we'll see it in the behavior.
    
    auto dims = video->getDimensions();
    ASSERT(dims.width > 0 && dims.height > 0);
    
    std::vector<uint32_t> frame(dims.width * dims.height);
    video->renderFrame(frame.data());
    
    // Verify frame is not just border + background (requires at least 3 unique colors if border exists)
    // or not just a solid block (requires 2 unique colors if no border).
    std::vector<uint32_t> uniqueColors;
    for (uint32_t p : frame) {
        bool found = false;
        for (uint32_t c : uniqueColors) {
            if (c == p) { found = true; break; }
        }
        if (!found) {
            uniqueColors.push_back(p);
            if (uniqueColors.size() > 3) break; // Optimization: we found enough
        }
    }
    
    bool hasBorder = (dims.displayWidth < dims.width) || (dims.displayHeight < dims.height);
    bool foundContent = false;
    
    // Check interior display area for unique colors
    std::vector<uint32_t> interiorColors;
    int yStart = 40; // Start near top of display area
    int yEnd   = dims.height - 40;
    for (int y = yStart; y < yEnd; y += 2) {
        for (int x = 40; x < (int)dims.width - 40; x += 2) {
            uint32_t p = frame[y * dims.width + x];
            bool found = false;
            for (uint32_t c : interiorColors) { if (c == p) { found = true; break; } }
            if (!found) {
                interiorColors.push_back(p);
                if (interiorColors.size() >= 2) break;
            }
        }
        if (interiorColors.size() >= 2) break;
    }

    if (hasBorder) {
        // C64: Interior should have at least 2 colors (BG + Text)
        foundContent = (interiorColors.size() >= 2);
    } else {
        // PET: Full frame should have at least 2 colors (BG + Text)
        foundContent = (uniqueColors.size() >= 2);
    }
    
    if (!foundContent) {
        fprintf(stderr, "Boot verification failed for %s: unique colors in center: ", machineId.c_str());
        for (uint32_t c : interiorColors) fprintf(stderr, "%08X ", c);
        fprintf(stderr, " (total unique in frame %zu) after %lu cycles.\n", uniqueColors.size(), cyclesDone);
        
        // Dump some CPU state
        if (!desc->cpus.empty()) {
            ICore* cpu = desc->cpus[0].cpu;
            fprintf(stderr, "CPU PC: $%04X, Cycles: %lu\n", cpu->pc(), cpu->cycles());
        }
        // Dump VRAM snippet
        if (machineId == "c64") {
            fprintf(stderr, "VRAM at $0400: \n");
            for (int r=0; r<4; ++r) {
                fprintf(stderr, "  %04X: ", 0x0400 + r*32);
                for (int i=0; i<32; ++i) fprintf(stderr, "%02X ", desc->buses[0].bus->peek8(0x0400 + r*32 + i));
                fprintf(stderr, "\n");
            }
            fprintf(stderr, "Color RAM at $D800: \n");
            for (int r=0; r<2; ++r) {
                fprintf(stderr, "  %04X: ", 0xD800 + r*32);
                for (int i=0; i<32; ++i) fprintf(stderr, "%02X ", desc->buses[0].bus->peek8(0xD800 + r*32 + i));
                fprintf(stderr, "\n");
            }
        } else if (machineId.find("vic20") != std::string::npos) {
            fprintf(stderr, "VRAM at $1E00: \n");
            for (int r=0; r<4; ++r) {
                fprintf(stderr, "  %04X: ", 0x1E00 + r*22);
                for (int i=0; i<22; ++i) fprintf(stderr, "%02X ", desc->buses[0].bus->peek8(0x1E00 + r*22 + i));
                fprintf(stderr, "\n");
            }
        } else {
            fprintf(stderr, "VRAM at $8000: ");
            for (int i=0; i<16; ++i) fprintf(stderr, "%02X ", desc->buses[0].bus->peek8(0x8000+i));
            fprintf(stderr, "\n");
        }
    }
    ASSERT(foundContent);

    delete desc;
}

TEST_CASE(pet2001_boot_screen) {
    // 5 seconds @ 1MHz should be more than enough for PET boot
    verifyMachineBoot("pet2001", 5000000);
}

TEST_CASE(vic20_boot_screen) {
    // 5 seconds is ample for VIC-20 boot
    verifyMachineBoot("vic20", 5000000);
}

TEST_CASE(c64_boot_screen) {
    // 10 seconds is ample for C64 Kernal to initialize and display message
    verifyMachineBoot("c64", 10000000);
}

// The C64's joysticks on CIA1 (control port 1 on port B with the keyboard
// rows, control port 2 on port A with the columns): a joystick pulls lines
// low, also the column lines the CIA drives as outputs.
TEST_CASE(c64_joysticks_on_cia1) {
    ensureRegistriesAndLoad();
    auto* desc = MachineRegistry::instance().createMachine("c64");
    ASSERT(desc != nullptr);
    ASSERT(desc->onJoystick != nullptr);
    ASSERT_EQ(desc->joystickPorts, 2);
    ASSERT_EQ(desc->primaryJoystickPort, 1);
    desc->onReset(*desc);
    for (uint64_t c = 0; c < 3000000;) c += desc->schedulerStep(*desc);

    CIA6526* cia1 = nullptr;
    std::vector<IOHandler*> handlers;
    desc->ioRegistry->enumerate(handlers);
    for (auto* h : handlers) {
        auto* c = dynamic_cast<CIA6526*>(h);
        if (c && c->baseAddr() == 0xDC00) cia1 = c;
    }
    ASSERT(cia1 != nullptr);
    auto rd = [cia1](uint32_t a) { uint8_t v = 0; cia1->ioPeek(nullptr, a, &v); return (int)v; };
    ASSERT_EQ(rd(0xDC02), 0xFF);                    // KERNAL: port A all outputs
    int idleA = rd(0xDC00), idleB = rd(0xDC01);
    ASSERT_EQ(idleB, 0xFF);                         // no key, no joystick

    desc->onJoystick(1, (uint8_t)~(0x01 | 0x10) & 0x1F);   // port 2: up + fire
    ASSERT_EQ(rd(0xDC00), idleA & ~0x11);
    ASSERT_EQ(rd(0xDC01), 0xFF);
    desc->onJoystick(1, 0x1F);
    desc->onJoystick(0, (uint8_t)~0x04 & 0x1F);     // port 1: left
    ASSERT_EQ(rd(0xDC00), idleA);
    ASSERT_EQ(rd(0xDC01), 0xFB);
    delete desc;
}

// The VIC-20's one joystick, as in VICE: up, down, left and fire on VIA1 port
// A (PA2-PA5), right on VIA2 PB7, which it shares with keyboard column 7.
TEST_CASE(vic20_joystick_on_via1_and_via2) {
    ensureRegistriesAndLoad();
    auto* desc = MachineRegistry::instance().createMachine("vic20");
    ASSERT(desc != nullptr);
    ASSERT_EQ(desc->joystickPorts, 1);
    desc->onReset(*desc);
    for (uint64_t c = 0; c < 3000000;) c += desc->schedulerStep(*desc);

    VIA6522* via1 = nullptr;
    VIA6522* via2 = nullptr;
    std::vector<IOHandler*> handlers;
    desc->ioRegistry->enumerate(handlers);
    for (auto* h : handlers) {
        auto* v = dynamic_cast<VIA6522*>(h);
        if (v && v->baseAddr() == 0x9110) via1 = v;
        if (v && v->baseAddr() == 0x9120) via2 = v;
    }
    ASSERT(via1 && via2);
    auto rd = [](VIA6522* v, uint32_t a) { uint8_t x = 0; v->ioPeek(nullptr, a, &x); return (int)x; };
    ASSERT_EQ(rd(via1, 0x9113) & 0x3C, 0);          // KERNAL: PA2-PA5 are inputs
    via2->ioWrite(nullptr, 0x9122, 0x7F);           // as games do: POKE 37154,127
    ASSERT_EQ(rd(via1, 0x911F) & 0x3C, 0x3C);       // idle
    ASSERT_EQ(rd(via2, 0x9120) & 0x80, 0x80);

    desc->onJoystick(0, (uint8_t)~(0x01 | 0x10) & 0x1F);   // up + fire
    ASSERT_EQ(rd(via1, 0x911F) & 0x3C, 0x3C & ~0x04 & ~0x20);
    ASSERT_EQ(rd(via2, 0x9120) & 0x80, 0x80);
    desc->onJoystick(0, (uint8_t)~(0x02 | 0x04 | 0x08) & 0x1F);   // down + left + right
    ASSERT_EQ(rd(via1, 0x911F) & 0x3C, 0x3C & ~0x08 & ~0x10);
    ASSERT_EQ(rd(via2, 0x9120) & 0x80, 0);
    desc->onJoystick(0, 0x1F);
    ASSERT_EQ(rd(via2, 0x9120) & 0x80, 0x80);
    via2->ioWrite(nullptr, 0x9122, 0xFF);
    delete desc;
}

// The VIC-20's tape lines, as in VICE and the KERNAL: sense on VIA1 PA6
// ($911F bit 6), motor on VIA1 CA2 (PCR $911C), write on VIA2 PB3, read
// pulses on VIA2 CA1 (IFR $912D bit 1).
TEST_CASE(vic20_tape_lines) {
    ensureRegistriesAndLoad();
    auto* desc = MachineRegistry::instance().createMachine("vic20");
    ASSERT(desc != nullptr);
    desc->onReset(*desc);
    for (uint64_t c = 0; c < 2000000;) c += desc->schedulerStep(*desc);

    VIA6522* via1 = nullptr;
    VIA6522* via2 = nullptr;
    IOHandler* tape = nullptr;
    std::vector<IOHandler*> handlers;
    desc->ioRegistry->enumerate(handlers);
    for (auto* h : handlers) {
        auto* v = dynamic_cast<VIA6522*>(h);
        if (v && v->baseAddr() == 0x9110) via1 = v;
        if (v && v->baseAddr() == 0x9120) via2 = v;
        if (std::string(h->name()) == "Tape") tape = h;
    }
    ASSERT(via1 && via2 && tape);
    auto rd = [](VIA6522* v, uint32_t a) { uint8_t x = 0; v->ioPeek(nullptr, a, &x); return (int)x; };
    auto state = [tape](const std::string& key) {
        DeviceInfo info; tape->getDeviceInfo(info);
        for (auto& s : info.state) if (s.first == key) return s.second;
        return std::string();
    };

    ASSERT_EQ(rd(via1, 0x911F) & 0x40, 0x40);       // no button pressed
    tape->controlTape("record");                    // RECORD + PLAY
    ASSERT_EQ(rd(via1, 0x911F) & 0x40, 0);          // sense low
    // The KERNAL's IRQ starts the motor while a button is held: CA2 low
    for (uint64_t c = 0; c < 100000;) c += desc->schedulerStep(*desc);
    ASSERT_EQ(rd(via1, 0x911C) & 0x0E, 0x0C);
    ASSERT_EQ(state("Motor On"), std::string("yes"));
    tape->controlTape("stoprecord");
    ASSERT_EQ(rd(via1, 0x911F) & 0x40, 0x40);
    for (uint64_t c = 0; c < 100000;) c += desc->schedulerStep(*desc);
    ASSERT_EQ(state("Motor On"), std::string("no"));

    // Read pulses arrive on VIA2 CA1
    ISignalLine* ca1 = via2->getSignalLine("ca1");
    ASSERT(ca1 != nullptr);
    via2->ioWrite(nullptr, 0x912C, rd(via2, 0x912C) & ~0x01);  // negative edge
    uint8_t dummy; via2->ioRead(nullptr, 0x9121, &dummy);      // clear CA1
    ASSERT_EQ(rd(via2, 0x912D) & 0x02, 0);
    ca1->set(false); ca1->set(true);
    ASSERT_EQ(rd(via2, 0x912D) & 0x02, 0x02);
    delete desc;
}

// RESTORE reaches the CPU's NMI: on the VIC-20 through VIA1 CA1 (VIA1's
// interrupt output is NMI, VIA2's is IRQ), on the C64 straight to NMI. The
// KERNAL's NMI handler jumps through $0318; point it at a counter.
static void restoreGivesNmi(const std::string& id) {
    ensureRegistriesAndLoad();
    auto* desc = MachineRegistry::instance().createMachine(id);
    ASSERT(desc != nullptr && !desc->cpus.empty());
    desc->onReset(*desc);
    for (uint64_t c = 0; c < 3000000;) c += desc->schedulerStep(*desc);
    IBus* bus = desc->cpus[0].dataBus;
    // $02A1 (free in both): INC $02A0 ; JMP $FEBC/$FF56 (KERNAL "restore registers, RTI")
    uint16_t exitNmi = id == "c64" ? 0xFEBC : 0xFF56;
    const uint8_t code[] = {0xEE, 0xA0, 0x02, 0x4C, (uint8_t)exitNmi, (uint8_t)(exitNmi >> 8)};
    bus->write8(0x02A0, 0);
    for (size_t i = 0; i < sizeof code; i++) bus->write8(0x02A1 + (uint32_t)i, code[i]);
    bus->write8(0x0318, 0xA1);
    bus->write8(0x0319, 0x02);

    for (uint64_t c = 0; c < 100000;) c += desc->schedulerStep(*desc);
    ASSERT_EQ((int)bus->peek8(0x02A0), 0);          // the jiffy IRQ is not an NMI
    ASSERT(desc->onKey("restore", true));
    for (uint64_t c = 0; c < 20000;) c += desc->schedulerStep(*desc);
    ASSERT(desc->onKey("restore", false));
    for (uint64_t c = 0; c < 100000;) c += desc->schedulerStep(*desc);
    ASSERT_EQ((int)bus->peek8(0x02A0), 1);          // one press, one NMI
    delete desc;
}

TEST_CASE(vic20_restore_is_nmi) { restoreGivesNmi("vic20"); }
TEST_CASE(c64_restore_is_nmi)   { restoreGivesNmi("c64"); }

// The PET had no RESTORE key; its NMI line is reachable only through an
// added button, modelled as the "nmi" key. The ROM's NMI handler jumps
// through $0094 (BASIC's warm start by default); point it at a counter.
TEST_CASE(pet_nmi_button) {
    ensureRegistriesAndLoad();
    auto* desc = MachineRegistry::instance().createMachine("pet4032");
    ASSERT(desc != nullptr && !desc->cpus.empty());
    ASSERT_EQ((int)desc->screenButtons.size(), 1);  // "Trigger NMI", not a RESTORE key
    ASSERT_EQ(desc->screenButtons[0].label, std::string("Trigger NMI"));
    ASSERT(desc->screenButtons[0].keys == std::vector<std::string>{"nmi"});
    ASSERT(!desc->onKey("restore", true));          // no RESTORE key on a PET
    desc->onReset(*desc);
    for (uint64_t c = 0; c < 3000000;) c += desc->schedulerStep(*desc);
    IBus* bus = desc->cpus[0].dataBus;
    const uint8_t code[] = {0xEE, 0x3A, 0x03, 0x40};       // $033B: INC $033A ; RTI
    bus->write8(0x033A, 0);
    for (size_t i = 0; i < sizeof code; i++) bus->write8(0x033B + (uint32_t)i, code[i]);
    bus->write8(0x0094, 0x3B);
    bus->write8(0x0095, 0x03);
    for (uint64_t c = 0; c < 100000;) c += desc->schedulerStep(*desc);
    ASSERT_EQ((int)bus->peek8(0x033A), 0);
    ASSERT(desc->onKey("nmi", true));
    for (uint64_t c = 0; c < 20000;) c += desc->schedulerStep(*desc);
    ASSERT(desc->onKey("nmi", false));
    for (uint64_t c = 0; c < 100000;) c += desc->schedulerStep(*desc);
    ASSERT_EQ((int)bus->peek8(0x033A), 1);          // one press, one NMI
    delete desc;

}

// The C64 and VIC-20 screen buttons: RUN/STOP + RESTORE and RESTORE. Holding
// the first button's keys gives the KERNAL's warm start, which clears the
// screen (a character poked at the top left goes).
static void runStopRestoreButton(const std::string& id, uint16_t screen) {
    ensureRegistriesAndLoad();
    auto* desc = MachineRegistry::instance().createMachine(id);
    ASSERT(desc != nullptr && !desc->cpus.empty());
    ASSERT_EQ((int)desc->screenButtons.size(), 2);
    const auto& rsr = desc->screenButtons[0];
    ASSERT_EQ(rsr.label, std::string("RUN/STOP + RESTORE"));
    ASSERT(rsr.keys == (std::vector<std::string>{"run_stop", "restore"}));
    ASSERT_EQ(desc->screenButtons[1].label, std::string("RESTORE"));
    desc->onReset(*desc);
    for (uint64_t c = 0; c < 3000000;) c += desc->schedulerStep(*desc);
    IBus* bus = desc->cpus[0].dataBus;
    bus->write8(screen, 0x18);                      // 'X' at the top left
    for (uint64_t c = 0; c < 50000;) c += desc->schedulerStep(*desc);
    ASSERT_EQ((int)bus->peek8(screen), 0x18);
    for (const auto& k : rsr.keys) ASSERT(desc->onKey(k, true));     // as the button does
    for (uint64_t c = 0; c < 150000;) c += desc->schedulerStep(*desc);
    for (auto it = rsr.keys.rbegin(); it != rsr.keys.rend(); ++it) ASSERT(desc->onKey(*it, false));
    for (uint64_t c = 0; c < 500000;) c += desc->schedulerStep(*desc);
    ASSERT_EQ((int)bus->peek8(screen), 0x20);       // warm start: screen cleared
    delete desc;
}

TEST_CASE(c64_run_stop_restore_button)   { runStopRestoreButton("c64", 0x0400); }
TEST_CASE(vic20_run_stop_restore_button) { runStopRestoreButton("vic20", 0x1E00); }

// Each machine declares its hardware panes and VIC graphics formats
// ("ui" in the machine JSON); gfx::machineFormats follows them.
TEST_CASE(machine_ui_capabilities) {
    ensureRegistriesAndLoad();
    auto ids = [](const MachineDescriptor* m) {
        std::string s;
        for (const auto* f : gfx::machineFormats(m)) s += std::string(s.empty() ? "" : ",") + f->id;
        return s;
    };
    auto* pet = MachineRegistry::instance().createMachine("pet4032");
    ASSERT(pet && pet->uiPanesSet);
    ASSERT(pet->hasPane("tape") && pet->hasPane("drive"));         // the IEEE-488 drive
    ASSERT(!pet->hasPane("graphics-vic") && !pet->hasPane("cartridge") && !pet->hasPane("sidscope"));
    ASSERT_EQ(ids(pet), std::string(""));           // no VIC graphics at all
    delete pet;

    auto* vic = MachineRegistry::instance().createMachine("vic20+8k");
    ASSERT(vic && vic->hasPane("graphics-vic") && vic->hasPane("cartridge") && !vic->hasPane("sidscope"));
    ASSERT_EQ(ids(vic), std::string("char,char-mc,char16,char16-mc"));   // VIC-I: no sprites
    ASSERT(!gfx::machineHasFormat(vic, gfx::Format::SpriteHires));
    delete vic;

    auto* c64 = MachineRegistry::instance().createMachine("c64");
    ASSERT(c64 && c64->hasPane("sidscope") && c64->hasPane("drive") && !c64->hasPane("mega65"));
    ASSERT_EQ(ids(c64), std::string("sprite,sprite-mc,char,char-mc"));    // VIC-II
    delete c64;

    MachineDescriptor undeclared;                   // a machine that says nothing: every pane,
    ASSERT(undeclared.hasPane("sidscope"));         // and the VIC formats
    int vicFormats = 0;
    for (const auto& f : gfx::formats()) vicFormats += std::string(f.family) == "vic";
    ASSERT(vicFormats > 0 && vicFormats < (int)gfx::formats().size());
    ASSERT_EQ((int)gfx::machineFormats(&undeclared).size(), vicFormats);
    ASSERT_EQ((int)gfx::machineFormats(nullptr).size(), vicFormats);
    ASSERT_EQ((int)gfx::machineFormats(nullptr, "antic").size(), 0);
}

// The PET's cassette #1, as VICE wires it (petpia1.c, petvia.c): sense on
// PIA1 PA4, motor on PIA1 CB2 (low = on), write on VIA PB3, read on PIA1 CA1
TEST_CASE(pet_tape_lines) {
    ensureRegistriesAndLoad();
    auto* desc = MachineRegistry::instance().createMachine("pet4032");
    ASSERT(desc != nullptr);
    desc->onReset(*desc);
    for (uint64_t c = 0; c < 2000000;) c += desc->schedulerStep(*desc);
    IOHandler* pia1 = nullptr;
    IOHandler* tape = nullptr;
    std::vector<IOHandler*> handlers;
    desc->ioRegistry->enumerate(handlers);
    for (auto* h : handlers) {
        if (h->baseAddr() == 0xE810 && dynamic_cast<PIA6520*>(h)) pia1 = h;
        if (std::string(h->name()) == "Tape") tape = h;
    }
    ASSERT(pia1 && tape);
    auto rd = [pia1](uint32_t a) { uint8_t x = 0; pia1->ioPeek(nullptr, a, &x); return (int)x; };
    auto state = [tape](const std::string& key) {
        DeviceInfo info; tape->getDeviceInfo(info);
        for (auto& s : info.state) if (s.first == key) return s.second;
        return std::string();
    };
    ASSERT_EQ(rd(0xE810) & 0x10, 0x10);            // no button
    tape->controlTape("record");
    ASSERT_EQ(rd(0xE810) & 0x10, 0);                // sense low
    uint8_t crb = (uint8_t)rd(0xE813);
    pia1->ioWrite(nullptr, 0xE813, (uint8_t)((crb & ~0x38) | 0x30));   // CB2 output low
    for (uint64_t c = 0; c < 1000;) c += desc->schedulerStep(*desc);
    ASSERT_EQ(state("Motor On"), std::string("yes"));
    pia1->ioWrite(nullptr, 0xE813, (uint8_t)((crb & ~0x38) | 0x38));   // CB2 output high
    for (uint64_t c = 0; c < 1000;) c += desc->schedulerStep(*desc);
    ASSERT_EQ(state("Motor On"), std::string("no"));
    tape->controlTape("stoprecord");
    ASSERT_EQ(rd(0xE810) & 0x10, 0x10);
    delete desc;
}

// An REU in the C64's expansion port, fitted at run time: the CPU waits
// while it holds the bus; it copies the screen to its bank 3 and back to
// $C400 on the $FF00 trigger (a write under the KERNAL); a fixed C64
// address reaches the VIC-II's border register; then it comes out again
TEST_CASE(c64_reu_expansion) {
    ensureRegistriesAndLoad();
    auto* desc = MachineRegistry::instance().createMachine("c64");
    ASSERT(desc != nullptr);
    auto* slot = desc->expansionSlot("port");
    ASSERT(slot != nullptr);
    EXPECT_EQ(slot->info(), std::string(""));
    std::string err;
    EXPECT_TRUE(!slot->install("reu", "3M", err));
    EXPECT_TRUE(!slot->install("no-such-device", "", err));
    ASSERT(slot->install("reu", "1M", err));
    EXPECT_EQ(slot->info(), std::string("reu (REU, 1 MB)"));
    desc->onReset(*desc);
    for (uint64_t c = 0; c < 2500000;) c += desc->schedulerStep(*desc);
    auto* reu = dynamic_cast<Reu*>(desc->ioRegistry->findHandler("REU"));
    ASSERT(reu != nullptr);
    IBus* bus = desc->buses[0].bus;
    ICore* cpu = desc->cpus[0].cpu;
    auto regs = [&](uint16_t host, uint32_t reuAddr, uint16_t len) {
        bus->write8(0xDF02, (uint8_t)host); bus->write8(0xDF03, (uint8_t)(host >> 8));
        bus->write8(0xDF04, (uint8_t)reuAddr); bus->write8(0xDF05, (uint8_t)(reuAddr >> 8));
        bus->write8(0xDF06, (uint8_t)(reuAddr >> 16));
        bus->write8(0xDF07, (uint8_t)len); bus->write8(0xDF08, (uint8_t)(len >> 8));
    };
    // The screen to bank 3: 1000 cycles with the CPU stopped, more when the
    // VIC-II's badlines take the bus from the REU too (#266)
    regs(0x0400, 0x030000, 1000);
    bus->write8(0xDF01, 0x90);
    const uint32_t pc = cpu->pc();
    bool held = true;
    int steps = 0;
    for (; steps < 3000 && reu->dmaActive(); ++steps) held = held && desc->schedulerStep(*desc) == 1 && cpu->pc() == pc;
    EXPECT_TRUE(held);
    EXPECT_TRUE(steps >= 1000);
    EXPECT_TRUE(!reu->dmaActive());
    bool same = true;
    for (int i = 0; i < 1000; ++i) same = same && reu->ram()[0x30000 + (size_t)i] == bus->peek8(0x0400 + (uint32_t)i);
    EXPECT_TRUE(same);
    EXPECT_EQ(bus->peek8(0xDF00) & 0x40, 0x40);     // end of block
    // Back to $C400, armed for $FF00: nothing until that write
    regs(0xC400, 0x030000, 1000);
    bus->write8(0xDF01, 0x81);
    desc->schedulerStep(*desc);
    EXPECT_TRUE(!reu->dmaActive());
    bus->write8(0xFF00, 0x00);                      // RAM under the KERNAL; the REU sees it
    EXPECT_TRUE(reu->dmaActive());
    for (int i = 0; i < 2000 && reu->dmaActive(); ++i) desc->schedulerStep(*desc);
    same = true;
    for (int i = 0; i < 1000; ++i) same = same && bus->peek8(0xC400 + (uint32_t)i) == reu->ram()[0x30000 + (size_t)i];
    EXPECT_TRUE(same);
    // DMA sees I/O: a fixed C64 address on the border colour
    reu->ram()[0x10] = 0x05;
    regs(0xD020, 0x000010, 1);
    bus->write8(0xDF0A, 0x80);
    bus->write8(0xDF01, 0x91);
    desc->schedulerStep(*desc);
    EXPECT_EQ(bus->peek8(0xD020) & 0x0F, 0x05);
    // Out again: $DF00 is no longer the REU's
    ASSERT(slot->install("", "", err));
    EXPECT_EQ(slot->info(), std::string(""));
    EXPECT_TRUE(desc->ioRegistry->findHandler("REU") == nullptr);
    EXPECT_TRUE(!desc->busMasterStep);
    delete desc;
}

// The CPU drives the REU: a program copies 40 bytes of the screen with the
// end-of-block IRQ on; the KERNAL's IRQ vector reaches its handler, which
// reads (and so acknowledges) the status
TEST_CASE(c64_reu_irq_program) {
    ensureRegistriesAndLoad();
    auto* desc = MachineRegistry::instance().createMachine("c64");
    ASSERT(desc != nullptr);
    std::string err;
    ASSERT(desc->expansionSlot("port")->install("reu", "256K", err));
    desc->onReset(*desc);
    for (uint64_t c = 0; c < 2500000;) c += desc->schedulerStep(*desc);
    IBus* bus = desc->buses[0].bus;
    static const uint8_t PROGRAM[] = {0x78, 0xA9, 0x7F, 0x8D, 0x0D, 0xDC, 0xAD, 0x0D, 0xDC, 0xA9, 0x00, 0x8D, 0x1A, 0xD0, 0x85, 0x02, 0xA9, 0x60, 0x8D, 0x14, 0x03, 0xA9, 0xC0, 0x8D, 0x15, 0x03, 0xA9, 0x00, 0x8D, 0x02, 0xDF, 0xA9, 0x04, 0x8D, 0x03, 0xDF, 0xA9, 0x00, 0x8D, 0x04, 0xDF, 0x8D, 0x05, 0xDF, 0x8D, 0x06, 0xDF, 0xA9, 0x28, 0x8D, 0x07, 0xDF, 0xA9, 0x00, 0x8D, 0x08, 0xDF, 0xA9, 0xC0, 0x8D, 0x09, 0xDF, 0xA9, 0x90, 0x8D, 0x01, 0xDF, 0x58, 0xA5, 0x02, 0xF0, 0xFC, 0x4C, 0x48, 0xC0, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xAD, 0x00, 0xDF, 0x85, 0x03, 0xE6, 0x02, 0x4C, 0x81, 0xEA};
    for (size_t i = 0; i < sizeof PROGRAM; ++i) bus->write8(0xC000 + (uint32_t)i, PROGRAM[i]);
    desc->cpus[0].cpu->setPc(0xC000);
    for (uint64_t c = 0; c < 20000;) c += desc->schedulerStep(*desc);
    EXPECT_EQ(bus->peek8(0x02), 1);                 // the handler ran once
    EXPECT_EQ(bus->peek8(0x03), 0xD0);              // IRQ, end of block, 256K chips
    EXPECT_EQ(bus->peek8(0xDF00), 0x10);            // and that read cleared them
    auto* reu = dynamic_cast<Reu*>(desc->ioRegistry->findHandler("REU"));
    ASSERT(reu != nullptr);
    bool same = true;
    for (int i = 0; i < 40; ++i) same = same && reu->ram()[(size_t)i] == bus->peek8(0x0400 + (uint32_t)i);
    EXPECT_TRUE(same);
    delete desc;
}

// A GeoRAM in the C64's port: the CPU pages it through $DE00 with $DFFE/$DFFF;
// BASIC's RAM and the REU's slot sharing stay out of each other's way
TEST_CASE(c64_georam_expansion) {
    ensureRegistriesAndLoad();
    auto* desc = MachineRegistry::instance().createMachine("c64");
    ASSERT(desc != nullptr);
    auto* slot = desc->expansionSlot("port");
    std::string err;
    EXPECT_TRUE(!slot->install("georam", "128K", err));
    ASSERT(slot->install("georam", "1M", err));
    EXPECT_EQ(slot->info(), std::string("georam (GeoRAM, 1 MB)"));
    desc->onReset(*desc);
    for (uint64_t c = 0; c < 2500000;) c += desc->schedulerStep(*desc);
    IBus* bus = desc->buses[0].bus;
    bus->write8(0xDFFF, 40);                        // block 40, page 17
    bus->write8(0xDFFE, 17);
    bus->write8(0xDE00, 0x12);
    bus->write8(0xDEFF, 0x34);
    bus->write8(0xDFFE, 18);
    EXPECT_TRUE(bus->read8(0xDE00) != 0x12 || bus->read8(0xDEFF) != 0x34);
    bus->write8(0xDFFE, 17);
    EXPECT_EQ(bus->read8(0xDE00), 0x12);
    EXPECT_EQ(bus->read8(0xDEFF), 0x34);
    auto* g = dynamic_cast<GeoRam*>(desc->ioRegistry->findHandler("GeoRAM"));
    ASSERT(g != nullptr);
    EXPECT_EQ(g->ram()[40 * 16384 + 17 * 256], 0x12);
    EXPECT_TRUE(!desc->busMasterStep);              // no DMA
    // Swapped for an REU: $DE00 is no longer RAM
    ASSERT(slot->install("reu", "512K", err));
    EXPECT_TRUE(desc->ioRegistry->findHandler("GeoRAM") == nullptr);
    EXPECT_TRUE(desc->ioRegistry->findHandler("REU") != nullptr);
    delete desc;
}

// The VIC-20's RAM expansions fitted at run time: BASIC's start-up message
// counts what the KERNAL found (3583 unexpanded, 6655 with 3K, 11775 with
// 8K, 19967 with 16K, 28159 with 24K or more; BLK5 isn't BASIC's)
TEST_CASE(vic20_ram_expansion_slot) {
    ensureRegistriesAndLoad();
    auto* desc = MachineRegistry::instance().createMachine("vic20");
    ASSERT(desc != nullptr);
    auto* slot = desc->expansionSlot("ram");
    ASSERT(slot != nullptr);
    EXPECT_EQ(slot->info(), std::string(""));
    IBus* bus = desc->buses[0].bus;
    // The screen is at $1E00 unexpanded and $1000 with 8K or more: look in both
    auto bytesFree = [&](const char* want) {
        std::string text;
        for (uint32_t a = 0x1000; a < 0x2000; ++a) {
            uint8_t c = bus->peek8(a) & 0x7F;
            text += (char)(c >= 1 && c <= 26 ? 'A' + c - 1 : c);
        }
        return text.find(std::string(want) + " BYTES FREE") != std::string::npos;
    };
    auto boot = [&]() {
        desc->onReset(*desc);
        for (uint64_t c = 0; c < 4000000;) c += desc->schedulerStep(*desc);
    };
    boot();
    EXPECT_TRUE(bytesFree("3583"));
    std::string err;
    const std::pair<const char*, const char*> cases[] = {
        {"3K", "6655"}, {"8K", "11775"}, {"16K", "19967"}, {"24K", "28159"}, {"35K", "28159"}, {"1,2", "19967"}};
    for (const auto& c : cases) {
        ASSERT(slot->install("ram", c.first, err));
        boot();
        if (!bytesFree(c.second)) printf("  %s: no '%s BYTES FREE'\n", c.first, c.second);
        EXPECT_TRUE(bytesFree(c.second));
    }
    EXPECT_EQ(slot->info(), std::string("ram (16K: BLK1, BLK2)"));
    ASSERT(slot->install("ram", "blk5", err));
    EXPECT_EQ(slot->info(), std::string("ram (8K: BLK5)"));
    bus->write8(0xA000, 0x42);
    EXPECT_EQ(bus->read8(0xA000), 0x42);            // RAM at $A000
    EXPECT_TRUE(!slot->install("ram", "64K", err));
    EXPECT_TRUE(!slot->install("ram", "4", err));   // no BLK4
    EXPECT_TRUE(!slot->install("reu", "", err));
    ASSERT(slot->install("", "", err));             // all out again
    EXPECT_EQ(slot->info(), std::string(""));
    EXPECT_EQ(bus->read8(0xA000), 0xFF);            // open bus
    delete desc;
    // A profile starts with its blocks fitted
    auto* v32 = MachineRegistry::instance().createMachine("vic20+32k");
    ASSERT(v32 != nullptr);
    EXPECT_EQ(v32->expansionSlot("ram")->info(), std::string("ram (32K: BLK1, BLK2, BLK3, BLK5)"));
    delete v32;
}

// A SwiftLink on the C64 with the Hayes modem as its far end: the 6510 sends
// "AT" (PETSCII) and its NMI handler collects the echo and "OK", one NMI a
// byte (reading the status releases /NMI for the next edge)
TEST_CASE(c64_swiftlink_modem_nmi) {
    ensureRegistriesAndLoad();
    auto* desc = MachineRegistry::instance().createMachine("c64");
    ASSERT(desc != nullptr);
    auto* slot = desc->expansionSlot("rs232");
    ASSERT(slot != nullptr);
    std::string err;
    EXPECT_TRUE(!slot->install("swiftlink", "DE80", err));
    ASSERT(slot->install("swiftlink", "", err));
    EXPECT_EQ(slot->info(), std::string("swiftlink (SwiftLink, DE00)"));
    ASSERT(desc->expansionSlot("port")->install("reu", "", err));   // beside an REU, as through an expander
    desc->onReset(*desc);
    for (uint64_t c = 0; c < 2500000;) c += desc->schedulerStep(*desc);
    auto* acia = dynamic_cast<Acia6551*>(desc->ioRegistry->findHandler("SwiftLink"));
    ASSERT(acia != nullptr);
    ASSERT(acia->attachSerial("modem"));
    IBus* bus = desc->buses[0].bus;
    static const uint8_t LOOP[] = {0x4C, 0x00, 0xC0};
    static const uint8_t NMI[] = {0x48, 0xAD, 0x01, 0xDE, 0xAD, 0x00, 0xDE, 0xA6, 0x02, 0x9D, 0x00, 0xC1, 0xE6, 0x02, 0x68, 0x40};
    for (size_t i = 0; i < sizeof LOOP; ++i) bus->write8(0xC000 + (uint32_t)i, LOOP[i]);
    for (size_t i = 0; i < sizeof NMI; ++i) bus->write8(0xC010 + (uint32_t)i, NMI[i]);
    bus->write8(0x02, 0);
    bus->write8(0x0318, 0x10); bus->write8(0x0319, 0xC0);
    desc->cpus[0].cpu->setPc(0xC000);
    bus->write8(0xDE03, 0x1F);                      // 38400 bps on a SwiftLink
    bus->write8(0xDE02, 0x09);                      // DTR, receive IRQs, transmitter on
    auto run = [&](uint64_t n) { for (uint64_t c = 0; c < n;) c += desc->schedulerStep(*desc); };
    for (uint8_t b : {0x41, 0x54, 0x0D}) { bus->write8(0xDE00, b); run(600); }
    run(30000);
    const int n = bus->peek8(0x02);
    std::string got;
    for (int i = 0; i < n; ++i) got += (char)bus->peek8(0xC100 + (uint32_t)i);
    EXPECT_TRUE(n >= 7);
    EXPECT_TRUE(got.find("\x41\x54\x0D") == 0);    // the echo
    EXPECT_TRUE(got.find("\x0D\xCF\xCB\x0D") != std::string::npos);   // "OK" in PETSCII
    delete desc;
}

// A second SID fitted at run time at $D420: a sawtooth on its voice 1 is
// heard through the first SID's output (the frontends play one device);
// the first SID stays silent; taken out, the tone goes
TEST_CASE(c64_second_sid) {
    ensureRegistriesAndLoad();
    auto* desc = MachineRegistry::instance().createMachine("c64");
    ASSERT(desc != nullptr);
    auto* slot = desc->expansionSlot("sid2");
    ASSERT(slot != nullptr);
    std::string err;
    EXPECT_TRUE(!slot->install("sid", "D000", err));   // the VIC's
    EXPECT_TRUE(!slot->install("sid", "D430", err));   // not on a 32-byte boundary
    ASSERT(slot->install("sid", "", err));
    EXPECT_EQ(slot->info(), std::string("sid (SID2, D420)"));
    desc->onReset(*desc);
    for (uint64_t c = 0; c < 2500000;) c += desc->schedulerStep(*desc);
    IBus* bus = desc->buses[0].bus;
    auto* sid = dynamic_cast<SID6581*>(desc->ioRegistry->findHandler("SID"));
    ASSERT(sid != nullptr);
    auto level = [&](double seconds) {
        std::vector<float> drain(65536);
        while (sid->pullSamples(drain.data(), (int)drain.size()) > 0) {}
        for (uint64_t c = 0; c < (uint64_t)(985248 * seconds);) c += desc->schedulerStep(*desc);
        std::vector<float> buf(16384);
        const int n = sid->pullSamples(buf.data(), (int)buf.size());
        float lo = 1, hi = -1;
        for (int i = 0; i < n; ++i) { lo = std::min(lo, buf[i]); hi = std::max(hi, buf[i]); }
        return n > 0 ? hi - lo : 0.0f;
    };
    const float quiet = level(0.05);
    // SID2 voice 1: 440 Hz-ish sawtooth, full sustain, volume 15
    bus->write8(0xD420, 0x00); bus->write8(0xD421, 0x1D);
    bus->write8(0xD425, 0x00); bus->write8(0xD426, 0xF0);
    bus->write8(0xD438, 0x0F);
    bus->write8(0xD424, 0x21);
    const float loud = level(0.1);
    if (!(loud > quiet + 0.05f)) printf("  quiet %.3f loud %.3f\n", quiet, loud);
    EXPECT_TRUE(loud > quiet + 0.05f);
    ASSERT(slot->install("", "", err));
    EXPECT_TRUE(desc->ioRegistry->findHandler("SID2") == nullptr);
    const float after = level(0.1);
    EXPECT_TRUE(after < loud * 0.5f);
    delete desc;
}

// A 1351 mouse in control port 1, fitted at run time: the host mouse's
// motion shows on the SID's POTX/POTY (position mod 64 in bits 1-6, up
// raising Y), its left button as port 1's fire (CIA1 $DC01 bit 4), the
// right as up (bit 0); taken out, the pots read $FF again
TEST_CASE(c64_1351_mouse) {
    ensureRegistriesAndLoad();
    auto* desc = MachineRegistry::instance().createMachine("c64");
    ASSERT(desc != nullptr);
    auto* slot = desc->expansionSlot("control1");
    ASSERT(slot != nullptr);
    IBus* bus = desc->buses[0].bus;
    desc->onReset(*desc);
    for (uint64_t c = 0; c < 2500000;) c += desc->schedulerStep(*desc);
    EXPECT_EQ(bus->read8(0xD419), 0xFF);            // nothing on the pots
    EXPECT_TRUE(!desc->onMouse);
    std::string err;
    ASSERT(slot->install("1351", "", err));
    EXPECT_EQ(slot->info(), std::string("1351 (1351 Mouse)"));
    ASSERT(desc->onMouse != nullptr);
    desc->onMouse(10, -3, 0, 0);                    // right 10, up 3
    EXPECT_EQ(bus->read8(0xD419), 10 << 1);
    EXPECT_EQ(bus->read8(0xD41A), 3 << 1);
    desc->onMouse(60, 0, 0, 0);                     // 70: wraps at 64
    EXPECT_EQ(bus->read8(0xD419), 6 << 1);
    // The buttons through CIA1 port B (keyboard columns released: $DC00 = $FF)
    bus->write8(0xDC02, 0xFF); bus->write8(0xDC00, 0xFF); bus->write8(0xDC03, 0x00);
    EXPECT_EQ(bus->read8(0xDC01) & 0x11, 0x11);
    desc->onMouse(0, 0, 1, 0);                      // left
    EXPECT_EQ(bus->read8(0xDC01) & 0x11, 0x01);
    desc->onMouse(0, 0, 2, 0);                      // right
    EXPECT_EQ(bus->read8(0xDC01) & 0x11, 0x10);
    ASSERT(slot->install("", "", err));
    EXPECT_TRUE(!desc->onMouse);
    EXPECT_EQ(bus->read8(0xD419), 0xFF);
    EXPECT_EQ(bus->read8(0xDC01) & 0x11, 0x11);     // buttons let go
    delete desc;
}

// ---------------------------------------------------------------------------
// The VIC-II takes the bus (#266): BA after VICE's viciisc cycle tables; the
// 6510's reads wait while it's low, its writes don't
// ---------------------------------------------------------------------------

// BA by cycle: a badline's cycles 12-54; each sprite's from 3 cycles before
// its fetches; eight sprites on a line, 19 cycles (Bauer's article, §3.8)
TEST_CASE(vic2_ba_cycles) {
    VIC2 v;
    v.ioWrite(nullptr, 0xD011, 0x1B);               // DEN, YSCROLL 3
    v.tick((uint64_t)0x31 * 63);                    // past line $30: badlines allowed
    EXPECT_TRUE(v.isBadline(0x33));
    EXPECT_TRUE(!v.isBadline(0x34));
    EXPECT_TRUE(!v.isBadline(0xFB));
    int n = 0;
    for (int c = 0; c < 63; ++c) n += v.baAt(0x33, c);
    EXPECT_EQ(n, 43);
    EXPECT_TRUE(!v.baAt(0x33, 10) && v.baAt(0x33, 11) && v.baAt(0x33, 53) && !v.baAt(0x33, 54));
    // Sprite 0 at Y=100 alone: cycles 55-59 of lines 100-120
    v.ioWrite(nullptr, 0xD001, 100);
    v.ioWrite(nullptr, 0xD015, 0x01);
    EXPECT_TRUE(!v.baAt(99, 54));
    EXPECT_TRUE(v.baAt(100, 54) && v.baAt(100, 58) && !v.baAt(100, 59));
    EXPECT_TRUE(v.baAt(120, 54) && !v.baAt(121, 54));
    v.ioWrite(nullptr, 0xD017, 0x01);               // expanded: 42 lines
    EXPECT_TRUE(v.baAt(141, 54) && !v.baAt(142, 54));
    // All eight on one line: cycles 55-63 and 1-10 of the next
    for (int i = 0; i < 8; ++i) v.ioWrite(nullptr, 0xD001 + 2 * (uint32_t)i, 100);
    v.ioWrite(nullptr, 0xD015, 0xFF);
    v.ioWrite(nullptr, 0xD017, 0x00);
    n = 0;
    for (int c = 54; c < 63; ++c) n += v.baAt(101, c);
    for (int c = 0; c < 11; ++c) n += v.baAt(102, c);
    EXPECT_EQ(n, 19);
    // NTSC: sprite 0's BA on cycles 56-60 of 65
    v.setPal(false);
    EXPECT_TRUE(!v.baAt(101, 54) && v.baAt(101, 55) && v.baAt(101, 59));
}

namespace {
// A C64 running `loop` at $C000 with interrupts off; the cycles the VIC-II
// takes from it over one frame: the scheduler's cycles less the CPU's own
int stolenPerFrame(MachineDescriptor* desc, const std::vector<uint8_t>& loop, uint8_t d011, uint8_t sprites, int spriteY) {
    IBus* bus = desc->buses[0].bus;
    ICore* cpu = desc->cpus[0].cpu;
    auto* vic = dynamic_cast<VIC2*>(desc->ioRegistry->findHandler("VIC-II"));
    if (!vic) return -1;
    static const uint8_t SETUP[] = {0x78, 0xA9, 0x7F, 0x8D, 0x0D, 0xDC, 0x8D, 0x0D, 0xDD, 0xAD, 0x0D, 0xDC,
                                    0xA9, 0x00, 0x8D, 0x1A, 0xD0};    // SEI; CIAs' and the VIC's IRQs off
    uint16_t a = 0xC000;
    for (uint8_t b : SETUP) bus->write8(a++, b);
    const uint16_t loopAt = a;
    for (uint8_t b : loop) bus->write8(a++, b);
    bus->write8(a++, 0x4C); bus->write8(a++, (uint8_t)loopAt); bus->write8(a++, (uint8_t)(loopAt >> 8));
    for (int i = 0; i < 8; ++i) bus->write8(0xD001 + 2 * (uint32_t)i, (uint8_t)spriteY);
    bus->write8(0xD015, sprites);
    bus->write8(0xD017, 0);
    bus->write8(0xD011, d011);
    cpu->setPc(0xC000);
    // A frame to settle, then from line 0 to line 0
    auto toLine0 = [&]() {
        while (vic->rasterLine() == 0) desc->schedulerStep(*desc);
        while (vic->rasterLine() != 0) desc->schedulerStep(*desc);
    };
    toLine0();
    toLine0();
    const uint64_t cpu0 = cpu->cycles();
    uint64_t total = 0;
    while (vic->rasterLine() == 0) total += (uint64_t)desc->schedulerStep(*desc);
    while (vic->rasterLine() != 0) total += (uint64_t)desc->schedulerStep(*desc);
    return (int)(total - (cpu->cycles() - cpu0));
}
}  // namespace

// Badlines on the C64: 25 a frame with the screen on, none with it off; a
// loop of reads loses all 43 BA cycles of each, one that writes at the end
// (INC) between 40 and 43
TEST_CASE(c64_badlines_steal_cycles) {
    ensureRegistriesAndLoad();
    auto* desc = MachineRegistry::instance().createMachine("c64");
    ASSERT(desc != nullptr);
    desc->onReset(*desc);
    for (uint64_t c = 0; c < 2500000;) c += desc->schedulerStep(*desc);
    const std::vector<uint8_t> nops = {0xEA, 0xEA};
    EXPECT_EQ(stolenPerFrame(desc, nops, 0x1B, 0, 0), 25 * 43);
    EXPECT_EQ(stolenPerFrame(desc, nops, 0x0B, 0, 0), 0);          // DEN off
    const int inc = stolenPerFrame(desc, {0xEE, 0x00, 0x03}, 0x1B, 0, 0);   // INC $0300
    EXPECT_TRUE(inc >= 25 * 40 && inc <= 25 * 43);
    EXPECT_TRUE(inc < 25 * 43);
    delete desc;
}

// Sprite DMA on the C64: eight sprites on a line take 19 cycles of each of
// their 21 lines; one alone, 5
TEST_CASE(c64_sprite_dma_steals_cycles) {
    ensureRegistriesAndLoad();
    auto* desc = MachineRegistry::instance().createMachine("c64");
    ASSERT(desc != nullptr);
    desc->onReset(*desc);
    for (uint64_t c = 0; c < 2500000;) c += desc->schedulerStep(*desc);
    const std::vector<uint8_t> nops = {0xEA, 0xEA};
    EXPECT_EQ(stolenPerFrame(desc, nops, 0x0B, 0xFF, 100), 21 * 19);
    EXPECT_EQ(stolenPerFrame(desc, nops, 0x0B, 0x01, 100), 21 * 5);
    delete desc;
}

// An REU transfer across badlines waits for them: the same copy takes 1000
// cycles with the screen off and about 40 more for each badline it meets
TEST_CASE(c64_reu_waits_for_badlines) {
    ensureRegistriesAndLoad();
    auto* desc = MachineRegistry::instance().createMachine("c64");
    ASSERT(desc != nullptr);
    std::string err;
    ASSERT(desc->expansionSlot("port")->install("reu", "512K", err));
    desc->onReset(*desc);
    for (uint64_t c = 0; c < 2500000;) c += desc->schedulerStep(*desc);
    IBus* bus = desc->buses[0].bus;
    auto* reu = dynamic_cast<Reu*>(desc->ioRegistry->findHandler("REU"));
    auto* vic = dynamic_cast<VIC2*>(desc->ioRegistry->findHandler("VIC-II"));
    ASSERT(reu && vic);
    auto copy = [&](uint8_t d011) {
        bus->write8(0xD011, d011);
        // (DEN counts on line $30: a frame for it to take effect) then from
        // line $40, badlines ahead
        while (vic->rasterLine() != 0) desc->schedulerStep(*desc);
        while (vic->rasterLine() != 0x40) desc->schedulerStep(*desc);
        bus->write8(0xDF02, 0x00); bus->write8(0xDF03, 0x04);
        bus->write8(0xDF04, 0); bus->write8(0xDF05, 0); bus->write8(0xDF06, 0);
        bus->write8(0xDF07, 0xE8); bus->write8(0xDF08, 0x03);   // 1000 bytes
        bus->write8(0xDF01, 0x90);
        int cycles = 0;
        while (reu->dmaActive()) cycles += desc->schedulerStep(*desc);
        return cycles;
    };
    EXPECT_EQ(copy(0x0B), 1000);
    // 1000 cycles span about 16 lines: two badlines, 43 cycles each
    const int on = copy(0x1B);
    EXPECT_TRUE(on >= 1000 + 2 * 40 && on <= 1000 + 3 * 43);
    delete desc;
}

namespace {
// How many times "READY." is on the C64's screen
int readyCount(IBus* bus) {
    static const uint8_t READY[] = {18, 5, 1, 4, 25, 46};   // screen codes
    int n = 0;
    for (uint16_t a = 0x0400; a + 6 <= 0x07E8; ++a) {
        bool m = true;
        for (int i = 0; i < 6 && m; ++i) m = (bus->peek8((uint16_t)(a + i)) & 0x7F) == READY[i];
        n += m;
    }
    return n;
}

// Type `text` through the KERNAL's keyboard buffer, 10 keys at a time
void typeKeys(MachineDescriptor* desc, IBus* bus, const std::string& text, uint64_t& cycles) {
    for (size_t at = 0; at < text.size();) {
        while (bus->peek8(0xC6) != 0) cycles += (uint64_t)desc->schedulerStep(*desc);
        const size_t n = std::min<size_t>(10, text.size() - at);
        for (size_t i = 0; i < n; ++i) bus->write8((uint16_t)(0x0277 + i), (uint8_t)text[at + i]);
        bus->write8(0xC6, (uint8_t)n);
        at += n;
    }
}

// One of VICE's "exitcode" test programs on a PAL C64, as VICE's testbench
// runs it: power on, autostart the PRG (injected, then RUN), and wait for the
// debug cart at $D7FF. The exit code, or -1 if none came within `limit`
// cycles of power on.
// `expansion` is "" or an expansion port device and its size ("reu", "512K").
// `disk` is a disk image for drive 8 (the list's "mountd64:") or a folder
// it serves: the PRG is then loaded from it through the KERNAL,
// LOAD"<NAME>",8,1 and RUN, as VICE's disk autostart does. `loadName` is the
// file's name there (default: the PRG's, without its extension, in capitals).
int runViceExitcodeTest(const std::string& prgPath, uint64_t limit,
                        const std::string& expansion = "", const std::string& size = "",
                        bool newCia = false, const std::string& disk = "",
                        const std::string& loadName = "") {
    std::ifstream f(prgPath, std::ios::binary);
    if (!f) return -2;
    std::vector<uint8_t> prg((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (prg.size() < 3) return -2;
    auto* desc = MachineRegistry::instance().createMachine("c64");
    if (!desc) return -3;
    std::string err;
    const bool reu = expansion == "reu";
    if (!expansion.empty() && !desc->expansionSlot("port")->install(expansion, size, err)) { delete desc; return -3; }
    // REU/reutiming2's references come from a C64C, but for the -m2 programs'
    // (a breadbin); Denise's test runner picks the board the same way
    if (reu && prgPath.find("reutiming2") != std::string::npos && prgPath.find("m2.") == std::string::npos) {
        IOHandler* r = desc->ioRegistry->findHandler("REU");
        if (!r || !r->setOption("glue", "c64c", err)) { delete desc; return -3; }
    }
    if (newCia) {                          // the 6526A (C64C)
        for (const char* n : {"CIA1", "CIA2"}) {
            IOHandler* c = desc->ioRegistry->findHandler(n);
            if (!c || !c->setOption("model", "6526A", err)) { delete desc; return -3; }
        }
    }
    if (!disk.empty() && !desc->ioRegistry->mountDisk(8, disk)) { delete desc; return -2; }
    DebugCart cart;
    desc->ioRegistry->registerHandler(&cart);
    desc->onReset(*desc);
    IBus* bus = desc->buses[0].bus;
    uint64_t cycles = 0;
    while (cycles < 2500000) cycles += (uint64_t)desc->schedulerStep(*desc);   // to READY.
    if (!disk.empty()) {
        std::string name = loadName;
        if (name.empty()) {
            name = prgPath.substr(prgPath.find_last_of('/') + 1);
            name = name.substr(0, name.find_last_of('.'));
            for (char& c : name) c = (char)toupper((unsigned char)c);
        }
        const int ready = readyCount(bus);
        typeKeys(desc, bus, "LOAD\"" + name + "\",8,1\r", cycles);
        while (readyCount(bus) <= ready && cycles < limit) cycles += (uint64_t)desc->schedulerStep(*desc);
        typeKeys(desc, bus, "RUN\r", cycles);
    } else {
    const uint16_t load = (uint16_t)(prg[0] | (prg[1] << 8));
    const uint16_t end = (uint16_t)(load + prg.size() - 2);
    for (size_t i = 2; i < prg.size(); ++i) bus->write8((uint16_t)(load + i - 2), prg[i]);
    for (uint16_t p : {0x2D, 0x2F, 0x31, 0xAE}) {        // VARTAB, ARYTAB, STREND, the load end
        bus->write8(p, (uint8_t)end);
        bus->write8(p + 1, (uint8_t)(end >> 8));
    }
    const char* run = "RUN\r";
    for (int i = 0; i < 4; ++i) bus->write8(0x0277 + i, (uint8_t)run[i]);
    bus->write8(0xC6, 4);
    }
    while (!cart.written() && cycles < limit) cycles += (uint64_t)desc->schedulerStep(*desc);
    const int code = cart.written() ? cart.code() : -1;
    if (std::getenv("MMEMU_VICE_RUN")) printf("  %llu cycles\n", (unsigned long long)cycles);
    if (code != 0 && std::getenv("MMEMU_VICE_SCREEN")) {      // what a failing program printed
        for (int r = 0; r < 25; ++r) {
            std::string line;
            for (int c = 0; c < 40; ++c) {
                const uint8_t v = bus->peek8((uint16_t)(0x0400 + r * 40 + c)) & 0x7F;
                line += v == 0 ? '@' : v < 27 ? (char)('A' + v - 1) : v >= 32 && v < 64 ? (char)v : '.';
            }
            if (line.find_first_not_of(' ') != std::string::npos) printf("    | %s\n", line.c_str());
        }
    }
    if (code != 0 || std::getenv("MMEMU_VICE_RUN")) {
        if (const char* dump = std::getenv("MMEMU_VICE_DUMP")) {   // the 64K as the CPU sees it, for diffing
            std::ofstream out(dump, std::ios::binary);
            for (uint32_t a = 0; a < 0x10000; ++a) out.put((char)bus->peek8(a));
        }
    }
    desc->ioRegistry->unregisterHandler(&cart);
    delete desc;
    return code;
}
}  // namespace

// The C64's drive 8 on CIA2 port A (#348): a bit the DDR leaves as an input
// floats high, so its 7406 pulls the line low; outputs at 0 release them
TEST_CASE(c64_iec_port_undriven_bits_assert_lines) {
    IecDrive d(8);
    d.setClockHz(985248);
    d.setDdr(0x00);
    ASSERT(!d.clkLine() && !d.dataLine() && !d.atnLine());
    d.setDdr(0x3F);
    d.writePort(0x00);
    ASSERT(d.clkLine() && d.dataLine() && d.atnLine());
    d.writePort(0x10);                              // CLK out
    ASSERT(!d.clkLine() && d.dataLine());
    ASSERT_EQ((int)(d.readPort() & 0xC0), 0x80);    // CLK reads low, DATA high
}

// A KERNAL LOAD over the serial bus from drive 8 serving a host folder (#348):
// "NAME" finds name.prg, as VICE's file-system drive does
TEST_CASE(c64_kernal_load_from_drive8_folder) {
    ensureRegistriesAndLoad();
    const std::string dir = "/tmp/mmemu_test_drive8_" + std::to_string((long)getpid());
    ASSERT(::mkdir(dir.c_str(), 0700) == 0 || errno == EEXIST);
    {
        std::ofstream f(dir + "/hello.prg", std::ios::binary);
        const uint8_t prg[] = {0x00, 0xC0, 0xDE, 0xAD, 0xBE, 0xEF};    // $C000: DE AD BE EF
        f.write((const char*)prg, sizeof prg);
    }
    auto* desc = MachineRegistry::instance().createMachine("c64");
    ASSERT(desc != nullptr);
    ASSERT(desc->ioRegistry->mountDisk(8, dir) != nullptr);
    desc->onReset(*desc);
    IBus* bus = desc->buses[0].bus;
    uint64_t cycles = 0;
    while (cycles < 2500000) cycles += (uint64_t)desc->schedulerStep(*desc);
    const int ready = readyCount(bus);
    typeKeys(desc, bus, "LOAD\"HELLO\",8,1\r", cycles);
    while (readyCount(bus) <= ready && cycles < 20000000) cycles += (uint64_t)desc->schedulerStep(*desc);
    EXPECT_EQ(bus->peek8(0xC000), 0xDE);
    EXPECT_EQ(bus->peek8(0xC003), 0xEF);
    EXPECT_EQ(bus->peek8(0xAE) | (bus->peek8(0xAF) << 8), 0xC004);   // the load's end
    delete desc;
    std::remove((dir + "/hello.prg").c_str());
    ::rmdir(dir.c_str());
}

// VICE's exit-code test programs (#304), from VICE's testprogs tree: set
// MMEMU_VICE_TESTPROGS to its root (with testbench/c64-testlist.in) and,
// optionally, MMEMU_VICE_FILTER to a part of the program paths to run. Only
// PAL programs without other hardware than an REU. Skipped without the tree.
TEST_CASE(c64_vice_testprogs) {
    const char* root = std::getenv("MMEMU_VICE_TESTPROGS");
    if (!root) return;
    const char* filter = std::getenv("MMEMU_VICE_FILTER");
    // MMEMU_VICE_SHARD=k/n: only every n-th program from the k-th (0..n-1), to
    // run the list in n processes
    int shard = 0, shards = 1, index = -1;
    if (const char* sh = std::getenv("MMEMU_VICE_SHARD")) {
        if (sscanf(sh, "%d/%d", &shard, &shards) != 2 || shards < 1) shard = 0, shards = 1;
    }
    ensureRegistriesAndLoad();
    // MMEMU_VICE_RUN=<prg>[,<cycles>]: just that program (a path under the
    // root), e.g. a "record" program whose results MMEMU_VICE_DUMP keeps
    if (const char* one = std::getenv("MMEMU_VICE_RUN")) {
        // MMEMU_VICE_RUN=<prg>[,<cycles>[,<option>...]]: options as the list's
        // (reu512k, geo512k, cia-new)
        std::vector<std::string> f;
        std::stringstream ss(one);
        for (std::string t; std::getline(ss, t, ',');) f.push_back(t);
        const std::string prg = f.empty() ? "" : f[0];
        const uint64_t limit = f.size() > 1 ? std::stoull(f[1]) : 100000000;
        std::string expansion, size, disk;
        bool newCia = false;
        for (size_t i = 2; i < f.size(); ++i) {
            const bool isReu = f[i].rfind("reu", 0) == 0, isGeo = f[i].rfind("geo", 0) == 0;
            if ((isReu || isGeo) && f[i].size() > 4 && isdigit((unsigned char)f[i][3])) {
                expansion = isReu ? "reu" : "georam";
                size = f[i].substr(3);
                for (char& c : size) c = (char)toupper((unsigned char)c);
            } else if (f[i] == "cia-new") newCia = true;
            else if (f[i].rfind("mountd64:", 0) == 0)
                disk = std::string(root) + "/" + prg.substr(0, prg.find_last_of('/') + 1) + f[i].substr(9);
        }
        const int code = runViceExitcodeTest(std::string(root) + "/" + prg, limit, expansion, size, newCia, disk);
        printf("  vice %-60s %s (%d)\n", prg.c_str(), code == 0 ? "pass" : "FAIL", code);
        return;
    }
    std::ifstream list(std::string(root) + "/testbench/c64-testlist.in");
    ASSERT(list.good());
    std::string line;
    int passed = 0, failed = 0;
    while (std::getline(list, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> f;
        std::stringstream ss(line);
        for (std::string s; std::getline(ss, s, ',');) f.push_back(s);
        if (f.size() < 4 || f[2] != "exitcode") continue;
        bool ok = true, newCia = false;
        std::string expansion, size, diskFile, loadName;
        for (size_t i = 4; i < f.size(); ++i) {
            // "reu512k", "reu16m", "geo512k": the expansion and its size
            const bool isReu = f[i].rfind("reu", 0) == 0, isGeo = f[i].rfind("geo", 0) == 0;
            if ((isReu || isGeo) && f[i].size() > 4 && isdigit((unsigned char)f[i][3])) {
                expansion = isReu ? "reu" : "georam";
                size = f[i].substr(3);
                for (char& c : size) c = (char)toupper((unsigned char)c);
            }
            else if (f[i] == "cia-new") newCia = true;
            else if (f[i].rfind("mountd64:", 0) == 0) diskFile = f[i].substr(9);
            else if (f[i].rfind("comment:load the test using \"", 0) == 0) {
                // A program that must be LOADed by name (C64/autostart/defaults
                // checks what the KERNAL's load leaves behind): from the disk,
                // or else from its folder on drive 8
                const size_t q = f[i].find('"') + 1;
                loadName = f[i].substr(q, f[i].find('"', q) - q);
            }
            else if (f[i] == "vicii-pal" || f[i] == "cia-old" || f[i] == "sid-old" ||
                     f[i].rfind("warn:", 0) == 0 || f[i].rfind("comment:", 0) == 0) {}
            else ok = false;               // NTSC, other carts, drives, CIA models...
        }
        // The list's directory may lack its "/" ("../general/fuxxortest,...")
        if (!f[0].empty() && f[0].back() != '/') f[0] += '/';
        const std::string path = std::string(root) + "/testbench/" + f[0] + f[1];
        if (!ok || (filter && path.find(filter) == std::string::npos)) continue;
        if (++index % shards != shard) continue;
        const std::string dir = std::string(root) + "/testbench/" + f[0];
        const std::string disk = !diskFile.empty() ? dir + diskFile : !loadName.empty() ? dir : "";
        const int code = runViceExitcodeTest(path, std::stoull(f[3]), expansion, size, newCia, disk, loadName);
        if (code == 0) ++passed; else ++failed;
        printf("  vice %-60s %s (%d)\n", (f[0] + f[1] + (diskFile.empty() ? "" : " +" + diskFile)).c_str(),
               code == 0 ? "pass" : "FAIL", code);
    }
    printf("  vice testprogs: %d passed, %d failed\n", passed, failed);
    EXPECT_EQ(failed, 0);
}
