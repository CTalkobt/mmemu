#include "test_harness.h"
#include "coco_test_rom.h"
#include "libdevices/main/io_registry.h"
#include "libdevices/main/ivideo_output.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <unistd.h>

using namespace cocotest;

namespace {

constexpr uint16_t IRQ_COUNT = 0x0700;
constexpr uint16_t FIRQ_COUNT = 0x0702;

/**
 * Super Extended Color BASIC's stand-in (32 KB at $8000): the MMU's two
 * tasks as BASIC sets them (pages $38-$3F), page $36 at $2000 for the
 * screen, INIT0 $4C (MMU, constant page, SCS), a 40-column screen with
 * attributes at physical $6C000, "HELLO" at its top in palette 10, the
 * GIME's VBORD as IRQ (counted at $0700; a FIRQ handler counts at $0702),
 * the CPU at 1.79 MHz, then a wait.
 */
std::vector<uint8_t> coco3Basic() {
    Asm a(0x8000, 0x8000, 0x12);
    a.emit({0x10, 0xCE, 0x7F, 0x00});                 // LDS #$7F00
    for (int i = 0; i < 16; ++i) {
        a.emit({0x86, 0x38 + (i & 7)});               // LDA #page
        a.ext(0xB7, (uint16_t)(0xFFA0 + i));          // STA $FFA0+i
    }
    a.emit({0x86, 0x36}); a.ext(0xB7, 0xFFA1);        // the screen's page at $2000
    a.emit({0x86, 0x4C}); a.ext(0xB7, 0xFF90);        // INIT0: MMUEN, MC3, MC2
    a.ext(0x7F, 0xFF91);                              // task 0, timer at the line rate
    a.ext(0x7F, 0xFFB0);                              // palette 0: black
    a.emit({0x86, 0x3F}); a.ext(0xB7, 0xFFB9);        // palette 9: white
    a.emit({0x86, 0x24}); a.ext(0xB7, 0xFFBA);        // palette 10: red
    a.emit({0x86, 0x03}); a.ext(0xB7, 0xFF98);        // text, 8 lines a row
    a.emit({0x86, 0x05}); a.ext(0xB7, 0xFF99);        // 40 columns, attributes
    a.emit({0x86, 0x09}); a.ext(0xB7, 0xFF9A);        // border
    a.ext(0x7F, 0xFF9C);
    a.emit({0x86, 0xD8}); a.ext(0xB7, 0xFF9D);        // video at $6C000
    a.ext(0x7F, 0xFF9E);
    a.ext(0x7F, 0xFF9F);
    // Clear the screen: spaces, white on black
    a.emit({0x8E, 0x20, 0x00});                       // LDX #$2000
    a.emit({0xCC, 0x20, 0x08});                       // LDD #$2008
    const uint16_t clr = a.pc;
    a.emit({0xED, 0x81});                             // STD ,X++
    a.emit({0x8C, 0x27, 0x80});                       // CMPX #$2780
    a.branch(0x26, clr);
    // The message, in red
    a.emit({0x8E, 0x20, 0x00});                       // LDX #$2000
    const uint16_t msgRef = a.pc;
    a.emit({0x10, 0x8E, 0x00, 0x00});                 // LDY #msg
    a.emit({0xC6, 0x10});                             // LDB #$10 (fg 10)
    const uint16_t loop = a.pc;
    a.emit({0xA6, 0xA0});                             // LDA ,Y+
    const uint16_t beq = a.pc;
    a.emit({0x27, 0x00});                             // BEQ done
    a.emit({0xED, 0x81});                             // STD ,X++
    a.branch(0x20, loop);
    a.b[(size_t)(beq + 1 - a.org)] = (uint8_t)(a.pc - (beq + 2));
    a.ext(0x7F, IRQ_COUNT);
    a.ext(0x7F, FIRQ_COUNT);
    // GIME VBORD on IRQ; PIA0's FS left off
    a.emit({0x86, 0x08}); a.ext(0xB7, 0xFF92);
    a.emit({0x86, 0x6C}); a.ext(0xB7, 0xFF90);        // + IEN
    a.ext(0xB7, 0xFFD9);                              // 1.79 MHz
    a.emit({0x1C, 0xAF});                             // ANDCC #$AF: IRQ and FIRQ on
    const uint16_t spin = a.pc;
    a.branch(0x20, spin);
    const uint16_t irq = a.pc;
    a.ext(0xB6, 0xFF92);                              // LDA $FF92 (acknowledge)
    a.ext(0x7C, IRQ_COUNT);
    a.emit({0x3B});
    const uint16_t firq = a.pc;
    a.ext(0xB6, 0xFF93);
    a.ext(0x7C, FIRQ_COUNT);
    a.emit({0x3B});
    const uint16_t msg = a.pc;
    a.text("HELLO");
    a.word((uint16_t)(msgRef + 2), msg);
    a.word(0xFFF6, firq);
    a.word(0xFFF8, irq);
    a.word(0xFFFE, 0x8000);
    return a.b;
}

MachineDescriptor* make3(int ramKb = 128) {
    coco::Roms roms;
    roms.coco3Basic = coco3Basic();
    std::string err;
    return coco::create(coco::Model::CoCo3, &err, ramKb, &roms);
}

}  // namespace

