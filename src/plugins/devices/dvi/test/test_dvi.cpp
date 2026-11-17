// The Tandy Disk/Video Interface (#358): the box's hardware as the service
// manual has it (refs/dvi/FINDINGS.md), then the real ROMs and system
// diskette when they are in roms/m100/ (dvi.rom, dvichar.rom,
// dvi_system.dsk and m100.rom; else those tests say so and pass).

#include "test_harness.h"
#include "plugins/devices/dvi/main/dvi.h"
#include "plugins/machines/m100/main/machine_m100.h"
#include "plugins/machines/m100/main/m100_system.h"
#include "plugins/machines/m100/test/m100_test_rom.h"
#include "plugins/z80/main/cpu_z80.h"
#include <cstdio>
#include <fstream>
#include <string>
#include <unistd.h>

using namespace m100test;
using m100::Model;

namespace {

std::string tmpPath(const std::string& tag) {
    return "/tmp/mmsim_dvi_" + std::to_string((long)getpid()) + "_" + tag;
}

// A stand-in boot ROM (an idle loop) and a character ROM whose 'A' is a bar
std::vector<uint8_t> idleBootRom() {
    std::vector<uint8_t> r(0x400, 0x00);
    r[0] = 0xF3;                                       // DI
    r[1] = 0x18; r[2] = 0xFE;                          // JR $
    r[0x200] = 0x5A;                                   // a marker to see the 1 KB repeat
    return r;
}

std::vector<uint8_t> testCharRom() {
    std::vector<uint8_t> cg(0x800, 0x00);
    for (int ra = 0; ra < 8; ++ra) cg[0x41 * 8 + ra] = 0xF0;    // 'A': the left half of the cell
    return cg;
}

// A raw 40 x 18 x 256 disk: byte i of track t sector s (1-18) = t * 18 + s + i
std::string rawDisk(const std::string& tag) {
    const std::string path = tmpPath(tag);
    std::ofstream o(path, std::ios::binary);
    for (int t = 0; t < 40; ++t)
        for (int s = 1; s <= 18; ++s)
            for (int i = 0; i < 256; ++i) o.put((char)(uint8_t)(t * 18 + s + i));
    return path;
}

bool have(const std::string& file) {
    std::ifstream f(m100::firmwareDir() + "/" + file);
    if (!f) std::printf("  (no %s in %s: skipped)\n", file.c_str(), m100::firmwareDir().c_str());
    return (bool)f;
}

// The Model 100 ROM's copy of its screen at $FE00 (40 x 8)
std::string lcdText(MachineDescriptor* m) {
    std::string s;
    for (int i = 0; i < 320; ++i) {
        const uint8_t c = m->buses[0].bus->peek8((uint32_t)(0xFE00 + i));
        s += (c >= 0x20 && c < 0x7F) ? (char)c : ' ';
        if (i % 40 == 39) s += '\n';
    }
    return s;
}

std::string dviText(const Dvi* d) {
    std::string s;
    for (int y = 0; y < 25; ++y) s += d->rowText(y) + "\n";
    return s;
}

// Run until `text` is in `get()` (true) or `seconds` pass
template <class F>
bool waitFor(MachineDescriptor* m, F get, const std::string& text, double seconds) {
    for (double t = 0; t < seconds; t += 0.25) {
        if (get().find(text) != std::string::npos) return true;
        runFor(m, 0.25);
    }
    if (get().find(text) != std::string::npos) return true;
    std::printf("  waiting for \"%s\":\n%s", text.c_str(), get().c_str());
    return false;
}

std::string copyOf(const std::string& from, const std::string& tag) {
    const std::string to = tmpPath(tag);
    std::ifstream i(from, std::ios::binary);
    std::ofstream o(to, std::ios::binary);
    o << i.rdbuf();
    return to;
}

}  // namespace

