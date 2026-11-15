#include "test_harness.h"
#include "plugins/devices/coco_sdc/main/coco_sdc.h"
#include "plugins/devices/wd17xx/main/floppy_disk.h"
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

// The CoCo SDC's command interface driven as the User Guide's CommSDC
// routine does: $43 to the latch, parameters to $FF49-$FF4B, the command to
// $FF48, 256-byte blocks through $FF4A/$FF4B while READY, BUSY polled.

namespace {

namespace fs = std::filesystem;

constexpr int CMD = 8, P1 = 9, P2 = 10, P3 = 11;
constexpr uint8_t FAILED = CocoSdc::ST_FAILED;

struct Card {
    std::string dir;
    Card() {
        char t[] = "/tmp/coco_sdc_XXXXXX";
        dir = mkdtemp(t);
    }
    ~Card() { std::error_code ec; fs::remove_all(dir, ec); }
    std::string file(const std::string& name, const std::vector<uint8_t>& bytes) const {
        const std::string p = dir + "/" + name;
        std::ofstream f(p, std::ios::binary);
        f.write((const char*)bytes.data(), (std::streamsize)bytes.size());
        return p;
    }
};

/** A CocoSdc with its FDC half's two drives (declared first: the SDC ejects into them). */
struct Rig {
    std::unique_ptr<FloppyDisk> disks[2];
    CocoSdc sdc;
    explicit Rig(const std::string& root) {
        sdc.insertFloppy = [this](int d, std::unique_ptr<FloppyDisk> disk) { disks[d] = std::move(disk); return true; };
        sdc.removeFloppy = [this](int d) {
            if (disks[d] && disks[d]->dirty()) disks[d]->save();
            return std::move(disks[d]);
        };
        sdc.floppy = [this](int d) { return disks[d].get(); };
        sdc.setCardRoot(root);
        sdc.reset();
        sdc.writeLatch(0x43);
    }
    uint8_t status() { uint8_t v = 0; sdc.read(CMD, v); return v; }
    void command(uint8_t c, uint8_t p1 = 0, uint8_t p2 = 0, uint8_t p3 = 0) {
        sdc.write(P1, p1);
        sdc.write(P2, p2);
        sdc.write(P3, p3);
        sdc.write(CMD, c);
    }
    std::vector<uint8_t> readBlock(bool eightBit = false) {
        std::vector<uint8_t> out;
        while (status() & CocoSdc::ST_READY) {
            uint8_t a = 0, b = 0;
            if (eightBit) {
                sdc.read(P3, a);
                out.push_back(a);
            } else {
                sdc.read(P2, a);        // LDD $FF4A
                sdc.read(P3, b);
                out.push_back(a);
                out.push_back(b);
            }
        }
        return out;
    }
    void writeBlock(const std::vector<uint8_t>& data) {
        for (size_t i = 0; i < 256; i += 2) {
            sdc.write(P2, i < data.size() ? data[i] : 0);
            sdc.write(P3, i + 1 < data.size() ? data[i + 1] : 0);
        }
    }
    /** A $E0 command: the string "X:path" in a 256-byte block. */
    uint8_t send(const std::string& text, int drive = 0, uint8_t p1 = 0, uint8_t p2 = 0) {
        command((uint8_t)(0xE0 | drive), p1, p2);
        ASSERT_EQ(status(), CocoSdc::ST_BUSY | CocoSdc::ST_READY);
        writeBlock(std::vector<uint8_t>(text.begin(), text.end()));
        return status();
    }
};

std::vector<uint8_t> pattern(size_t sectors) {
    std::vector<uint8_t> img(sectors * 256);
    for (size_t i = 0; i < img.size(); ++i) img[i] = (uint8_t)(i / 256 * 3 + i);
    return img;
}

}  // namespace

