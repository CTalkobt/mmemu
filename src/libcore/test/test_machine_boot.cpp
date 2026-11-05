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
#include <vector>
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

    DeviceRegistry::instance().registerDevice("datasette",
        []() -> IOHandler* { return new Datasette(); });

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
    ASSERT_EQ(desc->nmiButtonKey, std::string("nmi"));
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

    for (const char* id : {"vic20", "c64"}) {        // RESTORE machines: no NMI button
        auto* m = MachineRegistry::instance().createMachine(id);
        ASSERT(m != nullptr);
        ASSERT(m->nmiButtonKey.empty());
        delete m;
    }
}

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
    ASSERT(pet->hasPane("tape"));
    ASSERT(!pet->hasPane("graphics-vic") && !pet->hasPane("cartridge") && !pet->hasPane("sidscope") && !pet->hasPane("drive"));
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
