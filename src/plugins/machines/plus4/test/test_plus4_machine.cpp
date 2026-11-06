#include "test_harness.h"
#include "plugins/machines/plus4/main/machine_plus4.h"
#include "libcore/main/machine_desc.h"
#include "libdevices/main/io_registry.h"
#include "libdevices/main/ikeyboard_matrix.h"
#include "libdevices/main/device_info.h"
#include "libmem/main/ibus.h"
#include "libdebug/main/gfx_view.h"
#include "libdevices/main/ivideo_inspect.h"
#include <set>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <string>
#include <vector>

namespace {
// Screen codes of a text at the Plus/4's screen ($0C00, 40 x 25)
bool screenHas(MachineDescriptor* m, const std::string& text) {
    IBus* bus = m->buses[0].bus;
    std::vector<uint8_t> want;
    for (char c : text) want.push_back((uint8_t)(c >= 'A' && c <= 'Z' ? c - 'A' + 1 : c));
    for (uint32_t a = 0x0C00; a + want.size() <= 0x0C00 + 1000; ++a) {
        bool ok = true;
        for (size_t i = 0; i < want.size() && ok; ++i) ok = bus->peek8(a + (uint32_t)i) == want[i];
        if (ok) return true;
    }
    return false;
}
void run(MachineDescriptor* m, uint64_t ticks) {
    for (uint64_t t = 0; t < ticks;) t += (uint64_t)m->schedulerStep(*m);
}
IKeyboardMatrix* keyboard(MachineDescriptor* m) {
    std::vector<IOHandler*> hs;
    m->ioRegistry->enumerate(hs);
    for (auto* h : hs) if (auto* k = dynamic_cast<IKeyboardMatrix*>(h)) return k;
    return nullptr;
}
}