// Command mode comes and goes with $43 at $FF40; outside it the FDC has $FF48-$FF4B
TEST_CASE(coco_sdc_command_mode_latch) {
    CocoSdc sdc;
    uint8_t v = 0;
    ASSERT(!sdc.read(CMD, v));
    ASSERT(sdc.writeLatch(0x43));
    ASSERT(sdc.commandMode());
    ASSERT(sdc.read(CMD, v));
    ASSERT_EQ(v, 0);
    ASSERT(!sdc.writeLatch(0x00));     // "write 0 to the control latch": FDC emulation
    ASSERT(!sdc.commandMode());
    ASSERT(!sdc.write(P1, 1));
}

// A DSK over 2880 sectors is a hard disk: LSN reads and writes reach the file, 'Q' its size
TEST_CASE(coco_sdc_hard_disk_lsn) {
    Card card;
    const std::string path = card.file("HARD.DSK", pattern(3000));
    Rig r(card.dir);
    ASSERT(r.sdc.mount(1, path));
    ASSERT(r.sdc.isHardDisk(1));
    r.command(0x81, 0x00, 0x0B, 0x05);           // drive 1, LSN $000B05
    auto got = r.readBlock();
    ASSERT_EQ(got.size(), (size_t)256);
    for (int i = 0; i < 256; ++i) ASSERT_EQ(got[i], (uint8_t)(0xB05 * 3 + 0xB05 * 256 + i));
    ASSERT_EQ(r.status(), 0);
    // 8-bit read (bit 2): every byte through $FF4B
    r.command(0x85, 0, 0, 7);
    got = r.readBlock(true);
    ASSERT_EQ(got.size(), (size_t)256);
    ASSERT_EQ(got[1], (uint8_t)(7 * 3 + 7 * 256 + 1));
    // Write LSN 2 (8-bit, bit 3), read it back, and in the file
    std::vector<uint8_t> data(256);
    for (int i = 0; i < 256; ++i) data[i] = (uint8_t)(255 - i);
    r.command(0xA9, 0, 0, 2);
    ASSERT_EQ(r.status(), CocoSdc::ST_BUSY | CocoSdc::ST_READY);
    for (uint8_t b : data) r.sdc.write(P3, b);
    ASSERT_EQ(r.status(), 0);
    r.command(0x81, 0, 0, 2);
    ASSERT(r.readBlock() == data);
    {
        std::ifstream f(path, std::ios::binary);
        f.seekg(512);
        std::vector<uint8_t> disk(256);
        f.read((char*)disk.data(), 256);
        ASSERT(disk == data);
    }
    // The FDC sees its first 1440 sectors as 80 single-sided tracks
    ASSERT(r.disks[1] != nullptr);
    std::vector<uint8_t> sec;
    ASSERT(r.disks[1]->readSector(0, 0, 3, sec));
    ASSERT(sec == data);
    // Size: file / 256, in $FF49-$FF4B
    r.command(0xC1, 'Q');
    ASSERT_EQ(r.status(), 0);
    ASSERT_EQ(r.sdc.param(1), 0);
    ASSERT_EQ(r.sdc.param(2) << 8 | r.sdc.param(3), 3000);
    // Past the end, or an empty drive: FAILED + $10
    r.command(0x81, 0, 0x10, 0);
    ASSERT_EQ(r.status(), FAILED | CocoSdc::ST_NOT_FOUND);
    r.command(0x80, 0, 0, 0);
    ASSERT_EQ(r.status(), FAILED | CocoSdc::ST_NOT_FOUND);
    r.command(0xA0, 0, 0, 0);
    ASSERT_EQ(r.status(), FAILED | CocoSdc::ST_NOT_FOUND);
    r.command(0xC0, 'I');
    ASSERT_EQ(r.status(), FAILED);
}