// The stand-in boots: the MMU, a 40-column screen, the GIME's IRQ, 1.79 MHz
TEST_CASE(coco3_boot_40_columns) {
    MachineDescriptor* m = make3();
    ASSERT(m);
    ASSERT_EQ(m->machineId, std::string("coco3"));
    coco::System* sys = coco::systemOf(m);
    ASSERT(sys && sys->coco3());
    Gime* g = sys->gime();
    ASSERT(g);
    run(m, 300000);
    ASSERT(g->mmuEnabled());
    ASSERT(g->fast());
    ASSERT_EQ(g->reg(0), 0x6C);
    // The text in RAM: physical $6C000 (page $36), the CPU's $2000
    ASSERT_EQ(m->buses[0].bus->peek8(0x2000), 'H');
    ASSERT_EQ(m->buses[0].bus->peek8(0x2001), 0x10);
    ASSERT_EQ(sys->ramByte(0x6C000 & 0x1FFFF), 'H');
    // On the screen: 'H' in palette 10 on palette 0
    const int x0 = g->activeX(), y0 = g->activeY();
    const uint8_t hRow1 = gime_font::GLYPHS['H'][1];
    for (int d = 0; d < 8; ++d) ASSERT_EQ(g->pixel(x0 + d * 2, y0), (hRow1 & (0x80 >> d)) ? 0x24 : 0x00);
    ASSERT_EQ(g->pixel(0, y0), 0x09);
    // VBORD reached the CPU as IRQ each frame (20 frames at 1.79 MHz)
    const uint8_t irqs = m->buses[0].bus->peek8(IRQ_COUNT);
    ASSERT(irqs >= 15 && irqs <= 25);
    // The frontends' view: 704 x 240, the RGB / composite colours
    std::vector<IOHandler*> hs;
    m->ioRegistry->enumerate(hs);
    IVideoOutput* video = nullptr;
    for (auto* h : hs) if (auto* v = dynamic_cast<IVideoOutput*>(h)) video = v;
    ASSERT(video);
    ASSERT_EQ(video->getDimensions().width, Gime::OUT_W);
    ASSERT_EQ(video->getDimensions().height, Gime::OUT_H);
    std::string err;
    ASSERT(m->expansionSlot("monitor")->install("rgb", "", err));
    std::vector<uint32_t> px((size_t)Gime::OUT_W * Gime::OUT_H);
    video->renderFrame(px.data());
    ASSERT_EQ(px[(size_t)y0 * Gime::OUT_W + 0], Gime::rgb(0x09));
    delete m;
}

// GIME interrupts routed with the PIAs': the keyboard (EI1) as FIRQ, PIA0's FS as IRQ
TEST_CASE(coco3_keyboard_and_pia_interrupts) {
    MachineDescriptor* m = make3();
    coco::System* sys = coco::systemOf(m);
    IBus* bus = m->buses[0].bus;
    run(m, 100000);
    // All columns driven low, EI1 on FIRQ
    bus->write8(0xFF03, 0x00);
    bus->write8(0xFF02, 0xFF);                // DDRB: outputs
    bus->write8(0xFF03, 0x04);
    bus->write8(0xFF02, 0x00);
    bus->write8(0xFF93, Gime::INT_EI1);
    bus->write8(0xFF90, 0x7C);                // + FEN
    const uint8_t before = bus->peek8(FIRQ_COUNT);
    run(m, 2000);
    ASSERT_EQ(bus->peek8(FIRQ_COUNT), before);
    sys->keyboard.key("a", true);
    run(m, 2000);
    ASSERT(bus->peek8(FIRQ_COUNT) > before);
    sys->keyboard.key("a", false);
    // PIA0's FS (CB1) from the GIME's FSYNC: its flag sets each frame
    bus->read8(0xFF02);
    run(m, 20000);
    ASSERT(bus->peek8(0xFF03) & 0x80);
    delete m;
}

