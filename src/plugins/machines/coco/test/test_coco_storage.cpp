#include "test_harness.h"
#include "coco_test_rom.h"
#include "plugins/machines/coco/main/coco_storage.h"
#include "libdevices/main/device_info.h"
#include "plugins/devices/wd17xx/main/wd17xx.h"
#include <cstdio>
#include <ctime>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

// The CoCo SDC and the Burke & Burke interface on the cartridge port (#356):
// attached as the CLI's `expansion` does, a stand-in "SDC-DOS" in the SDC's
// flash run by the 6809, the XT controller through a Multi-Pak on a CoCo 3, and
// the Disto Super Controller II.

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

namespace {

// A fixed time for the XT-RTC's DS1215: Friday 25 December 2026, 12:34:56.42
void fixClock(coco::BurkeBurke& bb) {
    bb.clock().now = []() {
        std::tm t{};
        t.tm_sec = 56; t.tm_min = 34; t.tm_hour = 12;
        t.tm_mday = 25; t.tm_mon = 11; t.tm_year = 126; t.tm_wday = 5;
        return t;
    };
    bb.clock().hundredths = []() { return 42; };
}

const uint8_t XT_RTC_TIME[8] = {0x42, 0x56, 0x34, 0x12, 0x06, 0x25, 0x12, 0x26};   // BCD, low bit first
const uint8_t DS1215_PATTERN[8] = {0xC5, 0x3A, 0xA3, 0x5C, 0xC5, 0x3A, 0xA3, 0x5C};

}  // namespace

// The XT-RTC's DS1215 (schematic U4: /CEI from /SCS, /WE = A2, D = A0, Q =
// D0, ROM mode): any $FF40-$FF5F cycle with A2 low writes bit A0, read or
// write; a read with A2 high reads a bit; a write with A2 high is nothing
TEST_CASE(coco_burke_xt_rtc_clock_bits) {
    coco::BurkeBurke bb;
    fixClock(bb);
    bb.reset();
    uint8_t d = 0;
    ASSERT(!bb.read(0xFF44, false, true, d));             // closed: not answering
    int n = 0;
    for (uint8_t byte : DS1215_PATTERN)
        for (int i = 0; i < 8; ++i, ++n) {
            const int bit = byte >> i & 1;
            if (n & 1) bb.write((uint16_t)(0xFF4A | bit), false, true, 0x00);   // a write cycle, A2 low
            else bb.read((uint16_t)(0xFF48 | bit), false, true, d);             // a read cycle, A2 low
            bb.write(0xFF5C, false, true, 0xFF);                                 // A2 high write: ignored
            ASSERT(!bb.peek(0xFF44, false, true, d));                            // a peek: no cycle
        }
    for (int i = 0; i < 64; ++i) {
        ASSERT(bb.read(i & 1 ? 0xFF44 : 0xFF5D, false, true, d));
        ASSERT_EQ(d, (XT_RTC_TIME[i / 8] >> (i % 8)) & 1);
    }
    ASSERT(!bb.read(0xFF44, false, true, d));             // closed again after 64 bits
    // The controller is undisturbed and still answers its own ports
    ASSERT(bb.read(0xFF51, false, true, d));
    ASSERT_EQ(d, 0);
}

