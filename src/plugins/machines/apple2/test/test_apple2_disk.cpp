#include "test_harness.h"
#include "plugins/machines/apple2/main/machine_apple2.h"
#include "plugins/machines/apple2/main/disk2.h"
#include "libcore/main/machine_desc.h"
#include "libdevices/main/io_registry.h"
#include "libdevices/main/ikeyboard_matrix.h"
#include "libmem/main/ibus.h"
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using apple2::Disk2Card;
using apple2::Disk2Image;

namespace {
uint32_t s_seed = 1;
uint8_t rnd() { s_seed = s_seed * 1103515245u + 12345u; return (uint8_t)(s_seed >> 16); }

std::vector<uint8_t> randomDisk() {
    std::vector<uint8_t> d(35 * 4096);
    for (auto& b : d) b = rnd();
    // Not a DOS 3.3 catalog chain on track 17, nor ProDOS links
    d[0x11102] = 0x55;
    return d;
}
void run(MachineDescriptor* m, uint64_t cycles) {
    uint64_t t = 0;
    while (t < cycles) t += (uint64_t)m->schedulerStep(*m);
}
Disk2Card* card(MachineDescriptor* m) {
    std::vector<IOHandler*> hs;
    m->ioRegistry->enumerate(hs);
    for (auto* h : hs) if (auto* c = dynamic_cast<Disk2Card*>(h)) return c;
    return nullptr;
}
bool screenHas(MachineDescriptor* m, const std::string& text) {
    IBus* bus = m->buses[0].bus;
    for (int r = 0; r < 24; ++r) {
        const uint16_t base = (uint16_t)(0x400 + (r & 7) * 0x80 + (r >> 3) * 0x28);
        std::string s;
        for (int i = 0; i < 40; ++i) s += (char)(bus->peek8(base + (uint16_t)i) & 0x7F);
        if (s.find(text) != std::string::npos) return true;
    }
    return false;
}
}  // namespace

// 6-and-2 (Code62/Decode62): 256 bytes -> 343 disk bytes (each a valid
// nibble: bit 7 set) -> the same 256 bytes
TEST_CASE(disk2_six_and_two) {
    for (int k = 0; k < 20; ++k) {
        uint8_t sec[256], out[256], nib[343];
        for (auto& b : sec) b = rnd();
        Disk2Image::nibblizeSector(sec, nib);
        bool valid = true;
        for (uint8_t n : nib) if (!(n & 0x80)) valid = false;
        EXPECT_TRUE(valid);
        Disk2Image::denibblizeSector(nib, out);
        EXPECT_TRUE(std::equal(sec, sec + 256, out));
    }
}

// A track nibblized (DOS and ProDOS order, skewed) and written back is the
// same data; the address fields carry volume 254 and the track
TEST_CASE(disk2_track_roundtrip) {
    for (const char* ext : {".dsk", ".po"}) {
        auto data = randomDisk();
        auto img = Disk2Image::fromBytes(data, ext);
        ASSERT(img);
        EXPECT_EQ(img->tracks(), 35);
        EXPECT_TRUE(img->order() == (std::string(ext) == ".po" ? Disk2Image::Order::ProDOS : Disk2Image::Order::DOS));
        std::vector<uint8_t> nib(Disk2Image::MAX_NIBBLES);
        for (int t : {0, 5, 17, 34}) {
            const int n = img->readTrack(t, nib.data());
            EXPECT_EQ(n, 48 + 16 * 396);
            // Find the first address field: D5 AA 96, volume, track
            int at = -1;
            for (int i = 0; i + 6 < n; ++i)
                if (nib[i] == 0xD5 && nib[i + 1] == 0xAA && nib[i + 2] == 0x96) { at = i; break; }
            ASSERT(at >= 0);
            EXPECT_EQ(((nib[at + 3] << 1) | 1) & nib[at + 4], 254);
            EXPECT_EQ(((nib[at + 5] << 1) | 1) & nib[at + 6], t);
            // Change a byte through the nibbles' round trip: none changes
            img->writeTrack(t, nib.data(), n);
        }
        EXPECT_TRUE(img->data() == data);
    }
    std::string err;
    EXPECT_TRUE(!Disk2Image::fromBytes(std::vector<uint8_t>(1000), ".dsk", &err));
    auto nibImg = Disk2Image::fromBytes(std::vector<uint8_t>(35 * 6656, 0xFF), ".nib");
    ASSERT(nibImg);
    EXPECT_EQ(nibImg->tracks(), 35);
}