// Memory: the 1 KB boot ROM (repeating) or RAM1 by the bank flip-flop,
// RAM2, the 2-bit attribute RAM and the character RAM, $FF elsewhere
TEST_CASE(dvi_memory_map) {
    Dvi d(idleBootRom(), testCharRom());
    ASSERT_EQ(d.busRead(0x0000), 0xF3);
    ASSERT_EQ(d.busRead(0x0600), 0x5A);                // A10 grounded: $0200 again
    d.busWrite(0x0000, 0x11);                          // the ROM ignores writes
    ASSERT_EQ(d.busRead(0x0000), 0xF3);
    d.busWrite(0x0800, 0x22);
    ASSERT_EQ(d.busRead(0x0800), 0x22);
    ASSERT_EQ(d.busRead(0x1000), 0xFF);                // nothing at $1000-$BFFF
    d.portOut(0x20, 0x02);                             // STS bit 1: bank 1
    ASSERT(d.bank1());
    d.busWrite(0x0000, 0x33);
    ASSERT_EQ(d.busRead(0x0000), 0x33);                // RAM1
    ASSERT_EQ(d.busRead(0x0800), 0x22);                // RAM2 in both banks
    d.portOut(0x3F, 0x00);                             // (anywhere in $20-$3F)
    ASSERT_EQ(d.busRead(0x0000), 0xF3);                // back to the ROM; RAM1 kept
    d.portOut(0x20, 0x02);
    ASSERT_EQ(d.busRead(0x0000), 0x33);
    d.busWrite(0xC005, 0x02);
    ASSERT_EQ(d.busRead(0xC005), 0xFE);                // ARAM: 2 bits, bits 7-2 read 1
    ASSERT_EQ(d.busRead(0xC805), 0xFE);                // 2 KB, repeating to $DFFF
    d.busWrite(0xE123, 'X');
    ASSERT_EQ(d.busRead(0xF923), 'X');                 // CRAM likewise to $FFFF
    ASSERT_EQ(d.busRead(0x4000), 0xFF);
    ASSERT_EQ(d.portIn(0x80), 0xFF);                   // A7 = 1: no device
}

// The 8255 between the two: the portable writes port A (/OBF low: the
// box's INT), the box takes it with /ACK (IN $60-$7F); the box writes with
// /STB (OUT $60-$7F: IBF), the portable reads port A. Port B's low nibble
// is the data mode the box reads in the STS port; PC0-PC2 read 0 (the box's
// reset line, low while it runs) - how the Model 100's ROM finds a DVI
TEST_CASE(dvi_ppi_handshake) {
    Dvi d(idleBootRom(), testCharRom());
    ASSERT_EQ(d.hostIn(0x82) & 0x07, 0);               // a DVI is there
    d.hostOut(0x83, 0xC1);                             // mode 2 (the ROM's $768F)
    ASSERT_EQ(d.hostIn(0x82) & 0xA0, 0x80);            // /OBF high (empty), IBF low
    d.hostOut(0x81, 0x03);                             // data mode 3: disk command
    ASSERT_EQ(d.portIn(0x20) & 0x1F, 0x13);            // STS: PB0-PB3, D4 not driven
    d.hostOut(0x80, 0x5A);
    ASSERT(d.intPending());
    ASSERT_EQ(d.hostIn(0x82) & 0x80, 0);               // /OBF low: full
    ASSERT_EQ(d.portIn(0x60), 0x5A);                   // /ACK
    ASSERT(!d.intPending());
    ASSERT_EQ(d.hostIn(0x82) & 0x80, 0x80);
    ASSERT_EQ(d.portIn(0x20) & 0x40, 0);               // STS bit 6: IBF low
    d.portOut(0x7F, 0xA5);                             // /STB (anywhere in $60-$7F)
    ASSERT_EQ(d.portIn(0x20) & 0x40, 0x40);
    ASSERT_EQ(d.hostIn(0x82) & 0x20, 0x20);            // IBF for the portable
    ASSERT_EQ(d.hostIn(0x8C), 0xA5);                   // port A (mirrored through $8F)
    ASSERT_EQ(d.hostIn(0x82) & 0x20, 0);
}

// The CRTC: OUT $00 selects a register, OUT $02 writes it, IN $03 reads it
// (R/W = A0, RS = A1); the STS port's 80-column bit
TEST_CASE(dvi_crtc_ports) {
    Dvi d(idleBootRom(), testCharRom());
    d.portOut(0x00, 1);
    d.portOut(0x02, 80);
    ASSERT_EQ(d.crtcReg(1), 80);
    d.portOut(0x01, 99);                               // a read cycle: no write
    ASSERT_EQ(d.crtcReg(1), 80);
    d.portOut(0x00, 14);
    d.portOut(0x02, 0x12);
    ASSERT_EQ(d.portIn(0x03), 0x12);                   // the cursor registers read back
    d.portOut(0x00, 1);
    ASSERT_EQ(d.portIn(0x03), 0x00);                   // the others don't
    ASSERT(!d.cols80());
    d.portOut(0x20, 0x01);
    ASSERT(d.cols80());
}

