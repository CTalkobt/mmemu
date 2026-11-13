// The Dragon 32 / 64 and the boards' PAL timing, with hand-assembled 6809
// stand-ins for Dragon BASIC and DragonDOS (the real ROMs are copyright).

#include "test_harness.h"
#include "coco_test_rom.h"
#include "plugins/6809/main/cpu6809.h"
#include "plugins/devices/acia6551/main/acia6551.h"
#include "plugins/devices/wd17xx/main/wd17xx.h"
#include "libdevices/main/io_registry.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <unistd.h>

using namespace cocotest;

namespace {

constexpr uint16_t MARK_AT = 0x9000;     // a byte that tells the Dragon 64's two BASICs apart

/**
 * Dragon BASIC's stand-in, 16 KB at $8000: the PIAs as BASIC sets them
 * (PIA1 PB0 printer BUSY and PB2 in), the screen at $0400, "HELLO DRAGON",
 * FS interrupts counted at $0700, the cartridge FIRQ enabled (to $C000).
 */
std::vector<uint8_t> dragonBasic(uint8_t mark = 0x32) {
    Asm a(0x8000, 0x4000);
    a.emit({0x10, 0xCE, 0x7F, 0x00});           // LDS #$7F00
    a.ext(0x7F, 0xFF01); a.ext(0x7F, 0xFF00);   // PIA0: DDRA $00
    a.emit({0x86, 0x34}); a.ext(0xB7, 0xFF01);
    a.ext(0x7F, 0xFF03);
    a.emit({0x86, 0xFF}); a.ext(0xB7, 0xFF02);  // DDRB $FF
    a.emit({0x86, 0x34}); a.ext(0xB7, 0xFF03);
    a.ext(0x7F, 0xFF21);
    a.emit({0x86, 0xFE}); a.ext(0xB7, 0xFF20);  // PIA1: DDRA $FE
    a.emit({0x86, 0x34}); a.ext(0xB7, 0xFF21);
    a.ext(0x7F, 0xFF23);
    a.emit({0x86, 0xFA}); a.ext(0xB7, 0xFF22);  // DDRB $FA
    a.emit({0x86, 0x34}); a.ext(0xB7, 0xFF23);
    a.ext(0x7F, 0xFF22);
    a.emit({0x86, 0x02}); a.ext(0xB7, 0xFF20);  // STROBE high
    a.ext(0xB7, 0xFFC9);                        // SAM: display at $0400
    a.ext(0xB7, 0xFFDD);                        // 64K
    a.emit({0x8E, 0x04, 0x00});                 // LDX #$0400
    a.emit({0x86, 0x60});
    const uint16_t clr = a.pc;
    a.emit({0xA7, 0x80});
    a.emit({0x8C, 0x06, 0x00});
    a.branch(0x26, clr);
    a.emit({0x8E, 0x04, 0x00});
    const uint16_t msgRef = a.pc;
    a.emit({0x10, 0x8E, 0x00, 0x00});           // LDY #message
    const uint16_t ploop = a.pc;
    a.emit({0xA6, 0xA0});
    const uint16_t beq = a.pc;
    a.emit({0x27, 0x00});
    a.emit({0xA7, 0x80});
    a.branch(0x20, ploop);
    const uint16_t done = a.pc;
    a.b[(size_t)(beq + 1 - a.org)] = (uint8_t)(done - (beq + 2));
    a.ext(0x7F, COUNTER);
    a.ext(0x7F, CART_MARK);
    a.emit({0x86, 0x35}); a.ext(0xB7, 0xFF03);  // FS IRQ
    a.emit({0x86, 0x37}); a.ext(0xB7, 0xFF23);  // CART FIRQ
    a.emit({0x1C, 0xAF});
    const uint16_t spin = a.pc;
    a.branch(0x20, spin);
    const uint16_t irq = a.pc;
    a.ext(0xB6, 0xFF02);
    a.ext(0x7C, COUNTER);
    a.emit({0x3B});
    const uint16_t firq = a.pc;
    a.ext(0xB6, 0xFF22);
    a.ext(0x7E, 0xC000);
    const uint16_t msg = a.pc;
    a.text("HELLO DRAGON");
    a.word((uint16_t)(msgRef + 2), msg);
    a.b[MARK_AT - 0x8000] = mark;
    a.word(0xBFF6, firq);
    a.word(0xBFF8, irq);
    a.word(0xBFFC, irq);
    a.word(0xBFFE, 0x8000);
    return a.b;
}

MachineDescriptor* makeDragon(coco::Model model = coco::Model::Dragon32, coco::Tv tv = coco::Tv::Default, int ramKb = 0) {
    coco::Roms roms;
    roms.dragonBasic = dragonBasic(0x32);
    if (model == coco::Model::Dragon64) roms.dragon64k = dragonBasic(0x64);
    std::string err;
    return coco::create(model, &err, ramKb ? ramKb : (model == coco::Model::Dragon32 ? 32 : 64), &roms, tv);
}

coco::System* sysOf(MachineDescriptor* m) { return coco::systemOf(m); }

// Slow cycles a frame, measured over 10 frames
uint64_t cyclesPerFrame(MachineDescriptor* m) {
    coco::System* sys = sysOf(m);
    uint64_t f0 = sys->vdg.frames();
    while (sys->vdg.frames() == f0) m->schedulerStep(*m);
    f0 = sys->vdg.frames();
    const uint64_t t0 = sys->ticks();
    while (sys->vdg.frames() < f0 + 10) m->schedulerStep(*m);
    return (sys->ticks() - t0) / 10 / coco::TICKS_SLOW;
}

std::string tempPath(const char* stem, const char* ext) {
    std::string t = std::string("/tmp/") + stem + "_XXXXXX";
    std::vector<char> buf(t.begin(), t.end());
    buf.push_back(0);
    const int fd = mkstemp(buf.data());
    if (fd >= 0) close(fd);
    std::remove(buf.data());
    return std::string(buf.data()) + ext;
}

}  // namespace