// The stepper: phase 1 on moves the head a half track after 10 cycles;
// phase 2 on, phase 1 off: another; two adjacent magnets turned off in
// quick succession don't move it. Write protect reads in the latch ($C08D,
// then $C08E)
TEST_CASE(disk2_stepper_and_protect) {
    std::vector<uint8_t> rom(256, 0);
    Disk2Card c(rom);
    c.reset(true);
    auto img = Disk2Image::fromBytes(randomDisk(), ".dsk");
    img->setWriteProtected(true);
    c.insert(0, std::move(img));
    c.io(0xC0E9, nullptr, 0);                   // motor on
    EXPECT_TRUE(c.motorOn());
    c.io(0xC0E3, nullptr, 100);                 // phase 1 on
    EXPECT_EQ(c.phase(0), 0);                   // deferred
    c.update(120, 20);
    EXPECT_EQ(c.phase(0), 1);
    c.io(0xC0E5, nullptr, 300);                 // phase 2 on
    c.io(0xC0E2, nullptr, 340);                 // phase 1 off
    c.io(0xC0E4, nullptr, 400);                 // phase 2 off
    c.update(500, 100);
    EXPECT_EQ(c.phase(0), 2);
    c.io(0xC0E7, nullptr, 600);                 // phase 3 on: on to 3
    c.update(700, 100);
    EXPECT_EQ(c.phase(0), 3);
    c.io(0xC0E1, nullptr, 800);                 // phase 0 on: on to 4
    c.update(900, 100);
    EXPECT_EQ(c.phase(0), 4);
    // Phases 0 and 3 off within 10 cycles: no step (with only 0 off, phase
    // 3 would pull the head back to 3)
    c.io(0xC0E0, nullptr, 1000);
    c.io(0xC0E6, nullptr, 1004);
    c.update(1100, 100);
    EXPECT_EQ(c.phase(0), 4);
    // Write protect: $FF in the latch
    c.io(0xC0ED, nullptr, 1200);
    EXPECT_EQ(c.io(0xC0EE, nullptr, 1204), 0xFF);
    // Reads of the latch: valid nibbles (bit 7) every 32 cycles
    c.io(0xC0EC, nullptr, 1300);
    int nibbles = 0;
    for (uint64_t t = 1340; t < 1340 + 32 * 50; t += 32)
        if (c.io(0xC0EC, nullptr, t) & 0x80) ++nibbles;
    EXPECT_EQ(nibbles, 50);
    c.io(0xC0E8, nullptr, 3000);                // motor off: runs on a second
    EXPECT_TRUE(!c.motorOn());
    EXPECT_EQ(c.io(0xC0E1, nullptr, 3100), -1); // odd: the floating bus
}

// The boot ROM (P5, slot 6) reads track 0 sector 0 into $0800 and jumps to
// $0801: a boot sector that writes a mark and prints
TEST_CASE(apple2_disk_boot_sector) {
    std::string err;
    MachineDescriptor* m = apple2::create(apple2::Model::IIPlus, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    std::vector<uint8_t> d(35 * 4096, 0);
    // $0800: 1 sector; $0801: LDA #$A5; STA $0300; LDA #$C2 ('B'); STA $0400; JMP $080B
    const uint8_t boot[] = {0x01, 0xA9, 0xA5, 0x8D, 0x00, 0x03, 0xA9, 0xC2, 0x8D, 0x00, 0x04, 0x4C, 0x0B, 0x08};
    std::copy(boot, boot + sizeof boot, d.begin());
    Disk2Card* c = card(m);
    ASSERT(c);
    c->insert(0, Disk2Image::fromBytes(d, ".dsk"));
    m->onReset(*m);
    run(m, 3000000);
    IBus* bus = m->buses[0].bus;
    EXPECT_EQ(bus->peek8(0x300), 0xA5);
    EXPECT_EQ(bus->peek8(0x400), 0xC2);
    EXPECT_EQ(m->cpus[0].cpu->pc() & 0xFFF0, 0x0800);
    delete m;
}

// DOS 3.3 (AppleWin's System Master, if present): boots, CATALOGs, SAVEs a
// program to the image file, and the program loads again after a reboot
TEST_CASE(apple2_dos33_save) {
    const std::string master = "../refs/apple2/AppleWin/bin/DOS 3.3 System Master - 680-0210-A.dsk";
    std::ifstream in(master, std::ios::binary);
    if (!in) { printf("  (skipped: no %s)\n", master.c_str()); return; }
    const std::string path = "/tmp/mmemu_test_dos33.dsk";
    { std::ofstream out(path, std::ios::binary); out << in.rdbuf(); }
    std::string err;
    MachineDescriptor* m = apple2::create(apple2::Model::IIPlus, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    Disk2Card* c = card(m);
    ASSERT(c->mountDisk(1, path));
    m->onReset(*m);
    run(m, 20000000);
    EXPECT_TRUE(screenHas(m, "DOS VERSION 3.3"));
    IKeyboardMatrix* kbd = nullptr;
    std::vector<IOHandler*> hs;
    m->ioRegistry->enumerate(hs);
    for (auto* h : hs) if ((kbd = dynamic_cast<IKeyboardMatrix*>(h))) break;
    kbd->enqueueText("CATALOG\n");
    run(m, 6000000);
    EXPECT_TRUE(screenHas(m, "MASTER CREATE"));
    kbd->enqueueText("HOME\n10 PRINT 6*7\nSAVE MYPROG\nNEW\n");
    run(m, 12000000);
    c->ejectDisk(1);                            // flushes the track
    ASSERT(c->mountDisk(1, path));
    m->onReset(*m);
    run(m, 20000000);
    kbd->enqueueText("HOME\nRUN MYPROG\n");
    run(m, 8000000);
    EXPECT_TRUE(screenHas(m, "42"));
    delete m;
    std::remove(path.c_str());
}