// "M:" mounts by name (any case; no extension: name.*); LSNs reach the floppy's sectors
TEST_CASE(coco_sdc_mount_dsk_by_name) {
    Card card;
    std::vector<uint8_t> img(35 * 18 * 256, 0xE5);
    for (int i = 0; i < 256; ++i) img[(17 * 18 + 2) * 256 + i] = (uint8_t)(i ^ 0x3C);   // track 17 sector 3 = LSN 308
    fs::create_directory(card.dir + "/games");
    card.file("games/Zork.dsk", img);
    Rig r(card.dir);
    ASSERT_EQ(r.send("M:GAMES/ZORK"), 0);
    ASSERT(!r.sdc.isHardDisk(0));
    ASSERT(r.disks[0] != nullptr);
    ASSERT(r.sdc.imagePath(0).find("games/Zork.dsk") != std::string::npos);
    r.command(0x80, 0, 308 >> 8, 308 & 0xFF);
    auto got = r.readBlock();
    ASSERT_EQ(got.size(), (size_t)256);
    for (int i = 0; i < 256; ++i) ASSERT_EQ(got[i], (uint8_t)(i ^ 0x3C));
    r.command(0xC0, 'Q');
    ASSERT_EQ(r.sdc.param(2) << 8 | r.sdc.param(3), 35 * 18);
    // 'I': the 32-byte directory record, size LSB first
    r.command(0xC0, 'I');
    got = r.readBlock();
    ASSERT_EQ(std::string(got.begin(), got.begin() + 11), std::string("ZORK    DSK"));
    ASSERT_EQ(got[28] | got[29] << 8 | got[30] << 16 | got[31] << 24, 35 * 18 * 256);
    // A write through LSN lands in the floppy (the FDC sees the same disk)
    std::vector<uint8_t> data(256, 0xAB);
    r.command(0xA0, 0, 0, 5);
    r.writeBlock(data);
    ASSERT_EQ(r.status(), 0);
    std::vector<uint8_t> sec;
    ASSERT(r.disks[0]->readSector(0, 0, 6, sec));
    ASSERT_EQ(sec[0], 0xAB);
    ASSERT(r.disks[0]->dirty());
    // The same file in the other drive: in use
    ASSERT_EQ(r.send("M:GAMES/ZORK.DSK", 1), FAILED | CocoSdc::ST_IN_USE);
    // Not there; outside the card; not 8.3
    ASSERT_EQ(r.send("M:NOPE.DSK"), FAILED | CocoSdc::ST_NOT_FOUND);
    ASSERT(r.send("M:../../etc/passwd") & FAILED);
    ASSERT_EQ(r.send("M:TOOLONGNAME.DSK"), FAILED | CocoSdc::ST_INVALID);
    // "M:" alone ejects (and the change is saved)
    ASSERT_EQ(r.send("M:"), 0);
    ASSERT(r.sdc.imagePath(0).empty());
    ASSERT(r.disks[0] == nullptr);
    std::ifstream f(card.dir + "/games/Zork.dsk", std::ios::binary);
    f.seekg(5 * 256);
    ASSERT_EQ(f.get(), 0xAB);
}

// The single-sided LSN flag on a double-sided image: LSN 18 is track 1 side 0 (LSN 36)
TEST_CASE(coco_sdc_single_sided_flag) {
    Card card;
    card.file("DS.DSK", pattern(40 * 2 * 18));
    Rig r(card.dir);
    ASSERT_EQ(r.send("M:DS.DSK"), 0);
    ASSERT_EQ(r.disks[0]->heads(), 2);
    r.command(0x82, 0, 0, 18);
    auto got = r.readBlock();
    ASSERT_EQ(got[0], (uint8_t)(36 * 3 + 36 * 256));
    r.command(0x80, 0, 0, 18);
    got = r.readBlock();
    ASSERT_EQ(got[0], (uint8_t)(18 * 3 + 18 * 256));
}