// A missing Dragon BASIC names the file; the machine is still made
TEST_CASE(dragon_missing_rom_message) {
    const char* old = std::getenv("MMEMU_DRAGON_FIRMWARE");
    std::string keep = old ? old : "";
    setenv("MMEMU_DRAGON_FIRMWARE", "/nonexistent-dragon-roms", 1);
    std::string err;
    MachineDescriptor* m = coco::create(coco::Model::Dragon64, &err);
    if (old) setenv("MMEMU_DRAGON_FIRMWARE", keep.c_str(), 1); else unsetenv("MMEMU_DRAGON_FIRMWARE");
    ASSERT(m != nullptr);
    ASSERT(err.find("d64_1.rom") != std::string::npos);
    ASSERT(err.find("MMEMU_DRAGON_FIRMWARE") != std::string::npos);
    ASSERT(!m->warnings.empty());
    ASSERT_EQ(m->machineId, std::string("dragon64"));
    run(m, 50000);
    std::string screen;
    for (int i = 0; i < 19; ++i) screen += (char)m->buses[0].bus->peek8((uint16_t)i);
    ASSERT_EQ(screen, std::string("NO DRAGON BASIC ROM"));
    delete m;
}

// The stand-in boots; PAL: 312 lines of 57 cycles (17784 a frame), FS
// interrupts at 50 Hz, the 14.218 MHz crystal
TEST_CASE(dragon_pal_frame_timing) {
    MachineDescriptor* m = makeDragon();
    ASSERT(m);
    coco::System* sys = sysOf(m);
    ASSERT(sys->pal());
    ASSERT(sys->dragon());
    ASSERT_EQ(sys->vdg.frameLines(), 312);
    ASSERT(m->clockHz > 888600 && m->clockHz < 888700);        // 14.218 MHz / 16
    ASSERT_EQ(sys->sound.nativeSampleRate(), 43747);
    run(m, 200000);
    std::string text;
    for (int i = 0; i < 12; ++i) {
        const uint8_t c = m->buses[0].bus->peek8((uint16_t)(MESSAGE_AT + i)) & 0x3F;
        text += (char)(c < 0x20 ? c + 0x40 : c);
    }
    ASSERT_EQ(text, std::string("HELLO DRAGON"));
    const uint64_t per = cyclesPerFrame(m);
    ASSERT_EQ(per, (uint64_t)17784);
    const uint8_t before = m->buses[0].bus->peek8(COUNTER);
    run(m, (uint64_t)(m->clockHz));                            // a second
    const int n = (uint8_t)(m->buses[0].bus->peek8(COUNTER) - before);
    ASSERT(n >= 49 && n <= 51);
    // No artifact button on a PAL set
    bool artifact = false;
    for (const auto& b : m->screenButtons) if (b.label == "Artifact") artifact = true;
    ASSERT(!artifact);
    delete m;
}