// The ROM's address pins are the CPU's; 512 KB; the cartridge port's /CTS
TEST_CASE(coco3_rom_ram_and_cartridge) {
    MachineDescriptor* m = make3();
    coco::System* sys = coco::systemOf(m);
    IBus* bus = m->buses[0].bus;
    run(m, 100000);
    ASSERT_EQ(bus->peek8(0x8000), 0x10);       // LDS
    ASSERT_EQ(bus->peek8(0xFFFE), 0x80);
    // Without a cartridge $C000 floats; with a ROM Pak it reads the pak
    std::string err;
    std::vector<uint8_t> pak(0x2000, 0x5A);
    ASSERT(coco::insertRomPak(m, pak, "test.ccc", err));
    ASSERT_EQ(bus->peek8(0xC000), 0x5A);
    bus->write8(0xFF90, 0x4E);                // 32K internal: the pak is gone
    ASSERT(bus->peek8(0xC000) != 0x5A);
    bus->write8(0xFF90, 0x4F);                // 32K external
    ASSERT_EQ(bus->peek8(0x8000), 0x5A);
    ASSERT_EQ(bus->peek8(0xFFFE), 0x80);       // the vectors stay internal
    bus->write8(0xFF90, 0x6C);
    coco::removeRomPak(m);
    // TY: all RAM
    bus->write8(0xFFDF, 0);
    bus->write8(0x9000, 0xA5);
    ASSERT_EQ(bus->peek8(0x9000), 0xA5);
    bus->write8(0xFFDE, 0);
    ASSERT_EQ(bus->peek8(0x8000), 0x10);
    // 512K: pages $00-$2F are their own RAM
    ASSERT(m->expansionSlot("ram")->install("ram", "512K", err));
    ASSERT_EQ(sys->gime()->ramMask(), 0x7FFFFu);
    bus->write8(0xFFA2, 0x01);
    bus->write8(0x4000, 0x77);
    bus->write8(0xFFA2, 0x31);
    ASSERT(bus->peek8(0x4000) != 0x77);
    bus->write8(0xFFA2, 0x01);
    ASSERT_EQ(bus->peek8(0x4000), 0x77);
    ASSERT(!m->expansionSlot("ram")->install("ram", "64K", err));
    // The GIME revision
    ASSERT(m->expansionSlot("gime")->install("gime", "1986", err));
    ASSERT(sys->gime()->revision() == Gime::Revision::G1986);
    delete m;
}

// The FD-502, the Deluxe RS-232 Pak and the Multi-Pak go in the port as on a CoCo 2
TEST_CASE(coco3_disk_and_expansions) {
    MachineDescriptor* m = make3();
    std::string err;
    char path[] = "/tmp/mmemu-coco3-diskXXXXXX";
    const int fd = mkstemp(path);
    ASSERT(fd >= 0);
    std::vector<uint8_t> rom(0x2000, 0x39);
    ASSERT(write(fd, rom.data(), rom.size()) == (ssize_t)rom.size());
    close(fd);
    ASSERT(m->expansionSlot("cart")->install("mpi", "4", err));
    ASSERT(m->expansionSlot("mpi4")->install("fd502", path, err));
    ASSERT(m->expansionSlot("mpi1")->install("rs232", "", err));
    unlink(path);
    IBus* bus = m->buses[0].bus;
    run(m, 100000);
    ASSERT_EQ(bus->peek8(0xC000), 0x39);       // Disk BASIC through the Multi-Pak's slot 4
    // The controller at $FF48 (INIT0 MC2 lets $FF40-$FF4F out as /SCS)
    bus->write8(0xFF48, 0xD0);                // force interrupt
    ASSERT(!(bus->peek8(0xFF48) & 0x01));
    // The 6551 in slot 1 at $FF68
    bus->write8(0xFF6A, 0x0B);
    ASSERT_EQ(bus->peek8(0xFF6A), 0x0B);
    ASSERT(coco::cartridgeInfo(m).find("Multi-Pak") != std::string::npos);
    delete m;
}