// Too small for a DSK: a hardware error; "m:" mounts any file as raw blocks
TEST_CASE(coco_sdc_raw_mount_and_stream) {
    Card card;
    card.file("SMALL.BIN", pattern(5));
    Rig r(card.dir);
    ASSERT_EQ(r.send("M:SMALL.BIN"), FAILED | CocoSdc::ST_HARDWARE);
    ASSERT_EQ(r.send("m:SMALL.BIN"), 0);
    ASSERT(r.sdc.isHardDisk(0));
    ASSERT(r.disks[0] == nullptr);           // no FDC access
    r.command(0x80, 0, 0, 4);
    ASSERT_EQ(r.readBlock()[0], (uint8_t)(4 * 3 + 4 * 256));
    // Stream from 512-byte block 1: sectors 2-3, then block 2 (sector 4, padded), then done
    r.command(0x90, 0, 0, 1);
    std::vector<uint8_t> s = r.readBlock();
    ASSERT_EQ(s.size(), (size_t)1024);
    ASSERT_EQ(s[0], (uint8_t)(2 * 3 + 2 * 256));
    ASSERT_EQ(s[512], (uint8_t)(4 * 3 + 4 * 256));
    ASSERT_EQ(s[1023], 0);
    ASSERT_EQ(r.status(), 0);
    // Abort: $D0
    r.command(0x90, 0, 0, 0);
    ASSERT(r.status() & CocoSdc::ST_READY);
    r.command(0xD0);
    ASSERT_EQ(r.status(), 0);
}

// Listings: "L:" with a pattern, then '>' pages of 16-byte records; "D:" and 'C'
TEST_CASE(coco_sdc_directory_listing) {
    Card card;
    card.file("B.DSK", std::vector<uint8_t>(161280, 0));
    card.file("a.dsk", std::vector<uint8_t>(1000, 0));
    card.file("README.TXT", std::vector<uint8_t>(10, 0));
    card.file("a-very-long-name.dsk", std::vector<uint8_t>(10, 0));   // not 8.3: not listed
    fs::create_directory(card.dir + "/SUB");
    card.file("SUB/C.DSK", std::vector<uint8_t>(512, 0));
    Rig r(card.dir);
    r.command(0xC0, '>');                         // no listing yet
    ASSERT_EQ(r.status(), FAILED | CocoSdc::ST_HARDWARE);
    ASSERT_EQ(r.send("L:*.DSK"), 0);
    r.command(0xC0, '>');
    auto page = r.readBlock();
    ASSERT_EQ(page.size(), (size_t)256);
    ASSERT_EQ(std::string(page.begin(), page.begin() + 11), std::string("A       DSK"));
    ASSERT_EQ(page[11], 0);
    ASSERT_EQ(page[12] << 24 | page[13] << 16 | page[14] << 8 | page[15], 1000);   // MSB first
    ASSERT_EQ(std::string(page.begin() + 16, page.begin() + 27), std::string("B       DSK"));
    ASSERT_EQ(page[32], 0);                       // an unused record: the end
    ASSERT_EQ(r.send("L:*.*"), 0);
    r.command(0xC0, '>');
    page = r.readBlock();
    ASSERT_EQ(std::string(page.begin() + 32, page.begin() + 43), std::string("README  TXT"));
    ASSERT_EQ(std::string(page.begin() + 48, page.begin() + 59), std::string("SUB        "));
    ASSERT_EQ(page[59], 0x10);
    ASSERT_EQ(page[64], 0);
    ASSERT_EQ(r.send("L:NOSUCH/*.*"), FAILED | CocoSdc::ST_NOT_FOUND);
    // At the root 'C' fails with $10; in SUB it returns SUB's record
    r.command(0xC0, 'C');
    ASSERT_EQ(r.status(), FAILED | CocoSdc::ST_NOT_FOUND);
    ASSERT_EQ(r.send("D:sub"), 0);
    ASSERT_EQ(r.sdc.currentDir(), std::string("SUB"));
    r.command(0xC0, 'C');
    page = r.readBlock();
    ASSERT_EQ(std::string(page.begin(), page.begin() + 11), std::string("SUB        "));
    ASSERT_EQ(r.send("L:*.*"), 0);
    r.command(0xC0, '>');
    page = r.readBlock();
    ASSERT_EQ(std::string(page.begin(), page.begin() + 2), std::string(". "));
    ASSERT_EQ(std::string(page.begin() + 16, page.begin() + 18), std::string(".."));
    ASSERT_EQ(std::string(page.begin() + 32, page.begin() + 43), std::string("C       DSK"));
    ASSERT_EQ(r.send("D:.."), 0);
    ASSERT_EQ(r.sdc.currentDir(), std::string(""));
    ASSERT_EQ(r.send("D:.."), FAILED | CocoSdc::ST_NOT_FOUND);   // not above the card
    ASSERT_EQ(r.send("D:NOWHERE"), FAILED | CocoSdc::ST_NOT_FOUND);
}