// The Tano Dragon (NTSC Dragon 64) and a PAL CoCo: the frame each makes
TEST_CASE(dragon_tano_and_pal_coco) {
    MachineDescriptor* t = makeDragon(coco::Model::Dragon64, coco::Tv::NTSC);
    ASSERT(t);
    ASSERT(!sysOf(t)->pal());
    ASSERT_EQ(t->machineId, std::string("tano"));
    ASSERT_EQ(sysOf(t)->vdg.frameLines(), 262);
    run(t, 100000);
    ASSERT_EQ(cyclesPerFrame(t), (uint64_t)14934);
    delete t;
    coco::Roms roms;
    roms.colorBasic = colorBasic();
    std::string err;
    MachineDescriptor* c = coco::create(coco::Model::CoCo2, &err, 64, &roms, coco::Tv::PAL);
    ASSERT(c);
    coco::System* sys = sysOf(c);
    ASSERT(sys->pal());
    ASSERT_EQ(sys->masterHz(), coco::COCO_PAL_HZ);
    ASSERT(c->displayName.find("PAL") != std::string::npos);
    run(c, 200000);
    ASSERT_EQ(cyclesPerFrame(c), (uint64_t)17784);
    // FS interrupts at 50 Hz
    const uint8_t before = c->buses[0].bus->peek8(COUNTER);
    run(c, (uint64_t)c->clockHz);
    const int n = (uint8_t)(c->buses[0].bus->peek8(COUNTER) - before);
    ASSERT(n >= 49 && n <= 51);
    delete c;
}

// The Dragon's keyboard: the CoCo's keys and columns, its rows rotated
// (digits on rows 0-1, @ and letters from row 2, the arrows and SPACE on 5)
TEST_CASE(dragon_keyboard_matrix) {
    MachineDescriptor* m = makeDragon();
    ASSERT(m);
    run(m, 100000);
    IBus* bus = m->buses[0].bus;
    auto scan = [&](int col) { bus->write8(0xFF02, (uint8_t)~(1 << col)); return (uint8_t)(bus->read8(0xFF00) & 0x7F); };
    ASSERT_EQ(scan(1), 0x7F);
    m->onKey("a", true);                  // A: row 2 column 1
    ASSERT_EQ(scan(1), 0x7B);
    m->onKey("a", false);
    m->onKey("0", true);                  // 0: row 0 column 0
    ASSERT_EQ(scan(0), 0x7E);
    m->onKey("0", false);
    m->onKey("9", true);                  // 9: row 1 column 1
    ASSERT_EQ(scan(1), 0x7D);
    m->onKey("9", false);
    m->onKey("x", true);                  // X: row 5 column 0
    ASSERT_EQ(scan(0), 0x5F);
    m->onKey("x", false);
    m->onKey("up", true);                 // UP: row 5 column 3
    ASSERT_EQ(scan(3), 0x5F);
    m->onKey("up", false);
    m->onKey("return", true);             // ENTER: row 6 column 0, as the CoCo's
    ASSERT_EQ(scan(0), 0x3F);
    m->onKey("return", false);
    m->onKey("exclaim", true);            // SHIFT (row 6 column 7) with 1 (row 0 column 1)
    ASSERT_EQ(scan(7), 0x3F);
    ASSERT_EQ(scan(1), 0x7E);
    m->onKey("exclaim", false);
    // Typed text uses the same rows
    sysOf(m)->keyboard.enqueueText("B");
    bool seen = false;
    for (int i = 0; i < 2000 && !seen; ++i) {
        run(m, 100);
        seen = scan(2) == 0x7B;
    }
    ASSERT(seen);
    delete m;
}

// Dragon 32: PIA1 PB2 low (32K of 64K chips), PIA0 mirrored through
// $FF00-$FF1F (unexpanded); the printer takes PIA0 port B on STROBE
TEST_CASE(dragon32_ram_size_mirrors_and_printer) {
    MachineDescriptor* m = makeDragon();
    ASSERT(m);
    run(m, 100000);
    IBus* bus = m->buses[0].bus;
    coco::System* sys = sysOf(m);
    ASSERT_EQ(bus->read8(0xFF22) & 0x04, 0);
    ASSERT_EQ(bus->read8(0xFF07), bus->read8(0xFF03));       // a mirror of CRB
    // A byte to the printer: data on PIA0 port B, STROBE (PIA1 PA1) rising
    sys->printer.printed.clear();
    bus->write8(0xFF02, 'P');
    bus->write8(0xFF20, 0x00);
    bus->write8(0xFF20, 0x02);
    ASSERT_EQ(sys->printer.printed.size(), (size_t)1);
    ASSERT_EQ(sys->printer.printed[0], 'P');
    ASSERT_EQ(bus->read8(0xFF22) & 0x01, 0);                 // never BUSY
    // 16K: PB2 high
    std::string err;
    ASSERT(m->expansionSlot("ram")->install("ram", "16K", err));
    m->onReset(*m);
    run(m, 100000);
    ASSERT_EQ(bus->read8(0xFF22) & 0x04, 4);
    delete m;
}