// The Plus/4 boots BASIC 3.5 to READY.
TEST_CASE(plus4_boots_to_ready) {
    std::string err;
    MachineDescriptor* m = plus4::create(plus4::Model::Plus4, false, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    m->onReset(*m);
    run(m, 6000000);                                // ~3.4 s of TED clock
    ASSERT(screenHas(m, "COMMODORE BASIC V3.5"));
    ASSERT(screenHas(m, "READY."));
    delete m;
}

// The C16 boots with 16 KB and runs a typed line
TEST_CASE(c16_boots_and_runs_basic) {
    std::string err;
    MachineDescriptor* m = plus4::create(plus4::Model::C16, false, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    m->onReset(*m);
    run(m, 6000000);
    ASSERT(screenHas(m, "READY."));
    ASSERT(screenHas(m, "12277 BYTES FREE"));       // C16: 16 KB
    keyboard(m)->enqueueText("PRINT 6*7\n");
    run(m, 4000000);
    ASSERT(screenHas(m, " 42"));
    delete m;
}

// SAVE, LOAD and FILE NOT FOUND through the KERNAL's serial bus code to the
// IEC drive on unit 8 (a host folder)
TEST_CASE(plus4_iec_drive_save_load) {
    std::string err;
    MachineDescriptor* m = plus4::create(plus4::Model::C16, false, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    char dir[] = "/tmp/mmemu-iec-XXXXXX";
    ASSERT(mkdtemp(dir) != nullptr);
    std::vector<IOHandler*> hs;
    m->ioRegistry->enumerate(hs);
    bool mounted = false;
    for (auto* h : hs) mounted = mounted || h->mountDisk(8, dir);
    ASSERT(mounted);

    m->onReset(*m);
    run(m, 6000000);
    keyboard(m)->enqueueText("10 PRINT 40+3\nSAVE\"T\",8\n");
    run(m, 12000000);
    std::string path = std::string(dir) + "/T";
    FILE* f = fopen(path.c_str(), "rb");
    ASSERT(f != nullptr);
    uint8_t hdr[2] = {0, 0};
    ASSERT(fread(hdr, 1, 2, f) == 2);
    fclose(f);
    ASSERT(hdr[0] == 0x01 && hdr[1] == 0x10);       // BASIC at $1001

    keyboard(m)->enqueueText("NEW\nLOAD\"T\",8\nRUN\n");
    run(m, 16000000);
    ASSERT(screenHas(m, " 43"));

    keyboard(m)->enqueueText("LOAD\"NOPE\",8\n");
    run(m, 10000000);
    ASSERT(screenHas(m, "FILE NOT FOUND"));

    remove(path.c_str());
    rmdir(dir);
    delete m;
}

// The cassette port: the sense switch on $FD10 bit 2 and the motor on $01 bit 3
TEST_CASE(plus4_tape_port_lines) {
    std::string err;
    MachineDescriptor* m = plus4::create(plus4::Model::Plus4, false, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    m->onReset(*m);
    IBus* bus = m->buses[0].bus;
    IOHandler* tape = m->ioRegistry->findHandler("Tape");
    ASSERT(tape != nullptr);
    bus->write8(0xFD10, 0xFF);
    ASSERT(bus->read8(0xFD10) & 0x04);                       // no button down
    tape->controlTape("play");
    ASSERT(!(bus->read8(0xFD10) & 0x04));                    // PLAY: sense low
    auto motorOn = [&]() {
        DeviceInfo info;
        tape->getDeviceInfo(info);
        for (auto& kv : info.state) if (kv.first == "Motor On") return kv.second == "yes";
        return false;
    };
    bus->write8(0x00, 0x0F);
    bus->write8(0x01, 0x08);                                 // bit 3 high: motor off
    tape->tick(1);
    ASSERT(!motorOn());
    bus->write8(0x01, 0x00);                                 // low: motor on
    tape->tick(1);
    ASSERT(motorOn());
    delete m;
}

// RS-232 through the KERNAL: the 6551 at $FD00, looped back (the C16 has none)
TEST_CASE(plus4_acia_kernal_loopback) {
    std::string err;
    MachineDescriptor* m = plus4::create(plus4::Model::Plus4, false, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    IOHandler* acia = m->ioRegistry->findHandler("ACIA");
    ASSERT(acia != nullptr);
    ASSERT(acia->attachSerial("loopback"));
    m->onReset(*m);
    run(m, 6000000);
    // 1200 bps 8N1, DTR and transmit IRQs (the KERNAL sends from its IRQ)
    keyboard(m)->enqueueText("10 OPEN2,2,0,CHR$(8)+CHR$(5):PRINT#2,\"HE\"+\"LLO\";\n"
                             "20 FORI=1TO300:NEXT\n"
                             "30 GET#2,A$:IFA$<>\"\"THENPRINTA$;:GOTO30\n"
                             "40 CLOSE2\nRUN\n");
    run(m, 30000000);
    ASSERT(screenHas(m, "HELLO"));
    delete m;

    m = plus4::create(plus4::Model::C16, false, &err);
    ASSERT(m != nullptr);
    ASSERT(m->ioRegistry->findHandler("ACIA") == nullptr);
    delete m;
}

namespace {
std::vector<uint8_t> romFile(const char* path) {
    std::vector<uint8_t> d;
    if (FILE* f = fopen(path, "rb")) {
        int c;
        while ((c = fgetc(f)) != EOF) d.push_back((uint8_t)c);
        fclose(f);
    }
    return d;
}
}

// Banking (plus4mem.c): $FF3E/$FF3F switch the upper 32 KB between ROM and
// RAM, writes under ROM go to RAM, $FDD0-$FDDF pick the low (A0-A1) and high
// (A2-A3) ROMs, $FC00-$FCFF is always the KERNAL, and $FF20-$FF3D (and
// $FF3E/$FF3F with ROM selected) are the RAM or ROM underneath, not the TED
TEST_CASE(plus4_rom_ram_banking) {
    std::string err;
    MachineDescriptor* m = plus4::create(plus4::Model::Plus4, false, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    auto basic  = romFile("roms/plus4/basic.318006-01.bin");
    auto kernal = romFile("roms/plus4/kernal.318004-05.bin");
    auto funcLo = romFile("roms/plus4/3-plus-1.317053-01.bin");
    auto funcHi = romFile("roms/plus4/3-plus-1.317054-01.bin");
    ASSERT(basic.size() == 0x4000 && kernal.size() == 0x4000);
    ASSERT(funcLo.size() == 0x4000 && funcHi.size() == 0x4000);
    m->onReset(*m);
    IBus* bus = m->buses[0].bus;

    // After reset: BASIC low, KERNAL high
    bus->write8(0xFF3E, 0);
    bus->write8(0xFDD0, 0);
    ASSERT(bus->read8(0x8000) == basic[0] && bus->read8(0xBFFF) == basic[0x3FFF]);
    ASSERT(bus->read8(0xC000) == kernal[0] && bus->read8(0xFFFC) == kernal[0x3FFC]);

    // Writes under ROM reach RAM; $FF3F shows it, $FF3E hides it again
    uint8_t v = (uint8_t)~basic[0x0123];
    bus->write8(0x8123, v);
    ASSERT(bus->read8(0x8123) == basic[0x0123]);
    bus->write8(0xFF3F, 0);
    ASSERT(bus->read8(0x8123) == v);
    bus->write8(0xFF3E, 0);
    ASSERT(bus->read8(0x8123) == basic[0x0123]);

    // $FDD5: function ROM low and high; $FC00-$FCFF stays the KERNAL
    bus->write8(0xFDD5, 0);
    ASSERT(bus->read8(0x8000) == funcLo[0] && bus->read8(0xA000) == funcLo[0x2000]);
    ASSERT(bus->read8(0xC000) == funcHi[0] && bus->read8(0xD000) == funcHi[0x1000]);
    ASSERT(bus->read8(0xFC00) == kernal[0x3C00] && bus->read8(0xFCFF) == kernal[0x3CFF]);
    bus->write8(0xFDD0, 0);
    ASSERT(bus->read8(0x8000) == basic[0]);

    // $FF20-$FF3D: ROM underneath with ROM on, RAM with RAM on
    ASSERT(bus->read8(0xFF20) == kernal[0x3F20]);
    ASSERT(bus->read8(0xFF3E) == kernal[0x3F3E]);
    bus->write8(0xFF30, 0x5A);                               // to RAM
    ASSERT(bus->read8(0xFF30) == kernal[0x3F30]);
    bus->write8(0xFF3F, 0);
    ASSERT(bus->read8(0xFF30) == 0x5A);
    ASSERT(bus->read8(0xFF3E) == 0xFF && bus->read8(0xFF3F) == 0xFF);
    // ... and $FF00-$FF1F stay the TED either way
    bus->write8(0xFF15, 0x23);                               // background colour
    ASSERT((bus->read8(0xFF15) & 0x7F) == 0x23);
    ASSERT(!(bus->read8(0xFF13) & 0x01));                    // ROM status bit: RAM
    bus->write8(0xFF3E, 0);
    ASSERT(bus->read8(0xFF13) & 0x01);
    delete m;
}

// The C16's 16 KB repeats through the 64 KB map
TEST_CASE(c16_ram_mirrors) {
    std::string err;
    MachineDescriptor* m = plus4::create(plus4::Model::C16, false, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    m->onReset(*m);
    IBus* bus = m->buses[0].bus;
    bus->write8(0xFF3F, 0);                                  // RAM in the upper half
    bus->write8(0x2345, 0xA5);
    ASSERT(bus->read8(0x6345) == 0xA5);
    ASSERT(bus->read8(0xA345) == 0xA5 && bus->read8(0xE345) == 0xA5);
    delete m;
}

// The C116 is a C16 in a smaller case: 16 KB, no ACIA, boots to READY
TEST_CASE(c116_boots_as_c16) {
    std::string err;
    MachineDescriptor* m = plus4::create(plus4::Model::C116, false, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    ASSERT(m->machineId == "c116");
    ASSERT(m->displayName == "Commodore 116");
    ASSERT(m->ioRegistry->findHandler("ACIA") == nullptr);
    m->onReset(*m);
    run(m, 6000000);
    ASSERT(screenHas(m, "12277 BYTES FREE"));
    ASSERT(screenHas(m, "READY."));
    delete m;
}

// The graphics viewer (#208): the Plus/4 offers the TED formats and follows
// the character set the TED is using (the ROM set at $D000 after boot)
TEST_CASE(plus4_graphics_viewer) {
    std::string err;
    MachineDescriptor* m = plus4::create(plus4::Model::Plus4, false, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    auto fmts = gfx::machineFormats(m);
    ASSERT(fmts.size() == 2 && fmts[0]->format == gfx::Format::TedChar);
    ASSERT(gfx::machineFormats(m, "vic").empty());
    m->onReset(*m);
    run(m, 6000000);
    IVideoInspect* vi = gfx::findVideoInspect(m);
    ASSERT(vi != nullptr);
    gfx::ViewRequest req;
    req.source = gfx::Source::Charset;
    req.count = 2;
    req.columns = 2;
    gfx::View v = gfx::buildView(req, m->buses[0].bus, vi);
    ASSERT(v.error.empty());
    ASSERT(v.format == gfx::Format::TedChar && v.start == 0xD000);
    auto kernal = romFile("roms/plus4/kernal.318004-05.bin");
    ASSERT(kernal.size() == 0x4000);
    for (int i = 0; i < 16; ++i) ASSERT(vi->peekVideo(0xD000 + i) == kernal[0x1000 + i]);
    // White background, black characters: '@' (character 0) has both
    std::set<uint32_t> cols(v.sheet.argb.begin(), v.sheet.argb.end());
    ASSERT(cols.count(vi->paletteColor(0x71)) && cols.count(vi->paletteColor(0x00)));
    delete m;
}

namespace {
// A 32 KB cartridge image (C1 low and high). C1 low marks itself for
// autostart ($8006 = 1 and "CBM" at $8007, which the KERNAL's poll at
// $FC33-$FC4E checks) and is called at reset with C1 in both banks. It
// stores $5A at $0FF0 and C1 high's byte at $C123 at $0FF1, then loops at
// $8015.
std::vector<uint8_t> testCartridge(bool autostart) {
    std::vector<uint8_t> img(0x8000, 0xFF);
    const uint8_t lo[] = {
        0x78, 0x4C, 0x0A, 0x80, 0x00, 0x00,        // SEI / JMP $800A
        (uint8_t)(autostart ? 1 : 0), 'C', 'B', 'M',
        0xA9, 0x5A, 0x8D, 0xF0, 0x0F,              // LDA #$5A / STA $0FF0
        0xAD, 0x23, 0xC1, 0x8D, 0xF1, 0x0F,        // LDA $C123 / STA $0FF1
        0x4C, 0x15, 0x80,                          // JMP $8015
    };
    std::copy(lo, lo + sizeof lo, img.begin());
    img[0x4000 + 0x0123] = 0xA7;                   // C1 high, $C123
    return img;
}

// The same as a VICE .crt: "PLUS4 CARTRIDGE ", generic, two 16 KB CHIP
// packets (bank 0 = C1, at $8000 and $C000)
std::vector<uint8_t> toCrt(const std::vector<uint8_t>& bin) {
    std::vector<uint8_t> crt(0x40, 0);
    std::memcpy(crt.data(), "PLUS4 CARTRIDGE ", 16);
    crt[0x13] = 0x40;                              // header length
    crt[0x14] = 1;                                 // version 1.0
    std::memcpy(&crt[0x20], "TEST CART", 9);
    for (int half = 0; half < 2; ++half) {
        const uint8_t chip[16] = {'C', 'H', 'I', 'P', 0x00, 0x00, 0x40, 0x10, 0, 0, 0, 0,
                                  (uint8_t)(half ? 0xC0 : 0x80), 0x00, 0x40, 0x00};
        crt.insert(crt.end(), chip, chip + 16);
        crt.insert(crt.end(), bin.begin() + half * 0x4000, bin.begin() + (half + 1) * 0x4000);
    }
    return crt;
}
}

// A cartridge in the expansion port is ROM bank 2 ($FDD0 + 2 / + 8); the
// KERNAL finds it at reset and starts it
TEST_CASE(c16_cartridge_autostart) {
    std::string err;
    MachineDescriptor* m = plus4::create(plus4::Model::C16, false, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    IBus* bus = m->buses[0].bus;
    ASSERT(plus4::insertCartridge(m, testCartridge(true), "test.bin", err));
    ASSERT(plus4::cartridgeInfo(m) == "test.bin (32 KB C1 low/high)");
    run(m, 3000000);
    ASSERT(bus->peek8(0x0FF0) == 0x5A);
    ASSERT(bus->peek8(0x0FF1) == 0xA7);            // C1 high at $C000
    uint32_t pc = m->cpus[0].cpu->pc();
    ASSERT(pc >= 0x8015 && pc < 0x8018);
    // Banks by hand: C1 low and C2 (empty, $FF) high
    bus->write8(0xFDD0 + 0x02 + 0x0C, 0);
    ASSERT(bus->peek8(0x8007) == 'C');
    ASSERT(bus->peek8(0xC123) == 0xFF);
    ASSERT(bus->peek8(0xFC36) == 0x07);            // $FC00-$FCFF: still the KERNAL
    // Out again: BASIC
    plus4::removeCartridge(m);
    ASSERT(plus4::cartridgeInfo(m).empty());
    run(m, 6000000);
    ASSERT(screenHas(m, "READY."));
    delete m;
}

// Without the autostart mark the KERNAL leaves it alone: BASIC boots
TEST_CASE(plus4_cartridge_no_autostart) {
    std::string err;
    MachineDescriptor* m = plus4::create(plus4::Model::Plus4, false, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    ASSERT(plus4::insertCartridge(m, testCartridge(false), "test.bin", err));
    run(m, 6000000);
    ASSERT(screenHas(m, "READY."));
    ASSERT(m->buses[0].bus->peek8(0x0FF0) != 0x5A);
    delete m;
}

// VICE .crt images, 8 KB raw images and things that are not cartridges
TEST_CASE(plus4_cartridge_formats) {
    std::string err;
    MachineDescriptor* m = plus4::create(plus4::Model::C16, false, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    IBus* bus = m->buses[0].bus;
    ASSERT(plus4::insertCartridge(m, toCrt(testCartridge(true)), "test.crt", err));
    ASSERT(plus4::cartridgeInfo(m) == "test.crt (Plus/4 .crt: TEST CART)");
    run(m, 3000000);
    ASSERT(bus->peek8(0x0FF0) == 0x5A);
    ASSERT(bus->peek8(0x0FF1) == 0xA7);

    // 8 KB: C1 low $8000-$9FFF, the rest $FF
    std::vector<uint8_t> small(0x2000, 0x11);
    ASSERT(plus4::insertCartridge(m, small, "small.bin", err));
    ASSERT(plus4::cartridgeInfo(m) == "small.bin (8 KB C1 low)");
    bus->write8(0xFDD0 + 0x0A, 0);
    ASSERT(bus->peek8(0x9FFF) == 0x11);
    ASSERT(bus->peek8(0xA000) == 0xFF);
    ASSERT(bus->peek8(0xC000) == 0xFF);

    ASSERT(!plus4::insertCartridge(m, std::vector<uint8_t>(1000, 0), "odd.bin", err));
    ASSERT(err.find("not a Plus/4 cartridge") != std::string::npos);
    auto crt = toCrt(testCartridge(true));
    crt[0x17] = 1;                                 // hardware type 1 (Magic Cart)
    ASSERT(!plus4::insertCartridge(m, crt, "magic.crt", err));
    ASSERT(err.find("hardware type 1") != std::string::npos);
    delete m;
}