// "N:" creates (630 sectors) and mounts; "K:" makes a directory; "X:" deletes; 'V' the version
TEST_CASE(coco_sdc_new_mkdir_delete_version) {
    Card card;
    Rig r(card.dir);
    ASSERT_EQ(r.send("N:NEW.DSK", 1), 0);
    ASSERT_EQ(fs::file_size(card.dir + "/NEW.DSK"), (uintmax_t)(630 * 256));
    ASSERT(!r.sdc.imagePath(1).empty());
    ASSERT_EQ(r.send("X:NEW.DSK"), FAILED | CocoSdc::ST_IN_USE);   // mounted
    ASSERT_EQ(r.send("M:", 1), 0);
    ASSERT_EQ(r.send("X:NEW.DSK"), 0);
    ASSERT(!fs::exists(card.dir + "/NEW.DSK"));
    ASSERT_EQ(r.send("X:NEW.DSK"), FAILED | CocoSdc::ST_NOT_FOUND);
    ASSERT_EQ(r.send("K:DIR"), 0);
    ASSERT(fs::is_directory(card.dir + "/DIR"));
    ASSERT_EQ(r.send("K:NO/DIR"), FAILED | CocoSdc::ST_NOT_FOUND);
    card.file("DIR/F.DSK", std::vector<uint8_t>(10, 0));
    ASSERT_EQ(r.send("X:DIR"), FAILED | CocoSdc::ST_IN_USE);       // not empty
    // A new SDF image: 40 cylinders, 1 side
    ASSERT_EQ(r.send("N:DISK.SDF", 0, 40, 1), 0);
    r.command(0xC0, 'I');
    auto rec = r.readBlock();
    ASSERT_EQ(rec[11] & 0x04, 0x04);
    ASSERT(r.disks[0] != nullptr);
    ASSERT_EQ(r.disks[0]->cylinders(), 40);
    // "n:" an empty raw file
    ASSERT_EQ(r.send("n:RAW.BIN", 1), 0);
    ASSERT_EQ(fs::file_size(card.dir + "/RAW.BIN"), (uintmax_t)0);
    r.command(0xC0, 'V');
    ASSERT_EQ(r.status(), 0);
    ASSERT_EQ(r.sdc.param(2) << 8 | r.sdc.param(3), CocoSdc::VERSION);
    ASSERT_EQ(r.send("Z:X"), FAILED | CocoSdc::ST_INVALID);
    r.command(0xC0, 'Z');
    ASSERT_EQ(r.status(), FAILED);
}

// SDF images: read through LSN and written back to the file on eject
TEST_CASE(coco_sdc_sdf_round_trip) {
    Card card;
    // A formatted 2-track single-sided disk as SDF, via a JVC
    card.file("SRC.DSK", pattern(18 * 18));
    Rig r(card.dir);
    {
        auto src = FloppyDisk::load(card.dir + "/SRC.DSK");
        ASSERT(src != nullptr);
        ASSERT_EQ(r.send("N:IMG.SDF", 0, 18, 1), 0);
        ASSERT_EQ(r.send("M:"), 0);
    }
    // Lay real sectors down: copy the DSK's tracks into a new SDF through the FDC half
    ASSERT_EQ(r.send("M:IMG.SDF"), 0);
    FloppyDisk* d = r.disks[0].get();
    ASSERT(d != nullptr);
    for (int t = 0; t < 18; ++t) {
        std::vector<FloppySectorSpec> specs;
        for (int s = 1; s <= 18; ++s) {
            FloppySectorSpec sp;
            sp.track = (uint8_t)t;
            sp.sector = (uint8_t)s;
            sp.data.assign(256, (uint8_t)(t * 18 + s));
            specs.push_back(sp);
        }
        ASSERT(d->formatTrack(t, 0, specs));
    }
    d->markDirty();
    r.command(0x80, 0, 0, 20);                   // track 1, sector 3
    ASSERT_EQ(r.readBlock()[0], 21);
    ASSERT_EQ(r.send("M:"), 0);                  // written back as SDF
    std::ifstream f(card.dir + "/IMG.SDF", std::ios::binary);
    char sig[4];
    f.read(sig, 4);
    ASSERT_EQ(std::string(sig, 4), std::string("SDF1"));
    ASSERT_EQ(r.send("M:IMG.SDF"), 0);
    r.command(0x80, 0, 0, 20);
    ASSERT_EQ(r.readBlock()[0], 21);
    r.command(0x80, 0, 1, 0x43);                 // LSN 323: track 17, sector 18
    ASSERT_EQ(r.readBlock()[255], (uint8_t)(17 * 18 + 18));
}