// Dragon 64: PIA1 PB2 selects the BASIC (high the 32K mode, low the 64K
// mode); the 6551 at $FF04-$FF07 with PIA0 only at $FF00-$FF03
TEST_CASE(dragon64_rom_bank_and_acia) {
    MachineDescriptor* m = makeDragon(coco::Model::Dragon64);
    ASSERT(m);
    ASSERT_EQ(m->machineId, std::string("dragon64"));
    run(m, 100000);
    IBus* bus = m->buses[0].bus;
    coco::System* sys = sysOf(m);
    ASSERT_EQ(bus->read8(MARK_AT), 0x32);
    ASSERT(!sys->rom64kSelected());
    // PB2 an output, low: the 64K-mode BASIC
    bus->write8(0xFF23, 0x30);
    bus->write8(0xFF22, 0xFE);                // DDRB: PB2 out
    bus->write8(0xFF23, 0x34);
    bus->write8(0xFF22, 0x00);
    ASSERT(sys->rom64kSelected());
    ASSERT_EQ(bus->read8(MARK_AT), 0x64);
    ASSERT_EQ(bus->peek8(MARK_AT), 0x64);
    bus->write8(0xFF22, 0x04);
    ASSERT_EQ(bus->read8(MARK_AT), 0x32);
    // The ACIA: status with the transmitter empty; PIA0's CRB isn't at $FF07
    ASSERT(sys->acia());
    ASSERT_EQ(bus->read8(0xFF05) & 0x10, 0x10);
    bus->write8(0xFF07, 0x1E);                // 9600 baud, 8N1
    ASSERT_EQ(bus->read8(0xFF07), 0x1E);
    ASSERT(bus->read8(0xFF03) != 0x1E);
    bus->write8(0xFF06, 0x0B);                // DTR, no parity, no IRQs
    ASSERT_EQ(bus->read8(0xFF16), 0x0B);      // mirrored every 8 bytes
    // A byte round a loopback through the CLI's serial port
    std::vector<IOHandler*> hs;
    m->ioRegistry->enumerate(hs);
    bool took = false;
    for (auto* h : hs) if (!took && h->attachSerial("loopback")) took = std::string(h->name()) == "RS-232";
    ASSERT(took);
    bus->write8(0xFF04, 0xA5);
    bool got = false;
    for (int i = 0; i < 400 && !got; ++i) {
        run(m, 100);
        got = (bus->peek8(0xFF05) & 0x08) != 0;
    }
    ASSERT(got);
    ASSERT_EQ(bus->read8(0xFF04), 0xA5);
    delete m;
}

// A cartridge starts itself on the Dragon too: CART toggling, BASIC's FIRQ
TEST_CASE(dragon_cartridge_autostart) {
    MachineDescriptor* m = makeDragon();
    ASSERT(m);
    std::string err;
    ASSERT(coco::insertRomPak(m, romPak(0xD5), "game.dgn", err));
    m->onReset(*m);
    run(m, 300000);
    ASSERT_EQ(m->buses[0].bus->peek8(CART_MARK), 0xD5);
    // The CoCo's cartridges aren't offered
    ASSERT(!m->expansionSlot("cart")->install("fd500", "", err));
    ASSERT(m->expansionSlot("mpi1") == nullptr);
    delete m;
}