// A 6809 program in a stand-in XT-ROM (burned scrambled, as the real one
// is) unlocks the DS1215 with reads and reads the time to $0600 on a CoCo 2
TEST_CASE(coco_burke_xt_rtc_read_by_6809) {
    Asm a(0xC000);
    a.ext(0x7D, 0xFF44);                         // TST $FF44: a read resets the pattern
    a.emit({0x8E, 0xC1, 0x00});                  // LDX #pattern
    const uint16_t outer = a.pc;
    a.emit({0xA6, 0x80});                        // LDA ,X+
    a.emit({0xC6, 0x08});                        // LDB #8
    const uint16_t inner = a.pc;
    a.emit({0x44});                              // LSRA
    a.emit({0x25, 0x05});                        // BCS one
    a.ext(0x7D, 0xFF40);                         // TST $FF40 (A2 low, A0 = 0)
    a.emit({0x20, 0x03});                        // BRA next
    a.ext(0x7D, 0xFF41);                         // one: TST $FF41 (A0 = 1)
    a.emit({0x5A});                              // next: DECB
    a.branch(0x26, inner);
    a.emit({0x8C, 0xC1, 0x08});                  // CMPX #pattern+8
    a.branch(0x26, outer);
    a.emit({0xCE, 0x06, 0x00});                  // LDU #$0600
    const uint16_t outer2 = a.pc;
    a.emit({0xC6, 0x08});                        // LDB #8
    const uint16_t inner2 = a.pc;
    a.ext(0xB6, 0xFF44);                         // LDA $FF44 (A2 high)
    a.emit({0x44});                              // LSRA
    a.emit({0x66, 0xC4});                        // ROR ,U
    a.emit({0x5A});                              // DECB
    a.branch(0x26, inner2);
    a.emit({0x33, 0x41});                        // LEAU 1,U
    a.emit({0x11, 0x83, 0x06, 0x08});            // CMPU #$0608
    a.branch(0x26, outer2);
    a.emit({0x86, 0x5D}); a.ext(0xB7, CART_MARK);
    const uint16_t spin = a.pc;
    a.branch(0x20, spin);
    a.pc = 0xC100;
    for (uint8_t b : DS1215_PATTERN) a.emit({b});
    std::vector<uint8_t> eprom(a.b.size());
    for (size_t i = 0; i < a.b.size(); ++i) eprom[i ^ 0x70] = a.b[i];   // as burned: A4-A6 inverted
    const std::string dir = tempDir();
    writeFile(dir + "/xt.rom", eprom);

    coco::Roms roms;
    roms.colorBasic = jumpToCart();
    std::string err;
    MachineDescriptor* m = coco::create(coco::Model::CoCo2, &err, 64, &roms);
    ASSERT(m);
    ASSERT(m->expansionSlot("cart")->install("burke", "," + dir + "/xt.rom", err));
    auto* bb = dynamic_cast<coco::BurkeBurke*>(coco::systemOf(m)->cart());
    ASSERT(bb);
    fixClock(*bb);
    m->onReset(*m);
    run(m, 100000);
    IBus* bus = m->buses[0].bus;
    ASSERT_EQ(bus->peek8(CART_MARK), 0x5D);
    for (int i = 0; i < 8; ++i) ASSERT_EQ(bus->peek8((uint16_t)(0x0600 + i)), XT_RTC_TIME[i]);
    delete m;
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// Disto Super Controller II
// ---------------------------------------------------------------------------

namespace {

/** A 35-track single-sided JVC image: track t, sector s (1-18) filled from f(t, s, i). */
std::string sc2Image(const std::string& dir, uint8_t (*f)(int, int, int)) {
    std::vector<uint8_t> img(35 * 18 * 256);
    for (int t = 0; t < 35; ++t)
        for (int s = 1; s <= 18; ++s)
            for (int i = 0; i < 256; ++i) img[(size_t)((t * 18 + s - 1) * 256 + i)] = f(t, s, i);
    const std::string path = dir + "/sc2.dsk";
    writeFile(path, img);
    return path;
}

uint8_t sc2Pattern(int t, int s, int i) { return (uint8_t)(t * 31 + s * 7 + i * 3); }

/** A board on the Mini-Expansion Bus for the tests: 8 bytes of latches, its FIRQ by hand. */
struct TestMeb : coco::MebDevice {
    uint8_t regs[8] = {};
    bool wasReset = false;
    std::string describe() const override { return "test board"; }
    void reset() override { wasReset = true; }
    uint8_t read(int r) override { return regs[r]; }
    uint8_t peek(int r) const override { return regs[r]; }
    void write(int r, uint8_t v) override { regs[r] = v; }
};

struct Sc2Rig {
    coco::DistoSc2 sc;
    bool halt = false, nmi = false, cart = true;
    int nmis = 0, halts = 0;
    explicit Sc2Rig(bool alt = false) : sc(std::vector<uint8_t>(0x2000, 0x39), alt) {
        sc.setHalt = [this](bool on) { if (on) ++halts; halt = on; };
        sc.setNmi = [this](bool on) { if (on && !nmi) ++nmis; nmi = on; };
        sc.setCart = [this](bool level) { cart = level; };
        sc.reset();
    }
    bool tickUntil(const std::function<bool()>& done, uint64_t limit = 14318180ull * 2) {
        for (uint64_t t = 0; t < limit && !done(); t += 16) sc.clock(16);
        return done();
    }
    uint8_t rd(uint16_t a) {
        uint8_t d = 0xFF;
        sc.read(a, false, a >= 0xFF40 && a < 0xFF60, d);
        return d;
    }
    void wr(uint16_t a, uint8_t v) { sc.write(a, false, a >= 0xFF40 && a < 0xFF60, v); }
    /** Seek with the latch's DD + NMI enable set and the NMI masked, as NitrOS-9's rb1773 for the SC-II. */
    void seek(int track) {
        wr(0xFF76, coco::DistoSc2::CTRL_MASK_NMI);
        wr(0xFF40, 0x29);
        wr(0xFF4B, (uint8_t)track);
        wr(0xFF48, 0x10);
        tickUntil([&] { return !(rd(0xFF76) & 0x80); });
        rd(0xFF48);
    }
};

}  // namespace

// The SC-II's buffered ("no halt") read, as NitrOS-9's rb1773 does it: READ SECTOR,
// then $07 to $FF76 (read, buffered, NMI masked); the board takes the 256 bytes from
// the WD1773 itself, the CPU never halted and no NMI; $FF76 bit 7 goes low with
// INTRQ; $04 to $FF76 clears the counter and the CPU reads the buffer at $FF74
TEST_CASE(coco_sc2_buffered_read_without_halt) {
    const std::string dir = tempDir();
    Sc2Rig r;
    ASSERT_EQ(r.sc.kind(), std::string("sc2"));
    ASSERT(r.sc.controller().variant() == Wd17xx::Variant::WD1773);
    ASSERT(r.sc.mountDisk(0, sc2Image(dir, sc2Pattern)));
    ASSERT_EQ(r.rd(0xFF76) & 0x80, 0x80);                 // no interrupt
    r.seek(12);
    ASSERT_EQ(r.sc.controller().trackRegister(), 12);
    r.wr(0xFF4A, 5);
    r.wr(0xFF48, 0x80);                                    // READ SECTOR (the command first, as rb1773)
    r.wr(0xFF76, 0x07);
    r.wr(0xFF40, 0xA9);                                    // even with the latch's HALT enable set
    ASSERT(r.tickUntil([&] { return !(r.rd(0xFF76) & 0x80); }));
    ASSERT(!r.halt);
    ASSERT_EQ(r.halts, 0);
    ASSERT_EQ(r.nmis, 0);                                  // INTRQ masked from /NMI
    ASSERT_EQ(r.sc.bufferCounter(), 0);                    // 256 DRQs served: the 8-bit counter wrapped
    ASSERT_EQ(r.rd(0xFF48) & 0x1C, 0);                     // no RNF, CRC or lost data
    r.wr(0xFF76, coco::DistoSc2::CTRL_MASK_NMI);           // normal mode; clears the counter
    for (int i = 0; i < 256; ++i) ASSERT_EQ(r.rd((uint16_t)(0xFF74 + (i & 1))), sc2Pattern(12, 5, i));   // $FF75 mirrors $FF74
    ASSERT_EQ(r.sc.bufferCounter(), 0);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// A buffered write: the sector loaded into the buffer through $FF74 first ($04 to
// $FF76 clears the counter), then WRITE SECTOR and $06 (write, buffered, NMI masked):
// the WD1773's DRQs take the buffer's bytes; read back in normal mode with HALT, as
// Disk BASIC on an FD-502, with INTRQ's NMI now through (bit 2 clear)
TEST_CASE(coco_sc2_buffered_write) {
    const std::string dir = tempDir();
    Sc2Rig r;
    ASSERT(r.sc.mountDisk(0, sc2Image(dir, sc2Pattern)));
    r.seek(3);
    r.wr(0xFF76, coco::DistoSc2::CTRL_MASK_NMI);
    for (int i = 0; i < 256; ++i) r.wr(0xFF74, (uint8_t)(0xA5 ^ i));
    r.wr(0xFF4A, 9);
    r.wr(0xFF48, 0xA0);                                    // WRITE SECTOR
    r.wr(0xFF77, 0x06);                                    // $FF77 mirrors $FF76: also clears the counter
    ASSERT(r.tickUntil([&] { return !(r.rd(0xFF76) & 0x80); }));
    ASSERT_EQ(r.halts, 0);
    ASSERT_EQ(r.nmis, 0);
    ASSERT_EQ(r.rd(0xFF48) & 0x5C, 0);                     // no write protect, RNF, CRC or lost data
    // Back in normal mode the board is an FD-502: read it back a byte per DRQ with HALT
    r.wr(0xFF76, 0x00);
    r.wr(0xFF4A, 9);
    r.wr(0xFF40, 0xA9);
    r.wr(0xFF48, 0x80);
    std::vector<uint8_t> got;
    while (got.size() < 256) {
        ASSERT(r.tickUntil([&] { return !r.halt || r.nmi; }));
        if (r.nmi) break;
        got.push_back(r.rd(0xFF4B));
        r.wr(0xFF40, 0xA9);
    }
    ASSERT_EQ(got.size(), 256u);
    for (int i = 0; i < 256; ++i) ASSERT_EQ(got[i], (uint8_t)(0xA5 ^ i));
    ASSERT(r.tickUntil([&] { return r.nmi; }));             // INTRQ: NMI (not masked now)
    ASSERT_EQ(r.nmis, 1);
    ASSERT(r.halts > 0);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// The SC-II's address map (manual table 1): latch $FF40-$FF47, WD1773 $FF48-$FF4F,
// the Mini-Expansion Bus $FF50-$FF57, nothing at $FF58-$FF5F unless jumper J3 moves
// the buffer registers there; control bit 3 puts INTRQ on CART (FIRQ), as does a MEB
// board through J2; bit 7 of the control register reads INTRQ inverted
TEST_CASE(coco_sc2_address_map_firq_and_meb) {
    Sc2Rig r;
    uint8_t d = 0;
    ASSERT(r.sc.read(0xC000, true, false, d));
    ASSERT_EQ(d, 0x39);                                    // the DOS socket
    r.wr(0xFF43, 0x09);                                    // latch mirror
    ASSERT_EQ(r.sc.latch(), 0x09);
    r.wr(0xFF4E, 7);                                       // the WD1773's sector register at $FF4A's mirror
    ASSERT_EQ(r.sc.controller().sectorRegister(), 7);
    ASSERT(!r.sc.read(0xFF5A, false, true, d));            // $FF58-$FF5F: not decoded
    r.wr(0xFF5A, 9);
    ASSERT_EQ(r.sc.controller().sectorRegister(), 7);      // (an FD-502 mirrors the FDC there)
    ASSERT(!r.sc.read(0xFF52, false, true, d));            // no MEB board: not driven
    // A write to the data register counts; one to control clears the counter
    r.wr(0xFF74, 1);
    r.wr(0xFF75, 2);
    ASSERT_EQ(r.sc.bufferCounter(), 2);
    r.wr(0xFF76, 0);
    ASSERT_EQ(r.sc.bufferCounter(), 0);
    ASSERT_EQ(r.rd(0xFF74), 1);
    ASSERT_EQ(r.rd(0xFF74), 2);
    // INTRQ to CART with bit 3 (Force Interrupt $D8: INTRQ at once)
    r.wr(0xFF76, coco::DistoSc2::CTRL_MASK_NMI);
    r.wr(0xFF48, 0xD8);
    ASSERT(r.sc.controller().intrq());
    ASSERT(r.cart);                                        // FIRQ masked
    ASSERT_EQ(r.rd(0xFF76) & 0x80, 0x00);                  // INTRQ, inverted
    r.wr(0xFF76, coco::DistoSc2::CTRL_MASK_NMI | coco::DistoSc2::CTRL_FIRQ);
    ASSERT(!r.cart);                                       // CART pulled low
    r.wr(0xFF48, 0xD0);                                    // ends $D8's INTRQ
    r.rd(0xFF48);
    ASSERT(!r.sc.controller().intrq());
    ASSERT(r.cart);
    ASSERT_EQ(r.rd(0xFF76) & 0x80, 0x80);
    // NMI: unmasking with INTRQ pending and DD set raises it
    r.wr(0xFF40, 0x29);
    r.wr(0xFF48, 0xD8);
    ASSERT_EQ(r.nmis, 0);
    r.wr(0xFF76, 0x00);
    ASSERT_EQ(r.nmis, 1);
    r.wr(0xFF48, 0xD0);
    r.rd(0xFF48);
    // A board on the MEB: $FF50-$FF57 (A0-A2), its FIRQ through J2
    auto* meb = new TestMeb();
    r.sc.plugMeb(std::unique_ptr<coco::MebDevice>(meb));
    r.wr(0xFF53, 0x5A);
    ASSERT_EQ(meb->regs[3], 0x5A);
    ASSERT_EQ(r.rd(0xFF53), 0x5A);
    ASSERT_EQ(r.sc.latch(), 0x29);                         // not the latch's mirror (an FD-502's)
    meb->setFirq(true);
    ASSERT(!r.cart);
    meb->setFirq(false);
    ASSERT(r.cart);
    r.sc.reset();
    ASSERT(meb->wasReset);
    ASSERT(r.sc.describe().find("MEB: test board") != std::string::npos);
    // Jumper J3 in 2-3: the registers at $FF58 (data) and $FF5A (control), not $FF74
    Sc2Rig alt(true);
    alt.wr(0xFF76, 0x0F);
    ASSERT_EQ(alt.sc.control(), 0);
    ASSERT(!alt.sc.read(0xFF74, false, false, d));
    alt.wr(0xFF58, 0x11);
    alt.wr(0xFF59, 0x22);
    alt.wr(0xFF5B, coco::DistoSc2::CTRL_MASK_NMI);         // control (A1 high); clears the counter
    ASSERT_EQ(alt.sc.control(), coco::DistoSc2::CTRL_MASK_NMI);
    ASSERT_EQ(alt.rd(0xFF5C), 0x11);                       // A2 not decoded
    ASSERT_EQ(alt.rd(0xFF58), 0x22);
    ASSERT_EQ(alt.rd(0xFF5A) & 0x80, 0x80);
}

namespace {

/**
 * The 6809 running a no-halt read from the SC-II's DOS socket: READ SECTOR of
 * track 0 sector 1, $07 to $FF76, then a busy loop counting in Y until $FF76
 * bit 7 drops (the CPU is free meanwhile), $04 to $FF76 and 256 x LDA $FF74 to
 * $0600. Marks $0701 = $5D with the WD1773 status at $0702 and Y at $0703; an
 * NMI marks $0701 = $EE.
 */
std::vector<uint8_t> sc2Dos() {
    Asm d(0xC000, 0x2000, 0x12);
    d.emit({0x10, 0xCE, 0x7F, 0x00});                        // LDS #$7F00
    d.emit({0x1A, 0x50});                                    // ORCC #$50
    d.emit({0x86, 0x04}); d.ext(0xB7, 0xFF76);               // normal mode, NMI masked
    d.emit({0x86, 0x29}); d.ext(0xB7, 0xFF40);               // drive 0, motor, DD + NMI enable
    d.emit({0x86, 0x03}); d.ext(0xB7, 0xFF48);               // RESTORE
    const uint16_t busy = d.pc;
    d.ext(0xB6, 0xFF48);
    d.emit({0x85, 0x01});
    d.branch(0x26, busy);
    d.emit({0x86, 0x01}); d.ext(0xB7, 0xFF4A);               // sector 1
    d.emit({0x86, 0x80}); d.ext(0xB7, 0xFF48);               // READ SECTOR
    d.emit({0x86, 0x07}); d.ext(0xB7, 0xFF76);               // read, buffered, NMI masked
    d.emit({0x10, 0x8E, 0x00, 0x00});                        // LDY #0
    const uint16_t wait = d.pc;
    d.emit({0x31, 0x21});                                    // LEAY 1,Y
    d.ext(0xB6, 0xFF76);                                     // LDA $FF76
    d.branch(0x2B, wait);                                    // BMI wait
    d.emit({0x86, 0x04}); d.ext(0xB7, 0xFF76);               // clear the counter
    d.emit({0x8E, 0x06, 0x00});                              // LDX #$0600
    d.emit({0x5F});                                          // CLRB
    const uint16_t copy = d.pc;
    d.ext(0xB6, 0xFF74);
    d.emit({0xA7, 0x80});                                    // STA ,X+
    d.emit({0x5A});                                          // DECB
    d.branch(0x26, copy);
    d.ext(0xB6, 0xFF48); d.ext(0xB7, CART_MARK + 1);         // status
    d.emit({0x10, 0xBF, (CART_MARK + 2) >> 8, (CART_MARK + 2) & 0xFF});   // STY $0703
    d.emit({0x86, 0x5D}); d.ext(0xB7, CART_MARK);
    const uint16_t spin = d.pc;
    d.branch(0x20, spin);
    d.pc = 0xC200;                                           // NMI
    d.emit({0x86, 0xEE}); d.ext(0xB7, CART_MARK);
    const uint16_t spin2 = d.pc;
    d.branch(0x20, spin2);
    return d.b;
}

}  // namespace

// On a CoCo 2 (`expansion cart sc2 ROM`) and in a CoCo 3's Multi-Pak slot (where
// $FF74 reaches every slot): the 6809 reads a sector in buffered mode, spinning
// while the board takes the bytes, no NMI
TEST_CASE(coco_sc2_cpu_reads_without_halt) {
    const std::string dir = tempDir();
    const std::string dsk = sc2Image(dir, sc2Pattern);
    writeFile(dir + "/sc2dos.rom", sc2Dos());
    for (bool coco3 : {false, true}) {
        coco::Roms roms;
        if (coco3) {
            Asm a(0x8000, 0x8000, 0x12);
            a.emit({0x10, 0xCE, 0x7F, 0x00});
            a.emit({0x86, 0x04}); a.ext(0xB7, 0xFF90);       // INIT0: SCS on, ROM 16K internal / 16K external
            a.ext(0x7E, 0xC000);
            a.word(0xFFFC, 0xC200);                          // NMI
            a.word(0xFFFE, 0x8000);
            roms.coco3Basic = a.b;
        } else {
            Asm a(0xA000);
            a.ext(0x7E, 0xC000);
            a.word(0xBFFC, 0xC200);
            a.word(0xBFFE, 0xA000);
            roms.colorBasic = a.b;
        }
        std::string err;
        MachineDescriptor* m = coco::create(coco3 ? coco::Model::CoCo3 : coco::Model::CoCo2, &err, coco3 ? 128 : 64, &roms);
        ASSERT(m);
        if (coco3) {
            ASSERT(m->expansionSlot("cart")->install("mpi", "2", err));
            ASSERT(m->expansionSlot("mpi2")->install("sc2", dir + "/sc2dos.rom", err));
        } else {
            ASSERT(m->expansionSlot("cart")->install("sc2", dir + "/sc2dos.rom", err));
        }
        coco::Cart* c = coco::systemOf(m)->cart();
        ASSERT(c->mountDisk(0, dsk));
        ASSERT(c->describe().find("Disto Super Controller II") != std::string::npos);
        m->onReset(*m);
        run(m, 3000000);
        IBus* bus = m->buses[0].bus;
        ASSERT_EQ(bus->peek8(CART_MARK), 0x5D);
        ASSERT_EQ(bus->peek8(CART_MARK + 1) & 0x1C, 0);
        const int spins = bus->peek8(CART_MARK + 2) << 8 | bus->peek8(CART_MARK + 3);
        ASSERT(spins > 100);                                  // the CPU ran while the sector came in
        for (int i = 0; i < 256; ++i) ASSERT_EQ(bus->peek8((uint16_t)(0x0600 + i)), sc2Pattern(0, 1, i));
        delete m;
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// The Becker port and the built-in DriveWire server
// ---------------------------------------------------------------------------
namespace {

/** A stand-in DriveWire DOS at $C000: OP_READEX of drive 0's LSN 1 through the
 *  Becker port into $0600 (as HDB-DOS's Becker DWRead: poll $FF41 bit 1, take
 *  $FF42), the checksum sent back, the server's status byte to $0702, $5D to $0701. */
std::vector<uint8_t> beckerReadProgram() {
    Asm a(0xC000, 0x2000, 0xFF);
    const uint16_t cmdRef = a.pc;
    a.emit({0x8E, 0x00, 0x00});                 // LDX #cmd (patched)
    a.emit({0xC6, 0x05});                       // LDB #5
    const uint16_t tx = a.pc;
    a.emit({0xA6, 0x80});                       // LDA ,X+
    a.ext(0xB7, 0xFF42);                        // STA $FF42
    a.emit({0x5A});                             // DECB
    a.branch(0x26, tx);                         // BNE
    a.emit({0x8E, 0x06, 0x00});                 // LDX #$0600
    const uint16_t rx = a.pc;
    a.ext(0xB6, 0xFF41);                        // LDA $FF41
    a.emit({0x85, 0x02});                       // BITA #2
    a.branch(0x27, rx);                         // BEQ rx
    a.ext(0xB6, 0xFF42);                        // LDA $FF42
    a.emit({0xA7, 0x80});                       // STA ,X+
    a.emit({0x8C, 0x07, 0x00});                 // CMPX #$0700
    a.branch(0x26, rx);                         // BNE rx
    a.emit({0x8E, 0x06, 0x00});                 // LDX #$0600
    a.emit({0xCC, 0x00, 0x00});                 // LDD #0
    const uint16_t sum = a.pc;
    a.emit({0xEB, 0x80});                       // ADDB ,X+
    a.emit({0x89, 0x00});                       // ADCA #0
    a.emit({0x8C, 0x07, 0x00});                 // CMPX #$0700
    a.branch(0x26, sum);                        // BNE sum
    a.ext(0xB7, 0xFF42);                        // STA $FF42 (checksum, MSB first)
    a.ext(0xF7, 0xFF42);                        // STB $FF42
    const uint16_t st = a.pc;
    a.ext(0xB6, 0xFF41);
    a.emit({0x85, 0x02});
    a.branch(0x27, st);
    a.ext(0xB6, 0xFF42);                        // the status byte
    a.ext(0xB7, 0x0702);
    a.emit({0x86, 0x5D}); a.ext(0xB7, CART_MARK);
    const uint16_t spin = a.pc;
    a.branch(0x20, spin);
    const uint16_t cmd = a.pc;
    a.emit({0xD2, 0x00, 0x00, 0x00, 0x01});     // OP_READEX, drive 0, LSN 1
    a.word((uint16_t)(cmdRef + 1), cmd);
    return a.b;
}

}  // namespace

// The Becker port's registers: $FF41 bit 1 while the server has a byte, $FF42 the data
TEST_CASE(coco_becker_port_registers) {
    coco::BeckerCart c;
    c.reset();
    uint8_t d = 0xAA;
    ASSERT(c.read(0xFF41, false, true, d));
    ASSERT_EQ(d, 0x00);
    c.write(0xFF42, false, true, DriveWireServer::OP_TIME);
    ASSERT(c.peek(0xFF41, false, true, d));
    ASSERT_EQ(d, 0x02);
    int n = 0;
    while (c.read(0xFF41, false, true, d) && (d & 2)) {
        ASSERT(c.read(0xFF42, false, true, d));
        ++n;
    }
    ASSERT_EQ(n, 6);
    // Only on /SCS, only $FF41/$FF42; no ROM: /CTS open
    c.write(0xFF42, false, true, DriveWireServer::OP_SERREAD);
    ASSERT(!c.read(0xFF41, false, false, d));
    ASSERT(!c.read(0xFF43, false, true, d));
    ASSERT(!c.read(0xFF40, false, true, d));
    ASSERT(!c.read(0xC000, true, false, d));
    c.write(0xFF41, false, true, DriveWireServer::OP_TIME);   // not the data register
    ASSERT_EQ((int)c.server().outputSize(), 2);
    // A reset (the CoCo's) drops what the server had for it
    c.reset();
    ASSERT(c.read(0xFF41, false, true, d));
    ASSERT_EQ(d, 0x00);
    // In a Multi-Pak slot: reached when $FF7F routes /SCS to it
    coco::MultiPak mpi;
    mpi.insert(1, std::unique_ptr<coco::Cart>(new coco::BeckerCart({0x39, 0x12})));
    mpi.reset();                                             // switch 4: slot 4
    ASSERT(!mpi.read(0xFF41, false, true, d));
    mpi.write(0xFF7F, false, false, 0x11);                   // /CTS and /SCS to slot 2
    mpi.write(0xFF42, false, true, DriveWireServer::OP_SERREAD);
    ASSERT(mpi.read(0xFF41, false, true, d));
    ASSERT_EQ(d, 0x02);
    ASSERT(mpi.read(0xC001, true, false, d));                // its ROM
    ASSERT_EQ(d, 0x12);
}

// A CoCo 2 with the Becker cartridge: a 6809 program in its ROM reads a sector over DriveWire
TEST_CASE(coco2_becker_readex_from_6809) {
    const std::string dir = tempDir();
    std::vector<uint8_t> img(630 * 256);
    for (size_t i = 0; i < img.size(); ++i) img[i] = (uint8_t)(i * 3 + (i >> 8));
    writeFile(dir + "/os9.dsk", img);
    writeFile(dir + "/dw.rom", beckerReadProgram());

    coco::Roms roms;
    roms.colorBasic = jumpToCart();
    std::string err;
    MachineDescriptor* m = coco::create(coco::Model::CoCo2, &err, 64, &roms);
    ASSERT(m);
    ASSERT(m->expansionSlot("cart")->install("becker", dir + "/dw.rom", err));
    auto* becker = dynamic_cast<coco::BeckerCart*>(coco::systemOf(m)->cart());
    ASSERT(becker);
    ASSERT(m->ioRegistry->mountDisk(1, dir + "/os9.dsk") != nullptr);   // `disk mount 1`: DriveWire drive 0
    ASSERT_EQ(becker->diskPath(0), dir + "/os9.dsk");
    ASSERT(becker->describe().find("ROM dw.rom; drive 0: os9.dsk") != std::string::npos);
    m->onReset(*m);
    run(m, 100000);
    IBus* bus = m->buses[0].bus;
    ASSERT_EQ(bus->peek8(CART_MARK), 0x5D);
    ASSERT_EQ(bus->peek8(0x0702), 0x00);                     // checksum accepted
    for (int i = 0; i < 256; ++i) ASSERT_EQ(bus->peek8((uint16_t)(0x0600 + i)), img[256 + i]);
    ASSERT_EQ((int)becker->server().stats().reads, 1);
    ASSERT(becker->server().idle());
    int track = -1;
    bool motor = false;
    ASSERT(becker->diskStatus(0, track, motor));
    ASSERT_EQ(track, 0);
    delete m;
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// DriveWire over the bit banger
// ---------------------------------------------------------------------------
namespace {

// The serial driver of a bit-banger DriveWire client, in the style of
// NitrOS-9's dwread/dwwrite and HDB-DOS's (our own code): DP = $FF,
// interrupts masked, each bit cell counted out in CPU cycles, 15 and 16 in
// turn: 15.5 cycles a bit, 57600 baud at 0.894886 MHz and 115200 at
// 1.789773 MHz. PA1 ($FF20 bit 1) out, PB0 ($FF22 bit 0) in.

/** DWWRITE: Y bytes from X, 8N1. Edge to edge: start 15, d0 16, d1 15, ... d7 17. */
void bbWrite(Asm& a) {
    const uint16_t byte = a.pc;
    a.emit({0x86, 0x04});                       // LDA #4          pairs of data bits
    a.emit({0x0F, 0x20});                       // CLR <$20        the start bit (CLR writes on its 6th cycle)
    a.emit({0xE6, 0x80});                       // LDB ,X+     6
    a.emit({0x58});                             // LSLB        2   d0 to bit 1, d7 to C
    a.emit({0x21, 0x00});                       // BRN         3
    const uint16_t loop = a.pc;
    a.emit({0xD7, 0x20});                       // STB <$20    4   d0 d2 d4 d6
    a.emit({0x56});                             // RORB        2   the next bit to bit 1
    a.emit({0x21, 0x00, 0x21, 0x00});           // BRN, BRN    6
    a.emit({0x12, 0x12});                       // NOP, NOP    4
    a.emit({0xD7, 0x20});                       // STB <$20    4   d1 d3 d5 d7: 16 after
    a.emit({0x56});                             // RORB        2
    a.emit({0x12, 0x12});                       // NOP, NOP    4
    a.emit({0x4A});                             // DECA        2
    a.branch(0x26, loop);                       // BNE loop    3   (15 to the next STB's write)
    a.emit({0x86, 0x02});                       // LDA #2      2
    a.emit({0x97, 0x20});                       // STA <$20    4   the stop bit
    a.emit({0x31, 0x3F});                       // LEAY -1,Y
    a.branch(0x26, byte);                       // BNE byte
    a.emit({0x39});                             // RTS
}

/** DWREAD: Y bytes to X, 8N1, no timeout. The start bit looked for every 7
 *  cycles; d0 read 20 cycles after it was seen (1.29-1.74 bits into the
 *  frame), the rest 15 or 16 apart; looking again 9.4-9.8 bits in. */
void bbRead(Asm& a) {
    a.emit({0x86, 0x01});                       // LDA #1          PB0
    const uint16_t wait = a.pc;
    a.emit({0x95, 0x22});                       // BITA <$22   4
    a.branch(0x26, wait);                       // BNE wait    3
    a.emit({0x30, 0x01});                       // LEAX 1,X    5
    a.emit({0x31, 0x3F});                       // LEAY -1,Y   5
    a.emit({0x21, 0x00});                       // BRN         3
    uint16_t beqFinal = 0;
    for (int i = 0; i < 7; ++i) {
        a.emit({0x96, 0x22});                   // LDA <$22    4   d0 .. d6
        a.emit({0x44});                         // LSRA        2
        a.emit({0x56});                         // RORB        2
        if (i == 6) {
            a.emit({0x31, 0xA4});               // LEAY ,Y     4   the last byte?
            beqFinal = a.pc;
            a.emit({0x27, 0x00});               // BEQ final   3
        } else if (i % 2 == 0) {
            a.emit({0x21, 0x00, 0x12, 0x12});   // BRN NOP NOP 7   (15)
        } else {
            a.emit({0x21, 0x00, 0x21, 0x00, 0x12});   // BRN BRN NOP 8   (16)
        }
    }
    a.emit({0x96, 0x22});                       // LDA <$22        d7
    a.emit({0x44});
    a.emit({0x56});
    a.emit({0xE7, 0x1F});                       // STB -1,X    5
    a.emit({0x86, 0x01});                       // LDA #1      2
    a.branch(0x20, wait);                       // BRA wait    3
    const uint16_t fin = a.pc;
    a.b[(size_t)(beqFinal + 1 - a.org)] = (uint8_t)(fin - (beqFinal + 2));
    a.emit({0x96, 0x22});                       // LDA <$22        d7 of the last byte
    a.emit({0x44});
    a.emit({0x56});
    a.emit({0xE7, 0x1F});                       // STB -1,X
    a.emit({0x39});                             // RTS
}

/** OP_READEX of drive 0's LSN 1 over the bit banger into $0600; the checksum
 *  sent back; the server's status byte to $0702, $5D to $0701. At `org`
 *  ($C000 a cartridge's, $8000 the CoCo 3's ROM); `fast`: the CoCo 3 at 1.79 MHz. */
std::vector<uint8_t> bbReadProgram(uint16_t org, bool fast) {
    Asm a(org, org == 0x8000 ? 0x8000 : 0x2000, 0xFF);
    a.emit({0x1A, 0x50});                       // ORCC #$50
    a.emit({0x10, 0xCE, 0x7F, 0x00});           // LDS #$7F00
    if (fast) a.ext(0xB7, 0xFFD9);              // STA $FFD9: 1.79 MHz
    if (org != 0x8000) a.ext(0xB7, 0xFFDD);     // the SAM: 64K (the stack's RAM)
    a.emit({0x86, 0x34}); a.ext(0xB7, 0xFF21);  // PIA1: PA1 at mark before it's an output
    a.emit({0x86, 0x02}); a.ext(0xB7, 0xFF20);
    a.ext(0x7F, 0xFF21);                        // DDRA $FE, DDRB $FA (PB0, PB2 in)
    a.emit({0x86, 0xFE}); a.ext(0xB7, 0xFF20);
    a.emit({0x86, 0x34}); a.ext(0xB7, 0xFF21);
    a.ext(0x7F, 0xFF23);
    a.emit({0x86, 0xFA}); a.ext(0xB7, 0xFF22);
    a.emit({0x86, 0x34}); a.ext(0xB7, 0xFF23);
    a.emit({0x86, 0x02}); a.ext(0xB7, 0xFF20);  // PA1 at mark
    a.emit({0x86, 0xFF});
    a.emit({0x1F, 0x8B});                       // TFR A,DP
    const uint16_t cmdRef = a.pc;
    a.emit({0x8E, 0x00, 0x00});                 // LDX #cmd (patched)
    a.emit({0x10, 0x8E, 0x00, 0x05});           // LDY #5
    const uint16_t call1 = a.pc;
    a.emit({0xBD, 0x00, 0x00});                 // JSR write (patched)
    a.emit({0x8E, 0x06, 0x00});                 // LDX #$0600
    a.emit({0x10, 0x8E, 0x01, 0x00});           // LDY #256
    const uint16_t call2 = a.pc;
    a.emit({0xBD, 0x00, 0x00});                 // JSR read
    a.emit({0x8E, 0x06, 0x00});                 // LDX #$0600
    a.emit({0xCC, 0x00, 0x00});                 // LDD #0
    const uint16_t sum = a.pc;
    a.emit({0xEB, 0x80});                       // ADDB ,X+
    a.emit({0x89, 0x00});                       // ADCA #0
    a.emit({0x8C, 0x07, 0x00});                 // CMPX #$0700
    a.branch(0x26, sum);
    a.ext(0xFD, 0x0704);                        // STD $0704
    a.emit({0x8E, 0x07, 0x04});                 // LDX #$0704
    a.emit({0x10, 0x8E, 0x00, 0x02});           // LDY #2
    const uint16_t call3 = a.pc;
    a.emit({0xBD, 0x00, 0x00});                 // JSR write
    a.emit({0x8E, 0x07, 0x02});                 // LDX #$0702
    a.emit({0x10, 0x8E, 0x00, 0x01});           // LDY #1
    const uint16_t call4 = a.pc;
    a.emit({0xBD, 0x00, 0x00});                 // JSR read
    a.emit({0x86, 0x5D}); a.ext(0xB7, CART_MARK);
    const uint16_t spin = a.pc;
    a.branch(0x20, spin);
    const uint16_t cmd = a.pc;
    a.emit({0xD2, 0x00, 0x00, 0x00, 0x01});     // OP_READEX, drive 0, LSN 1
    const uint16_t write = a.pc;
    bbWrite(a);
    const uint16_t read = a.pc;
    bbRead(a);
    a.word((uint16_t)(cmdRef + 1), cmd);
    a.word((uint16_t)(call1 + 1), write);
    a.word((uint16_t)(call3 + 1), write);
    a.word((uint16_t)(call2 + 1), read);
    a.word((uint16_t)(call4 + 1), read);
    if (org == 0x8000) a.word(0xFFFE, 0x8000);
    return a.b;
}

/** The stand-in on `model`, DriveWire on the bit banger (`serial`), drive 0 a
 *  test image: true if the sector came in whole and the server took the checksum. */
bool bbReadex(coco::Model model, bool hd6309, const std::string& serial, bool fast = true,
              const std::function<void(MachineDescriptor*)>& check = nullptr) {
    const std::string dir = tempDir();
    std::vector<uint8_t> img(630 * 256);
    for (size_t i = 0; i < img.size(); ++i) img[i] = (uint8_t)(i * 7 + (i >> 8) + 1);
    writeFile(dir + "/os9.dsk", img);
    coco::Roms roms;
    std::string err;
    const bool c3 = model == coco::Model::CoCo3;
    if (c3) roms.coco3Basic = bbReadProgram(0x8000, fast);
    else roms.colorBasic = jumpToCart();
    MachineDescriptor* m = coco::create(model, &err, c3 ? 128 : 64, &roms, coco::Tv::Default, hd6309);
    if (!m) return false;
    if (!c3) {
        writeFile(dir + "/dw.rom", bbReadProgram(0xC000, false));
        EXPECT_TRUE(m->expansionSlot("cart")->install("dosrom", dir + "/dw.rom", err));
    }
    coco::System* sys = coco::systemOf(m);
    EXPECT_TRUE(sys->rs232.attachSerial(serial));
    IOHandler* h = m->ioRegistry->mountDisk(1, dir + "/os9.dsk");      // `disk mount 1`: DriveWire drive 0
    EXPECT_TRUE(h == &sys->rs232);
    m->onReset(*m);
    run(m, 400000);
    IBus* bus = m->buses[0].bus;
    bool ok = bus->peek8(CART_MARK) == 0x5D && bus->peek8(0x0702) == 0x00;
    for (int i = 0; ok && i < 256; ++i) ok = bus->peek8((uint16_t)(0x0600 + i)) == img[256 + i];
    if (ok) {
        const auto& st = sys->rs232.driveWire()->stats();
        EXPECT_EQ((int)st.reads, 1);
        EXPECT_EQ((int)st.crcErrors, 0);
        EXPECT_TRUE(sys->rs232.driveWire()->idle());
        if (check) check(m);
    }
    delete m;
    std::error_code ec;
    fs::remove_all(dir, ec);
    return ok;
}

}  // namespace

// A CoCo 2 (0.89 MHz): 57600 baud, the machine's rate; HDB-DOS's ROM socket without a controller
TEST_CASE(coco2_drivewire_bitbanger_readex) {
    ASSERT(bbReadex(coco::Model::CoCo2, false, "drivewire", true, [](MachineDescriptor* m) {
        coco::System* sys = coco::systemOf(m);
        ASSERT_EQ(sys->rs232.baud(), 57600);
        ASSERT(sys->cart()->describe().find("DOS ROM (dw.rom, 8 KB, no autostart)") != std::string::npos);
        int track = -1, sector = -1;
        bool led = false;
        ASSERT(sys->rs232.getDiskStatus(1, track, sector, led));
        ASSERT(sys->rs232.getMountedDiskPath(1).find("os9.dsk") != std::string::npos);
        ASSERT(sys->rs232.sent.empty());                    // DriveWire's commands aren't "printed"
    }));
    // The guest's cycle counts are for 57600: at another rate nothing gets through
    ASSERT(!bbReadex(coco::Model::CoCo2, false, "drivewire:38400"));
    ASSERT(!bbReadex(coco::Model::CoCo2, false, "drivewire:115200"));
}

// A CoCo 3 at 1.79 MHz: 115200 baud (the 6809, and the HD6309 in its 6809 mode)
TEST_CASE(coco3_drivewire_bitbanger_readex) {
    ASSERT(bbReadex(coco::Model::CoCo3, false, "drivewire", true, [](MachineDescriptor* m) {
        ASSERT_EQ(coco::systemOf(m)->rs232.baud(), 115200);
    }));
    ASSERT(bbReadex(coco::Model::CoCo3, true, "drivewire"));
    ASSERT(bbReadex(coco::Model::CoCo3, false, "drivewire:57600", false));   // at 0.89 MHz: 57600
    ASSERT(!bbReadex(coco::Model::CoCo3, false, "drivewire", false));        // 0.89 MHz against 115200
}

// PB0 is sampled on the cycle the 6809 reads PIA1, its dead cycles in their
// place: LDA $FF22 reads on its 5th cycle (opcode, address x2, dead, read),
// LDA <$22 on its 4th. The line falls exactly as that cycle starts (seen
// low) or a tick after (seen high). On a CoCo 2 (16 ticks a cycle) and a
// CoCo 3 at 1.79 MHz (8).
TEST_CASE(coco_pb0_sampled_on_the_read_cycle) {
    for (int coco3 = 0; coco3 < 2; ++coco3) {
        for (int ext = 0; ext < 2; ++ext) {
            for (int late = 0; late < 2; ++late) {
                const uint16_t org = coco3 ? 0x8000 : 0xA000;
                Asm a(org, coco3 ? 0x8000 : 0x2000, 0x12);
                if (coco3) a.ext(0xB7, 0xFFD9);                 // 1.79 MHz
                a.emit({0x86, 0x34}); a.ext(0xB7, 0xFF23);      // PIA1 port B: the data register (all inputs)
                a.emit({0x86, 0xFF}); a.emit({0x1F, 0x8B});     // DP = $FF
                a.emit({0x12});
                const uint16_t at = a.pc;
                if (ext) a.ext(0xB6, 0xFF22);                   // LDA $FF22
                else a.emit({0x96, 0x22});                      // LDA <$22
                const uint16_t spin = a.pc;
                a.branch(0x20, spin);
                a.word(coco3 ? 0xFFFE : 0xBFFE, org);
                coco::Roms roms;
                if (coco3) roms.coco3Basic = a.b;
                else roms.colorBasic = a.b;
                std::string err;
                MachineDescriptor* m = coco::create(coco3 ? coco::Model::CoCo3 : coco::Model::CoCo2, &err, coco3 ? 128 : 64, &roms);
                ASSERT(m);
                coco::System* sys = coco::systemOf(m);
                m->onReset(*m);
                for (int i = 0; i < 100 && m->cpus[0].cpu->pc() != at; ++i) m->schedulerStep(*m);
                ASSERT_EQ(m->cpus[0].cpu->pc(), at);
                const uint64_t cycle = coco3 ? 8 : 16;
                const uint64_t readCycle = sys->ticks() + (ext ? 4 : 3) * cycle;
                sys->rs232.receiveAt({0x00}, readCycle + (uint64_t)late);
                m->schedulerStep(*m);
                ASSERT_EQ(m->cpus[0].cpu->pc(), spin);
                ASSERT_EQ((int)(m->cpus[0].cpu->regRead(0) & 1), late);
                delete m;
            }
        }
    }
}
// DriveWire's network access (the virtual modem's tcp connect): off unless
// asked for (",net", the "network" option); a switch in the device's info
TEST_CASE(coco_drivewire_network_switch) {
    auto switchOf = [](IOHandler* h) {
        DeviceInfo info;
        h->getDeviceInfo(info);
        for (const auto& sw : info.switches)
            if (sw.key == "network") return sw.on ? 1 : 0;
        return -1;
    };
    std::string err;
    // The bit banger: the option is kept until DriveWire is attached
    coco::BitBanger bb;
    ASSERT_EQ(switchOf(&bb), -1);                           // no DriveWire, no switch
    ASSERT(bb.attachSerial("drivewire"));
    ASSERT_EQ(switchOf(&bb), 0);
    ASSERT(!bb.driveWire()->network());
    ASSERT(bb.setOption("network", "on", err));
    ASSERT(bb.driveWire()->network());
    ASSERT_EQ(switchOf(&bb), 1);
    ASSERT(!bb.setOption("network", "sometimes", err));
    ASSERT(bb.setOption("network", "off", err));
    ASSERT_EQ(switchOf(&bb), 0);
    ASSERT(bb.attachSerial("drivewire:38400,cow,net"));
    ASSERT(bb.driveWire()->network());
    ASSERT(!bb.attachSerial("drivewire,net,ro"));
    coco::BitBanger early;
    ASSERT(early.setOption("network", "on", err));          // before the attach: kept
    ASSERT(early.attachSerial("drivewire"));
    ASSERT(early.driveWire()->network());

    // The Becker cartridge, through the machine's cartridge port
    coco::Roms roms;
    roms.colorBasic = jumpToCart();
    MachineDescriptor* m = coco::create(coco::Model::CoCo2, &err, 64, &roms);
    ASSERT(m);
    IOHandler* port = m->ioRegistry->findHandler("Cartridge");
    ASSERT(port);
    ASSERT_EQ(switchOf(port), -1);
    ASSERT(!port->setOption("network", "on", err));         // no Becker cartridge
    ASSERT(m->expansionSlot("cart")->install("becker", "", err));
    auto* becker = dynamic_cast<coco::BeckerCart*>(coco::systemOf(m)->cart());
    ASSERT(becker && !becker->server().network());
    ASSERT_EQ(switchOf(port), 0);
    ASSERT(port->setOption("network", "on", err));
    ASSERT(becker->server().network());
    ASSERT_EQ(switchOf(port), 1);
    ASSERT(becker->describe().find("network on") != std::string::npos);
    ASSERT(m->expansionSlot("cart")->install("becker", "cow,net", err));
    becker = dynamic_cast<coco::BeckerCart*>(coco::systemOf(m)->cart());
    ASSERT(becker && becker->server().network() && becker->copyOnWrite());
    delete m;
}

// Held options: set "when started" they wait for the guest's DriveWire
// driver to start (DWINIT / INIT); with a delay, for that much guest time
TEST_CASE(coco_drivewire_held_options) {
    OptionTiming t;
    ASSERT(OptionTiming::parse("started", t) && t.kind == OptionTiming::Kind::Started);
    ASSERT(OptionTiming::parse("2.5s", t) && t.kind == OptionTiming::Kind::Delay && t.seconds == 2.5);
    ASSERT(OptionTiming::parse("now", t) && t.kind == OptionTiming::Kind::Now);
    ASSERT(!OptionTiming::parse("later", t));
    ASSERT(!OptionTiming::parse("-1", t));
    std::string err;
    const uint64_t ms = (uint64_t)(coco::MASTER_HZ / 1000);

    // The bit banger: "started" waits for DWINIT
    coco::BitBanger bb;
    ASSERT(bb.attachSerial("drivewire"));
    ASSERT(!bb.setOptionWhen("network", "maybe", OptionTiming::started(), err));   // checked at once
    ASSERT(!bb.setOptionWhen("speed", "1", OptionTiming::started(), err));
    ASSERT(bb.setOptionWhen("network", "on", OptionTiming::started(), err));
    ASSERT_EQ((int)bb.pendingOptions().size(), 1);
    uint64_t now = 0;
    for (int i = 0; i < 50; ++i) bb.clock(now += ms);
    ASSERT(!bb.driveWire()->network());                     // no init yet
    bb.driveWire()->receive(DriveWireServer::OP_DWINIT);
    bb.driveWire()->receive(0x00);
    bb.clock(now += 2 * ms);
    ASSERT(bb.driveWire()->network());
    ASSERT(bb.pendingOptions().empty());
    // A delay: 0.2 s of guest time from when it was set
    ASSERT(bb.setOptionWhen("network", "off", OptionTiming::delay(0.2), err));
    for (int i = 0; i < 150; ++i) bb.clock(now += ms);
    ASSERT(bb.driveWire()->network());
    for (int i = 0; i < 60; ++i) bb.clock(now += ms);
    ASSERT(!bb.driveWire()->network());
    // A setting now replaces a held one
    ASSERT(bb.setOptionWhen("network", "on", OptionTiming::delay(0.1), err));
    ASSERT(bb.setOptionWhen("network", "off", OptionTiming::now(), err));
    ASSERT(bb.pendingOptions().empty());

    // The cartridge port: held before the Becker cartridge is there; DriveWire 3's INIT
    coco::Roms roms;
    roms.colorBasic = jumpToCart();
    MachineDescriptor* m = coco::create(coco::Model::CoCo2, &err, 64, &roms);
    ASSERT(m);
    IOHandler* port = m->ioRegistry->findHandler("Cartridge");
    ASSERT(port);
    ASSERT(port->setOptionWhen("network", "on", OptionTiming::started(), err));
    ASSERT(m->expansionSlot("cart")->install("becker", "", err));
    auto* becker = dynamic_cast<coco::BeckerCart*>(coco::systemOf(m)->cart());
    ASSERT(becker);
    auto& cp = coco::systemOf(m)->cartPort;
    for (int i = 0; i < 20; ++i) cp.clock(ms);
    ASSERT(!becker->server().network());
    becker->write(0xFF42, false, true, DriveWireServer::OP_INIT);
    for (int i = 0; i < 3; ++i) cp.clock(ms);
    ASSERT(becker->server().network());
    ASSERT(port->pendingOptions().empty());
    // A held option the device then refuses: shown in its info
    ASSERT(port->setOptionWhen("network", "off", OptionTiming::delay(0.01), err));
    ASSERT(m->expansionSlot("cart")->install("", "", err));    // the cartridge taken out
    for (int i = 0; i < 20; ++i) cp.clock(ms);
    DeviceInfo info;
    port->getDeviceInfo(info);
    bool refused = false;
    for (const auto& st : info.state) refused |= st.first == "Held option refused";
    ASSERT(refused);
    delete m;
}


// The transport: a command's frames off PA1 to the server, its answer down
// PB0 TURNAROUND_US after the stop bit, 8N1 back to back; one server only
TEST_CASE(coco_bitbanger_drivewire_line) {
    coco::BitBanger bb;
    bb.setDriveWireBaud(57600);
    ASSERT(!bb.rxConnected());
    ASSERT(!bb.attachSerial("drivewire:300000"));
    ASSERT(!bb.attachSerial("drivewire,ro"));
    ASSERT(bb.attachSerial("drivewire"));
    ASSERT_EQ(bb.baud(), 57600);
    ASSERT(bb.rxConnected());
    ASSERT(bb.rxLevel(0));                                  // idle: mark
    const double bit = coco::MASTER_HZ / 57600.0;
    // OP_TIME ($23), 8N1 from t = 1000
    const uint64_t t0 = 1000;
    bb.txLevel(t0, false);
    for (int i = 0; i < 8; ++i) bb.txLevel(t0 + (uint64_t)(bit * (1 + i)), (DriveWireServer::OP_TIME >> i) & 1);
    bb.txLevel(t0 + (uint64_t)(bit * 9), true);
    bb.txLevel(t0 + (uint64_t)(bit * 9.6), true);           // the frame is complete
    ASSERT(bb.receiving());
    const uint64_t start = t0 + (uint64_t)(bit * 10.0) + (uint64_t)(coco::BitBanger::TURNAROUND_US * coco::MASTER_HZ / 1e6);
    ASSERT(bb.rxLevel(start - 1));                          // not yet
    ASSERT(!bb.rxLevel(start));                             // the start bit of the year
    std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    uint8_t year = 0;
    for (int i = 0; i < 8; ++i)
        if (bb.rxLevel(start + (uint64_t)(bit * (1.5 + i)))) year |= (uint8_t)(1 << i);
    ASSERT_EQ(year, (uint8_t)tm.tm_year);
    ASSERT(bb.rxLevel(start + (uint64_t)(bit * 9.5)));      // stop
    ASSERT(!bb.rxLevel(start + (uint64_t)(bit * 10.2)));    // the month's start bit, straight after
    ASSERT(bb.rxLevel(start + (uint64_t)(bit * 70)));       // six bytes, then mark
    ASSERT(!bb.receiving());
    // Drives: `disk mount N` is drive N-1
    char path[] = "/tmp/coco_bbdw_XXXXXX";
    const int fd = mkstemp(path);
    ASSERT(fd >= 0);
    std::vector<uint8_t> sector(256, 0xA5);
    ASSERT(write(fd, sector.data(), sector.size()) == 256);
    close(fd);
    ASSERT(bb.mountDisk(2, path));
    ASSERT(bb.driveWire()->mounted(1));
    DeviceInfo info;
    bb.getDeviceInfo(info);
    ASSERT(!info.state.empty() && info.state[0].second.find("57600 baud; drive 1:") != std::string::npos);
    bb.ejectDisk(2);
    ASSERT(!bb.driveWire()->mounted(1));
    unlink(path);
    // `serial none` takes it off; with a Becker cartridge's server in the machine it won't go on
    ASSERT(bb.attachSerial("none"));
    ASSERT(!bb.driveWire() && !bb.rxConnected());
    ASSERT(!bb.mountDisk(1, "/dev/null"));
    coco::Roms roms;
    roms.colorBasic = jumpToCart();
    std::string err;
    MachineDescriptor* m = coco::create(coco::Model::CoCo2, &err, 64, &roms);
    ASSERT(m);
    ASSERT(m->expansionSlot("cart")->install("mpi", "", err));
    ASSERT(m->expansionSlot("mpi2")->install("becker", "", err));
    ASSERT(!coco::systemOf(m)->rs232.attachSerial("drivewire"));
    ASSERT(m->expansionSlot("mpi2")->install("", "", err));
    ASSERT(coco::systemOf(m)->rs232.attachSerial("drivewire,cow"));
    delete m;
}
