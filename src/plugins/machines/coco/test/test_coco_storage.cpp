#include "test_harness.h"
#include "coco_test_rom.h"
#include "plugins/machines/coco/main/coco_storage.h"
#include "plugins/devices/wd17xx/main/wd17xx.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

// The CoCo SDC and the Burke & Burke interface on the cartridge port (#356):
// attached as the CLI's `expansion` does, a stand-in "SDC-DOS" in the SDC's
// flash run by the 6809, and the XT controller through a Multi-Pak on a CoCo 3.

using namespace cocotest;

namespace {

namespace fs = std::filesystem;

std::string tempDir() {
    char t[] = "/tmp/coco_storage_XXXXXX";
    return mkdtemp(t);
}

void writeFile(const std::string& path, const std::vector<uint8_t>& bytes) {
    std::ofstream f(path, std::ios::binary);
    f.write((const char*)bytes.data(), (std::streamsize)bytes.size());
}

/** BASIC's stand-in that just goes to the cartridge ($C000). */
std::vector<uint8_t> jumpToCart() {
    Asm a(0xA000);
    a.emit({0x10, 0xCE, 0x7F, 0x00});           // LDS #$7F00
    a.ext(0x7E, 0xC000);                        // JMP $C000
    a.word(0xBFFE, 0xA000);
    return a.b;
}

/** The SDC's bank 0: read LSN 1 of drive 0 in command mode to $0600, mark $0701. */
std::vector<uint8_t> sdcProgram() {
    Asm a(0xC000, 0x4000, 0xFF);
    a.emit({0x86, 0x43}); a.ext(0xB7, 0xFF40);  // command mode
    a.ext(0x7F, 0xFF49);                        // LSN $000001
    a.ext(0x7F, 0xFF4A);
    a.emit({0x86, 0x01}); a.ext(0xB7, 0xFF4B);
    a.emit({0x86, 0x80}); a.ext(0xB7, 0xFF48);  // read sector, drive 0
    a.emit({0x8E, 0x06, 0x00});                 // LDX #$0600
    const uint16_t wait = a.pc;
    a.ext(0xB6, 0xFF48);                        // LDA status
    a.emit({0x85, 0x02});                       // BITA #READY
    a.branch(0x27, wait);                       // BEQ wait
    const uint16_t loop = a.pc;
    a.ext(0xFC, 0xFF4A);                        // LDD $FF4A
    a.emit({0xED, 0x81});                       // STD ,X++
    a.ext(0xB6, 0xFF48);
    a.emit({0x85, 0x02});
    a.branch(0x26, loop);                       // BNE loop
    a.ext(0x7F, 0xFF40);                        // back to floppy mode
    a.emit({0x86, 0x5D}); a.ext(0xB7, CART_MARK);
    const uint16_t spin = a.pc;
    a.branch(0x20, spin);
    return a.b;
}

}  // namespace