// DragonDOS: the WD2797 at $FF40-$FF43, the latch at $FF48; a VDK sector
// read as DragonDOS does it (a byte on each DRQ, which is the CART line;
// INTRQ to NMI at the end with the latch's NMI enable)
TEST_CASE(dragon_dragondos_reads_a_sector) {
    std::vector<uint8_t> rom(0x2000, 0x12);
    rom[0] = 'D'; rom[1] = 'K';
    coco::DragonDos dos(rom);
    bool cart = true, nmi = false;
    dos.setCart = [&](bool level) { cart = level; };
    dos.setNmi = [&](bool on) { nmi = on; };
    dos.reset();
    ASSERT(!cart);
    ASSERT(!nmi);
    uint8_t d = 0;
    ASSERT(dos.read(0xC000, true, false, d) && d == 'D');

    // A 40-track single-sided VDK: 18 sectors of 256 bytes a track
    const std::string vdk = tempPath("dragondos", ".vdk");
    {
        std::vector<uint8_t> img = {'d', 'k', 12, 0, 0x10, 0x10, 'M', 0, 40, 1, 0, 0};
        std::vector<uint8_t> body(40 * 18 * 256, 0xE5);
        for (int i = 0; i < 256; ++i) body[20 * 18 * 256 + 4 * 256 + i] = (uint8_t)(i * 7);   // track 20, sector 5
        img.insert(img.end(), body.begin(), body.end());
        std::ofstream f(vdk, std::ios::binary);
        f.write((const char*)img.data(), (std::streamsize)img.size());
    }
    ASSERT(dos.mountDisk(0, vdk));
    ASSERT(dos.describe().find("drive 1:") != std::string::npos);

    auto tickUntil = [&](std::function<bool()> done, uint64_t limit) {
        for (uint64_t t = 0; t < limit && !done(); t += 16) dos.clock(16);
        return done();
    };
    auto rd = [&](uint16_t a) { dos.read(a, false, true, d); return d; };
    dos.write(0xFF48, false, true, 0x24);                 // drive 1, motor, double density, NMI enabled
    ASSERT_EQ(dos.controller().selectedDrive(), 0);
    ASSERT(dos.controller().motor());
    ASSERT(dos.controller().doubleDensity());
    dos.write(0xFF43, false, true, 20);                   // SEEK track 20
    dos.write(0xFF40, false, true, 0x10);
    ASSERT(tickUntil([&] { return nmi; }, 14318180ull * 2));
    ASSERT_EQ(rd(0xFF41), 20);
    rd(0xFF40);                                            // status read clears INTRQ: NMI off
    ASSERT(!nmi);
    // READ SECTOR 5 with IBM lengths (L): each byte as DRQ (CART) rises
    dos.write(0xFF42, false, true, 5);
    dos.write(0xFF40, false, true, 0x88);
    std::vector<uint8_t> got;
    while (got.size() < 256) {
        ASSERT(tickUntil([&] { return cart || nmi; }, 14318180ull));
        if (nmi) break;
        got.push_back(rd(0xFF43));
        ASSERT(!cart);                                     // reading the data clears DRQ
    }
    ASSERT_EQ(got.size(), (size_t)256);
    for (int i = 0; i < 256; ++i) ASSERT_EQ(got[i], (uint8_t)(i * 7));
    ASSERT(tickUntil([&] { return nmi; }, 14318180ull));
    ASSERT_EQ(rd(0xFF40) & 0x1C, 0);
    // Without the NMI enable INTRQ doesn't reach /NMI
    dos.write(0xFF48, false, true, 0x04);
    dos.write(0xFF40, false, true, 0x10);
    tickUntil([&] { return dos.controller().intrq(); }, 14318180ull * 2);
    ASSERT(dos.controller().intrq());
    ASSERT(!nmi);
    ASSERT(dos.ejectDisk(0));
    std::remove(vdk.c_str());
}

// DragonDOS in a Dragon: fitted with `expansion cart dragondos`, `-i` / `disk
// mount 1` puts the image in DragonDOS's drive 1, its ROM at $C000
TEST_CASE(dragon_dragondos_in_the_machine) {
    MachineDescriptor* m = makeDragon(coco::Model::Dragon64);
    ASSERT(m);
    const std::string romPath = tempPath("ddos", ".rom");
    {
        std::vector<uint8_t> rom = romPak(0x44);
        std::ofstream f(romPath, std::ios::binary);
        f.write((const char*)rom.data(), (std::streamsize)rom.size());
    }
    std::string err;
    ASSERT(m->expansionSlot("cart")->install("dragondos", romPath, err));
    std::remove(romPath.c_str());
    const std::string vdk = tempPath("ddos_disk", ".vdk");
    {
        std::vector<uint8_t> img = {'d', 'k', 12, 0, 0x10, 0x10, 'M', 0, 40, 1, 0, 0};
        img.resize(12 + 40 * 18 * 256, 0xE5);
        std::ofstream f(vdk, std::ios::binary);
        f.write((const char*)img.data(), (std::streamsize)img.size());
    }
    ASSERT(m->ioRegistry->mountDisk(1, vdk) != nullptr);
    auto* dos = dynamic_cast<coco::DragonDos*>(sysOf(m)->cart());
    ASSERT(dos);
    ASSERT_EQ(dos->diskPath(0), vdk);
    m->onReset(*m);
    run(m, 100000);
    IBus* bus = m->buses[0].bus;
    ASSERT_EQ(bus->peek8(0xC000), 0x86);
    bus->write8(0xFF48, 0x05);                // drive 2, motor
    ASSERT_EQ(dos->controller().selectedDrive(), 1);
    bus->write8(0xFF42, 0x09);                // the sector register
    ASSERT_EQ(bus->read8(0xFF42), 0x09);
    delete m;
    std::remove(vdk.c_str());
}