// Without coco3.rom: a stand-in that says so
TEST_CASE(coco3_missing_rom_message) {
    const char* old = std::getenv("MMEMU_COCO_FIRMWARE");
    std::string keep = old ? old : "";
    setenv("MMEMU_COCO_FIRMWARE", "/nonexistent-coco-roms", 1);
    std::string err;
    MachineDescriptor* m = coco::create(coco::Model::CoCo3, &err, 128);
    if (old) setenv("MMEMU_COCO_FIRMWARE", keep.c_str(), 1); else unsetenv("MMEMU_COCO_FIRMWARE");
    ASSERT(m != nullptr);
    ASSERT(err.find("coco3.rom") != std::string::npos);
    ASSERT(!m->warnings.empty());
    run(m, 100000);
    std::string screen;
    for (int i = 0; i < 8; ++i) screen += (char)(m->buses[0].bus->peek8((uint16_t)i) & 0x7F);
    ASSERT_EQ(screen, std::string("NO`SUPER"));
    coco::System* sys = coco::systemOf(m);
    ASSERT(sys->gime()->coco());
    // On the screen through the VDG-compatible mode: 'N' inverse, green on black
    const int x0 = sys->gime()->activeX(), y0 = sys->gime()->activeY();
    const uint8_t nRow1 = gime_font::GLYPHS['N'][1];
    for (int d = 0; d < 8; ++d) ASSERT_EQ(sys->gime()->pixel(x0 + d * 2, y0 + 1), (nRow1 & (0x80 >> d)) ? 0x00 : 0x12);
    delete m;
}

// Snapshots: the RAM, the GIME and the clock
TEST_CASE(coco3_snapshot_roundtrip) {
    MachineDescriptor* m = make3();
    IBus* bus = m->buses[0].bus;
    run(m, 100000);
    std::vector<uint8_t> snap(bus->stateSize());
    bus->saveState(snap.data());
    const uint8_t was = bus->peek8(0x2000);
    bus->write8(0x2000, 0x00);
    bus->write8(0xFFB0, 0x3F);
    bus->loadState(snap.data());
    ASSERT_EQ(bus->peek8(0x2000), was);
    ASSERT_EQ(coco::systemOf(m)->gime()->palette(0), 0x00);
    ASSERT(coco::systemOf(m)->gime()->mmuEnabled());
    run(m, 10000);
    delete m;
}

// coco3h: the same board with the HD6309; the stand-in boots on it, and
// native mode (LDMD #1) runs the 6309's own instructions on the CoCo 3's bus
TEST_CASE(coco3h_hd6309) {
    coco::Roms roms;
    roms.coco3Basic = coco3Basic();
    std::string err;
    MachineDescriptor* m = coco::create(coco::Model::CoCo3, &err, 512, &roms, coco::Tv::Default, true);
    ASSERT(m);
    ASSERT_EQ(std::string(m->cpus[0].cpu->isaName()), std::string("6309"));
    coco::System* sys = coco::systemOf(m);
    run(m, 300000);
    ASSERT(sys->gime()->mmuEnabled());
    ASSERT_EQ(m->buses[0].bus->peek8(0x2000), 'H');
    ASSERT(m->buses[0].bus->peek8(IRQ_COUNT) >= 15);
    delete m;
}

// The receive line: 8N1 at the baud rate, LSB first, back to back; mark between
TEST_CASE(coco_serial_receive_line) {
    coco::BitBanger b;
    b.setBaud(600);
    const double bit = coco::MASTER_HZ / 600.0;
    auto at = [&](double bits) { return b.rxLevel((uint64_t)(bit * bits)); };
    ASSERT(b.rxLevel(0));                       // nothing queued: mark
    b.receive({0xA5, 0x00});
    ASSERT(b.receiving());
    ASSERT(!at(0));                             // the start bit, from the first look
    ASSERT(!at(0.5));
    const uint8_t want = 0xA5;
    for (int i = 0; i < 8; ++i) ASSERT_EQ(at(1.5 + i), (((want >> i) & 1) != 0));
    ASSERT(at(9.5));                            // the stop bit
    ASSERT(!at(10.5));                          // the next frame's start bit, straight after
    ASSERT(!at(15.5));
    ASSERT(at(19.5));
    ASSERT(at(20.5));
    ASSERT(!b.receiving());
}