// The picture: CRAM through the character ROM (code x 8 + raster, D7 the
// left dot), 40 columns twice as wide; ARAM bit 0 clear reverses
TEST_CASE(dvi_render) {
    Dvi d(idleBootRom(), testCharRom());
    const uint8_t regs[16] = {63, 40, 50, 0xB5, 31, 6, 25, 28, 0x50, 7, 0x20, 7, 0, 0, 0, 0};
    for (int r = 0; r < 16; ++r) { d.portOut(0x00, (uint8_t)r); d.portOut(0x02, regs[r]); }
    for (int i = 0; i < 2000; ++i) { d.busWrite((uint16_t)(0xE000 + i), ' '); d.busWrite((uint16_t)(0xC000 + i), 1); }
    d.busWrite(0xE000, 'A');                           // row 0, column 0
    d.busWrite(0xE001 + 40, 'A');                      // row 1, column 1
    d.busWrite(0xC001 + 40, 0);                        // reversed
    ASSERT_EQ(d.rowText(0).substr(0, 2), "A ");
    std::vector<uint32_t> px((size_t)Dvi::OUT_W * Dvi::OUT_H);
    d.render(px.data());
    auto lit = [&](int x, int y) { return (px[(size_t)y * Dvi::OUT_W + x] & 0xFFFFFF) != 0; };
    ASSERT(lit(0, 0) && lit(7, 1));                    // 'A': 4 dots x 2 wide
    ASSERT(!lit(8, 0));
    ASSERT(!lit(16, 0));                               // a space
    ASSERT(!lit(16, 16) && lit(24, 16));               // reversed 'A' at row 1, column 1 (dots 16-31)
    d.portOut(0x20, 0x01);                             // 80 columns: a dot is one pixel
    d.portOut(0x00, 1);
    d.portOut(0x02, 80);
    d.render(px.data());
    ASSERT(lit(3, 0) && !lit(4, 0));
}

// The FDC at $40-$5F: a Read Sector ($84 at $50) and the data read at $43,
// each IN waiting (the box's /WAIT) until DRQ; INTRQ at the end is the NMI;
// the drives' motors and STS bit 7 follow the FDC's head load
TEST_CASE(dvi_fdc_read_with_wait) {
    Dvi d(idleBootRom(), testCharRom());
    const std::string disk = rawDisk("raw.dsk");
    ASSERT(d.mountDisk(0, disk));
    d.portOut(0x20, 0x10);                             // drive 0
    for (int i = 0; i < 200000 && d.fdc().busy(); ++i) d.advance(64);     // the reset's Restore
    d.portOut(0x53, 5);
    d.portOut(0x50, 0x18);                             // Seek to the data register's track
    for (int i = 0; i < 200000 && d.fdc().busy(); ++i) d.advance(64);
    ASSERT_EQ(d.fdc().driveCylinder(0), 5);
    d.portOut(0x52, 3);                                // sector 3
    d.portOut(0x50, 0x84);                             // Read Sector (E: settle)
    ASSERT(d.portIn(0x20) & 0x80);                     // HLD: the motor runs
    bool ok = true;
    for (int i = 0; i < 256; ++i) {
        const uint8_t v = d.portIn(0x43);              // waits for each byte
        if (v != (uint8_t)(5 * 18 + 3 + i)) { ok = false; std::printf("  byte %d = %02X\n", i, v); break; }
    }
    ASSERT(ok);
    const uint8_t st = d.portIn(0x40);                 // waits for the end (INTRQ), reads the status
    ASSERT_EQ(st & 0x1D, 0x00);                        // not busy, no error
    ASSERT(!d.fdc().intrq());                          // (reading the status clears it)
    std::remove(disk.c_str());
}

TEST_CASE(dvi_snapshot) {
    Dvi d(idleBootRom(), testCharRom());
    d.portOut(0x20, 0x03);
    d.busWrite(0x0123, 0x44);
    d.busWrite(0xE010, 'Q');
    d.portOut(0x00, 6);
    d.portOut(0x02, 24);
    d.hostOut(0x80, 0x77);
    const auto s = d.saveState();
    Dvi e(idleBootRom(), testCharRom());
    ASSERT(e.loadState(s));
    ASSERT(e.bank1() && e.cols80());
    ASSERT_EQ(e.busRead(0x0123), 0x44);
    ASSERT_EQ(e.busRead(0xE010), 'Q');
    ASSERT_EQ(e.crtcReg(6), 24);
    ASSERT(e.intPending());
    ASSERT_EQ(e.portIn(0x60), 0x77);
    ASSERT(!e.loadState({1, 2, 3}));
}