// A CoCo 2 with the SDC: its firmware runs from flash and reads a hard disk image by LSN
TEST_CASE(coco_sdc_cart_reads_lsn_from_flash) {
    const std::string dir = tempDir();
    std::vector<uint8_t> img(2900 * 256);
    for (size_t i = 0; i < img.size(); ++i) img[i] = (uint8_t)(i * 5 + 1);
    writeFile(dir + "/DISK.VHD", img);
    const std::string fw = dir + "/sdcdos.rom";
    writeFile(fw, sdcProgram());

    coco::Roms roms;
    roms.colorBasic = jumpToCart();
    std::string err;
    MachineDescriptor* m = coco::create(coco::Model::CoCo2, &err, 64, &roms);
    ASSERT(m);
    ASSERT(m->expansionSlot("cart")->install("sdc", dir + "," + fw, err));
    auto* sdc = dynamic_cast<coco::CocoSdcCart*>(coco::systemOf(m)->cart());
    ASSERT(sdc);
    ASSERT(sdc->mountDisk(0, dir + "/DISK.VHD"));          // as `disk mount 1 DISK.VHD`
    ASSERT(sdc->describe().find("DISK.VHD (LBA)") != std::string::npos);
    m->onReset(*m);
    run(m, 200000);
    IBus* bus = m->buses[0].bus;
    ASSERT_EQ(bus->peek8(CART_MARK), 0x5D);
    for (int i = 0; i < 256; ++i) ASSERT_EQ(bus->peek8((uint16_t)(0x0600 + i)), img[256 + i]);
    ASSERT(!sdc->sdc().commandMode());
    // Floppy mode: $FF48 is the WD1793's status again
    ASSERT_EQ(bus->peek8(0xFF48), sdc->floppyHalf().controller().statusValue());
    delete m;
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// The SDC's floppy half: a .DSK mounted is drive 0 of the WD1793 (Disk BASIC's view)
TEST_CASE(coco_sdc_cart_floppy_mode) {
    const std::string dir = tempDir();
    std::vector<uint8_t> img(35 * 18 * 256, 0xE5);
    writeFile(dir + "/GAME.DSK", img);
    coco::CocoSdcCart c({}, dir);
    c.reset();
    ASSERT(c.mountDisk(0, dir + "/GAME.DSK"));
    ASSERT(c.floppyHalf().controller().disk(0) != nullptr);
    uint8_t d = 0;
    ASSERT(!c.read(0xC000, true, false, d));               // no firmware: the ROM area is open
    c.write(0xFF40, false, true, 0x09);                    // drive 0, motor on
    ASSERT_EQ(c.floppyHalf().latch(), 0x09);
    ASSERT(c.floppyHalf().controller().motor());
    c.write(0xFF4B, false, true, 0x42);                    // the WD1793's data register
    ASSERT_EQ(c.floppyHalf().controller().dataRegister(), 0x42);
    // Command mode leaves the latch alone and takes $FF48-$FF4B
    c.write(0xFF40, false, true, 0x43);
    ASSERT_EQ(c.floppyHalf().latch(), 0x09);
    c.write(0xFF4B, false, true, 0x99);
    ASSERT_EQ(c.floppyHalf().controller().dataRegister(), 0x42);
    ASSERT_EQ(c.sdc().param(3), 0x99);
    // $FF50 is not the SDC's
    ASSERT(!c.read(0xFF58, false, true, d));
    ASSERT(c.ejectDisk(0));
    ASSERT(c.floppyHalf().controller().disk(0) == nullptr);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// A CoCo 3 with a Multi-Pak: Burke & Burke in slot 1 at $FF50, the SDC in slot 4
TEST_CASE(coco3_mpi_burke_and_sdc) {
    const std::string dir = tempDir();
    std::vector<uint8_t> hd(4 * 17 * 512 * 2);
    for (size_t i = 0; i < hd.size(); ++i) hd[i] = (uint8_t)(i ^ (i >> 9));
    writeFile(dir + "/os9.img", hd);

    coco::Roms roms;
    Asm a(0x8000, 0x8000, 0x12);
    a.emit({0x10, 0xCE, 0x7F, 0x00});
    const uint16_t spin = a.pc;
    a.branch(0x20, spin);
    a.word(0xFFFE, 0x8000);
    roms.coco3Basic = a.b;
    std::string err;
    MachineDescriptor* m = coco::create(coco::Model::CoCo3, &err, 128, &roms);
    ASSERT(m);
    ASSERT(m->expansionSlot("cart")->install("mpi", "4", err));
    ASSERT(m->expansionSlot("mpi1")->install("burke", dir + "/os9.img", err));
    ASSERT(m->expansionSlot("mpi4")->install("sdc", dir, err));
    ASSERT(!m->expansionSlot("mpi2")->install("burke", dir + "/missing.img", err));
    m->onReset(*m);
    run(m, 1000);
    IBus* bus = m->buses[0].bus;
    bus->write8(0xFF7F, 0x30);                            // /SCS to slot 1, /CTS slot 4
    // Read cylinder 0, head 1, sector 2 (LBA 19)
    bus->write8(0xFF51, 0);                               // reset
    bus->write8(0xFF52, 0);                               // select
    ASSERT_EQ(bus->read8(0xFF51) & 0x0F, 0x0D);           // BUSY, C/D, REQ
    for (int b : {0x08, 0x01, 0x02, 0x00, 0x01, 0x00}) bus->write8(0xFF50, (uint8_t)b);
    for (int i = 0; i < 512; ++i) {
        ASSERT_EQ(bus->read8(0xFF51) & 0x0F, 0x0B);       // BUSY, I/O, REQ
        ASSERT_EQ(bus->read8(0xFF50), hd[19 * 512 + i]);
    }
    ASSERT_EQ(bus->read8(0xFF51) & 0x0F, 0x0F);
    ASSERT_EQ(bus->read8(0xFF50), 0x00);                  // completion: no error
    // The SDC in slot 4 (its $FF40 needs INIT0's SCS bit on a CoCo 3)
    bus->write8(0xFF90, 0x04);
    bus->write8(0xFF7F, 0x33);
    bus->write8(0xFF40, 0x43);
    bus->write8(0xFF49, 'Z');
    bus->write8(0xFF48, 0xC0);
    ASSERT_EQ(bus->read8(0xFF48), CocoSdc::ST_FAILED);
    delete m;
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// Burke & Burke's BIOS ROM socket: on /CTS with A4-A6 inverted; the controller only at $FF50-$FF53
TEST_CASE(coco_burke_rom_socket) {
    std::vector<uint8_t> rom(0x2000);
    for (size_t i = 0; i < rom.size(); ++i) rom[i] = (uint8_t)(i >> 4);
    coco::BurkeBurke bb(rom);
    uint8_t d = 0;
    ASSERT(bb.read(0xC000, true, false, d));
    ASSERT_EQ(d, rom[0x0070]);                            // A4-A6 inverted
    ASSERT(bb.read(0xC075, true, false, d));
    ASSERT_EQ(d, rom[0x0005]);
    ASSERT(bb.read(0xE010, true, false, d));              // an 8 KB EPROM repeats
    ASSERT_EQ(d, rom[0x0060]);
    ASSERT(bb.read(0xFF51, false, true, d));
    ASSERT_EQ(d, 0);                                      // idle status
    ASSERT(!bb.read(0xFF54, false, true, d));
    ASSERT(!bb.read(0xFF40, false, true, d));
    coco::BurkeBurke none;
    ASSERT(!none.read(0xC000, true, false, d));
}