// EI2: the RS-232 receive line's falling edge (a start bit), as PIA1 PB0 reads it
TEST_CASE(coco3_serial_ei2) {
    MachineDescriptor* m = make3();
    coco::System* sys = coco::systemOf(m);
    Gime* g = sys->gime();
    IBus* bus = m->buses[0].bus;
    run(m, 100000);
    bus->write8(0xFF93, Gime::INT_EI2);        // EI2 to FIRQ status (FEN off: no FIRQ taken)
    run(m, 1000);
    ASSERT(!(g->firqStatus() & Gime::INT_EI2));
    ASSERT(!(sys->pia1.portB() & 0x01));         // nothing connected: PB0 low, no EI2
    sys->rs232.receive({});
    run(m, 100);
    ASSERT(sys->pia1.portB() & 0x01);           // connected, idle at mark
    ASSERT(!(g->firqStatus() & Gime::INT_EI2));
    sys->rs232.receive({0x00});
    run(m, 100);
    ASSERT(g->firqStatus() & Gime::INT_EI2);
    ASSERT(!(sys->pia1.portB() & 0x01));
    bus->read8(0xFF93);                        // acknowledge
    run(m, 2000);                              // data bits (all 0): no new edge
    ASSERT(!(g->firqStatus() & Gime::INT_EI2));
    run(m, 20000);
    ASSERT(sys->pia1.portB() & 0x01);           // back at mark
    ASSERT(!sys->rs232.receiving());
    // From a host file
    char path[] = "/tmp/mmemu-coco3-serialXXXXXX";
    const int fd = mkstemp(path);
    ASSERT(fd >= 0);
    ASSERT(write(fd, "U", 1) == 1);
    close(fd);
    ASSERT(sys->rs232.attachSerial(std::string("in:") + path));
    unlink(path);
    run(m, 100);
    ASSERT(g->firqStatus() & Gime::INT_EI2);
    delete m;
}

// Disto 2 MB: page register bits 6-7 pick the 512K bank; they read back
TEST_CASE(coco3_disto_2mb) {
    MachineDescriptor* m = make3();
    coco::System* sys = coco::systemOf(m);
    IBus* bus = m->buses[0].bus;
    std::string err;
    ASSERT(m->expansionSlot("ram")->install("ram", "2M", err));
    ASSERT_EQ(sys->gime()->ramKb(), 2048);
    ASSERT(m->expansionSlot("ram")->info().find("2M") != std::string::npos);
    run(m, 100000);
    bus->write8(0xFFA2, 0xC1);
    bus->write8(0x4000, 0x77);
    ASSERT_EQ(sys->ramByte((3u << 19) | (1u << 13)), 0x77);
    ASSERT_EQ(bus->read8(0xFFA2), 0xC1);
    bus->write8(0xFFA2, 0x01);
    ASSERT(bus->peek8(0x4000) != 0x77);
    bus->write8(0xFF9B, 0x03);
    ASSERT_EQ(bus->read8(0xFF9B) & 0x03, 0x03);
    ASSERT(m->expansionSlot("ram")->install("ram", "1M", err));
    ASSERT_EQ(sys->gime()->ramMask(), 0xFFFFFu);
    delete m;
}

// The monitor: composite's artifact option (and the screen button), RGB's
// monitor detect on PIA1 PB3
TEST_CASE(coco3_monitor_options) {
    MachineDescriptor* m = make3();
    coco::System* sys = coco::systemOf(m);
    std::string err;
    ASSERT(m->expansionSlot("monitor")->install("composite", "bluered", err));
    ASSERT(sys->gime()->artifact() == Gime::Artifact::BlueRed);
    ASSERT(!m->expansionSlot("monitor")->install("composite", "green", err));
    m->onKey("artifact", true);
    ASSERT(sys->gime()->artifact() == Gime::Artifact::RedBlue);
    m->onKey("artifact", true);
    ASSERT(sys->gime()->artifact() == Gime::Artifact::Off);
    run(m, 1000);
    ASSERT(sys->pia1.b.inSink & 0x08);
    ASSERT(m->expansionSlot("monitor")->install("rgb", "", err));
    run(m, 1000);
    ASSERT(!(sys->pia1.b.inSink & 0x08));
    delete m;
}