// A directory mounted is a disk set; '+' and $D0 '#' move through it
TEST_CASE(coco_sdc_disk_set) {
    Card card;
    fs::create_directory(card.dir + "/GAME");
    card.file("GAME/PART1.DSK", pattern(18 * 18));
    card.file("GAME/PART2.DSK", pattern(18 * 18));
    card.file("GAME/PART3.DSK", pattern(18 * 18));
    Rig r(card.dir);
    ASSERT_EQ(r.send("M:GAME"), 0);
    ASSERT(r.sdc.imagePath(0).find("PART1.DSK") != std::string::npos);
    r.command(0xC0, '+');
    ASSERT_EQ(r.status(), 0);
    ASSERT(r.sdc.imagePath(0).find("PART2.DSK") != std::string::npos);
    r.command(0xD0, '#', 3, 1);
    ASSERT_EQ(r.status(), 0);
    ASSERT(r.sdc.imagePath(0).find("PART3.DSK") != std::string::npos);
    r.command(0xC0, '+');                         // past the last: back to 1
    ASSERT(r.sdc.imagePath(0).find("PART1.DSK") != std::string::npos);
    r.command(0xD0, '#', 7, 0);
    ASSERT_EQ(r.status(), FAILED | CocoSdc::ST_NOT_FOUND);
}

// STARTUP.CFG mounts and sets the directory at power-on
TEST_CASE(coco_sdc_startup_cfg) {
    Card card;
    fs::create_directory(card.dir + "/OS9");
    card.file("OS9/BOOT.DSK", pattern(18 * 35));
    const std::string cfg = "D=/OS9\r\n0=BOOT.DSK\r\n";
    card.file("STARTUP.CFG", std::vector<uint8_t>(cfg.begin(), cfg.end()));
    Rig r(card.dir);
    ASSERT_EQ(r.sdc.currentDir(), std::string("OS9"));
    ASSERT(r.sdc.imagePath(0).find("OS9/BOOT.DSK") != std::string::npos);
    ASSERT(r.disks[0] != nullptr);
}

// Flash: 8 banks of 16 KB, the bank at $FF43, the DIP switches' bank at reset
TEST_CASE(coco_sdc_flash_banks) {
    CocoSdc sdc;
    std::vector<uint8_t> flash(8 * 0x4000);
    for (int b = 0; b < 8; ++b) flash[(size_t)b * 0x4000 + 0x10] = (uint8_t)(0xB0 + b);
    sdc.loadFlash(flash);
    ASSERT(sdc.hasFlash());
    ASSERT_EQ(sdc.romByte(0xC010), 0xB0);
    ASSERT(sdc.write(3, 5));
    ASSERT_EQ(sdc.romByte(0xC010), 0xB5);
    uint8_t v = 0;
    ASSERT(sdc.read(3, v));
    ASSERT_EQ(v, 5);
    sdc.reset();
    ASSERT_EQ(sdc.bank(), 0);
    sdc.setDipBank(2);
    sdc.reset();
    ASSERT_EQ(sdc.romByte(0xC010), 0xB2);
    // An 8 KB ROM repeats through bank 0
    std::vector<uint8_t> rom(0x2000, 0);
    rom[0x100] = 0x77;
    sdc.loadFlash(rom);
    sdc.setBank(0);
    ASSERT_EQ(sdc.romByte(0xE100), 0x77);
}