// The Model 100's sysbus slot: the DVI takes ports $80-$8F (and the RAMPAC's
// place), shows its monitor under the LCD, and takes disk units 1 and 2
TEST_CASE(dvi_m100_slot) {
    if (!have("dvi.rom") || !have("dvichar.rom")) return;
    MachineDescriptor* m = make(Model::M100, idleRom(Model::M100));
    ASSERT(m != nullptr);
    auto* sys = m100::systemOf(m);
    const auto lcdOnly = sys->display.getDimensions();
    std::string err;
    auto* slot = m->expansionSlot("sysbus");
    ASSERT(slot->install("rampac", "", err));
    ASSERT(slot->install("dvi", "", err));
    ASSERT(sys->dvi() != nullptr && sys->rampac() == nullptr);
    ASSERT_EQ(sys->in(0x82) & 0x07, 0);                 // the ROM's check
    ASSERT(sys->display.getDimensions().height > lcdOnly.height);
    const std::string disk = rawDisk("slot.dsk");
    ASSERT(m->ioRegistry->mountDisk(2, disk) != nullptr);
    ASSERT_EQ(sys->dvi()->diskPath(1), disk);
    ASSERT(slot->info().find("dvi") == 0);
    ASSERT(slot->install("", "", err));
    ASSERT(sys->dvi() == nullptr);
    ASSERT_EQ(sys->in(0x82), 0xFF);
    std::remove(disk.c_str());
    delete m;
}

// The real thing: Model 100 + DVI + the system diskette. At power-on the
// Model 100's ROM finds the DVI and runs the IPL the box sends, which loads
// DISK-BASIC (4.6 KB less free); the box shows its banner. Then LFILES,
// SAVE / LOAD on the disk, SCREEN 1,1 to the monitor and WIDTH 80
TEST_CASE(dvi_real_system_disk) {
    if (!have("m100.rom") || !have("dvi.rom") || !have("dvichar.rom") || !have("dvi_system.dsk")) return;
    const std::string disk = copyOf(m100::firmwareDir() + "/dvi_system.dsk", "system.dsk");
    {
        std::string err;
        MachineDescriptor* m = m100::create(Model::M100, &err);
        ASSERT(m != nullptr);
        ASSERT(m->expansionSlot("sysbus")->install("dvi", disk, err));
        auto* sys = m100::systemOf(m);
        Dvi* d = sys->dvi();
        auto lcd = [&]() { return lcdText(m); };
        auto crt = [&]() { return dviText(d); };
        ASSERT(waitFor(m, crt, "MICROSOFT", 3));
        ASSERT(waitFor(m, lcd, "24941 Bytes free", 10));    // 29638 without DISK-BASIC
        sys->keyboard.enqueueText("\\n");                    // BASIC
        ASSERT(waitFor(m, lcd, "Ok", 5));
        sys->keyboard.enqueueText("LFILES 0\\n");
        ASSERT(waitFor(m, lcd, "139.5 K AVAILABLE", 15));
        EXPECT_TRUE(lcd().find("SYSTEM VER 01.00.00.200.") != std::string::npos);
        sys->keyboard.enqueueText("10 PRINT \"SAVED\";6*7\\nSAVE \"0:HELLO\"\\n");
        runFor(m, 10);
        sys->keyboard.enqueueText("NEW\\nLOAD \"0:HELLO\"\\n");
        runFor(m, 8);
        sys->keyboard.enqueueText("RUN\\n");
        ASSERT(waitFor(m, lcd, "SAVED 42", 5));
        sys->keyboard.enqueueText("SCREEN 1,1\\n");
        runFor(m, 2);
        sys->keyboard.enqueueText("PRINT \"ON THE MONITOR\";6*7\\n");
        ASSERT(waitFor(m, crt, "ON THE MONITOR 42", 5));
        ASSERT(!d->cols80());
        sys->keyboard.enqueueText("WIDTH 80\\nLFILES 0\\n");
        ASSERT(waitFor(m, crt, "HELLO", 15));
        EXPECT_TRUE(d->cols80());
        EXPECT_EQ(d->crtcReg(1), 80);
        delete m;                                        // the disk is written back
    }
    // The file is on the image: a fresh machine lists it
    std::string err;
    MachineDescriptor* m = m100::create(Model::M100, &err);
    ASSERT(m->expansionSlot("sysbus")->install("dvi", disk, err));
    auto* sys = m100::systemOf(m);
    auto lcd = [&]() { return lcdText(m); };
    ASSERT(waitFor(m, lcd, "Bytes free", 10));
    sys->keyboard.enqueueText("\\nLFILES 0\\n");
    EXPECT_TRUE(waitFor(m, lcd, "HELLO", 15));
    delete m;
    std::remove(disk.c_str());
}
