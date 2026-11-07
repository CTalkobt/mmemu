#include "test_harness.h"
#include "plugins/machines/apple2/main/machine_apple2.h"
#include "plugins/machines/apple2/main/disk2.h"
#include "libcore/main/machine_desc.h"
#include "libdevices/main/io_registry.h"
#include "libdevices/main/ikeyboard_matrix.h"
#include "libmem/main/ibus.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
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

// --- WOZ ---------------------------------------------------------------------

namespace {
void put16(std::vector<uint8_t>& d, size_t at, uint32_t v) { d[at] = (uint8_t)v; d[at + 1] = (uint8_t)(v >> 8); }
void put32(std::vector<uint8_t>& d, size_t at, uint32_t v) { put16(d, at, v & 0xFFFF); put16(d, at + 2, v >> 16); }

// A WOZ2 file laid out as AppleWin creates one: header, INFO (v2, 60
// bytes), TMAP, TRKS (160 entries, bits from block 3); `tracks` of bits
std::vector<uint8_t> makeWoz2(const std::vector<std::vector<uint8_t>>& tracks,
                              const std::vector<uint32_t>& bitCounts, int timing = 32, bool protect = false) {
    std::vector<uint8_t> d(1536, 0);
    std::memcpy(&d[0], "WOZ2\xFF\x0A\x0D\x0A", 8);
    std::memcpy(&d[12], "INFO", 4); put32(d, 16, 60);
    d[20] = 2; d[21] = 1; d[22] = protect ? 1 : 0;
    std::memset(&d[25], ' ', 32);
    d[57] = 1; d[58] = 1; d[59] = (uint8_t)timing;
    std::memcpy(&d[80], "TMAP", 4); put32(d, 84, 160);
    std::memset(&d[88], 0xFF, 160);
    std::memcpy(&d[248], "TRKS", 4);
    uint32_t block = 3, largest = 0;
    for (size_t t = 0; t < tracks.size(); ++t) {
        const uint32_t blocks = (uint32_t)((tracks[t].size() + 511) / 512);
        put16(d, 256 + t * 8, block);
        put16(d, 256 + t * 8 + 2, blocks);
        put32(d, 256 + t * 8 + 4, bitCounts[t]);
        d.resize(d.size() + blocks * 512, 0);
        std::copy(tracks[t].begin(), tracks[t].end(), d.begin() + block * 512);
        block += blocks;
        largest = std::max(largest, blocks);
        // whole track t at quarter tracks 4t-1 .. 4t+1
        for (int q = (int)t * 4 - 1; q <= (int)t * 4 + 1; ++q)
            if (q >= 0 && q < 160) d[88 + q] = (uint8_t)t;
    }
    put16(d, 64, largest);
    put32(d, 252, (uint32_t)(d.size() - 256));
    put32(d, 8, Disk2Image::crc32(d.data() + 12, d.size() - 12));
    return d;
}

// A .dsk's tracks as a WOZ2: the nibbles DOS 3.3 writes, the gaps' sync
// bytes 10 bits long (FF and two zeroes), as a real disk has them
std::vector<uint8_t> dskToWoz(const std::vector<uint8_t>& dsk) {
    auto img = Disk2Image::fromBytes(dsk, ".dsk");
    std::vector<std::vector<uint8_t>> tracks;
    std::vector<uint32_t> counts;
    std::vector<uint8_t> nib(Disk2Image::MAX_NIBBLES);
    for (int t = 0; t < 35; ++t) {
        const int n = img->readTrack(t, nib.data(), false);
        std::vector<uint8_t> bits;
        uint32_t count = 0;
        auto bit = [&](int b) {
            if (count % 8 == 0) bits.push_back(0);
            if (b) bits.back() |= (uint8_t)(0x80 >> (count % 8));
            ++count;
        };
        for (int i = 0; i < n; ++i) {
            // gap 1: 0-47; per sector (396): 0-13 address, 14-19 gap 2,
            // 20-368 data, 369-395 gap 3
            const int s = i - 48;
            const bool gap = s < 0 || (s % 396 >= 14 && s % 396 < 20) || s % 396 >= 369;
            for (int k = 7; k >= 0; --k) bit((nib[i] >> k) & 1);
            if (gap) { bit(0); bit(0); }
        }
        tracks.push_back(bits);
        counts.push_back(count);
    }
    return makeWoz2(tracks, counts);
}
}  // namespace

// WOZ2: INFO, TMAP and TRKS read; a track's bits and length; an unmapped
// quarter track is noise; the optimal bit timing and write protect
TEST_CASE(disk2_woz_parse) {
    std::vector<uint8_t> dsk(35 * 4096);
    for (auto& b : dsk) b = rnd();
    auto woz = dskToWoz(dsk);
    std::string err;
    auto img = Disk2Image::fromBytes(woz, ".woz", &err);
    ASSERT(img);
    EXPECT_TRUE(img->isWoz());
    EXPECT_EQ(img->tracks(), 40);
    EXPECT_EQ(img->optimalBitTiming(), 32);
    EXPECT_TRUE(!img->writeProtected());
    std::vector<uint8_t> buf(img->maxTrackBytes());
    uint32_t bits = 0;
    const int n = img->wozReadTrack(4 * 17, buf.data(), bits);    // track 17
    EXPECT_EQ(bits, (uint32_t)(48 * 10 + 16 * (14 * 8 + 6 * 10 + 349 * 8 + 27 * 10)));
    EXPECT_EQ(n, (int)((bits + 7) / 8));
    EXPECT_EQ(buf[0], 0xFF);                                       // a sync byte
    EXPECT_EQ(buf[1] & 0xC0, 0x3F & 0xC0);                         // its two zero bits
    img->wozReadTrack(4 * 17 + 2, buf.data(), bits);              // half track 17.5: unmapped
    EXPECT_EQ(bits, (uint32_t)(Disk2Image::WOZ_EMPTY_TRACK * 8));
    // Timing and write protect from INFO
    auto fast = Disk2Image::fromBytes(makeWoz2({{0xFF}}, {8}, 28, true), ".woz");
    ASSERT(fast);
    EXPECT_EQ(fast->optimalBitTiming(), 28);
    EXPECT_TRUE(fast->writeProtected());
    // Not a WOZ
    auto bad = woz;
    bad[3] = '9';
    EXPECT_TRUE(!Disk2Image::fromBytes(bad, ".woz", &err));
}

// A write to an unmapped quarter track adds a track at the end of TRKS,
// seen from the quarter tracks either side; the CRC is kept
TEST_CASE(disk2_woz_new_track) {
    auto img = Disk2Image::fromBytes(makeWoz2({}, {}), ".woz");
    ASSERT(img);
    std::vector<uint8_t> bits(Disk2Image::WOZ_EMPTY_TRACK);
    for (auto& b : bits) b = rnd();
    img->wozWriteTrack(8, bits.data(), (int)bits.size());         // track 2
    const auto& d = img->data();
    EXPECT_EQ(d[88 + 7], 0);
    EXPECT_EQ(d[88 + 8], 0);
    EXPECT_EQ(d[88 + 9], 0);
    EXPECT_EQ(d[88 + 6], 0xFF);
    const uint32_t crc = d[8] | (d[9] << 8) | (d[10] << 16) | ((uint32_t)d[11] << 24);
    EXPECT_EQ(crc, Disk2Image::crc32(d.data() + 12, d.size() - 12));
    std::vector<uint8_t> back(img->maxTrackBytes());
    uint32_t count = 0;
    const int n = img->wozReadTrack(9, back.data(), count);
    EXPECT_EQ(n, (int)bits.size());
    EXPECT_EQ(count, (uint32_t)bits.size() * 8);
    EXPECT_TRUE(std::equal(bits.begin(), bits.end(), back.begin()));
    // A second track goes after it
    img->wozWriteTrack(16, bits.data(), (int)bits.size());
    EXPECT_EQ(img->data()[88 + 16], 1);
    EXPECT_EQ(img->data()[256 + 8] | (img->data()[256 + 9] << 8), 3 + 13);    // start block
}

// WOZ stepping: two adjacent magnets on put the head between (a quarter
// track); a .dsk's head goes on to the next half track (AppleWin: no
// quarter tracks for .dsk/.nib)
TEST_CASE(disk2_woz_quarter_tracks) {
    for (bool isWoz : {true, false}) {
        Disk2Card c(std::vector<uint8_t>(256, 0));
        c.reset(true);
        std::vector<uint8_t> dsk(35 * 4096, 0);
        c.insert(0, isWoz ? Disk2Image::fromBytes(dskToWoz(dsk), ".woz") : Disk2Image::fromBytes(dsk, ".dsk"));
        c.io(0xC0E9, nullptr, 0);               // motor on
        c.io(0xC0E3, nullptr, 100);             // phase 1 on, with phase 0 off: half track 1
        c.update(200, 100);
        EXPECT_EQ(c.quarterTrack(0), 2);
        c.io(0xC0E5, nullptr, 300);             // phases 1 and 2 on
        c.update(400, 100);
        EXPECT_EQ(c.quarterTrack(0), (isWoz ? 3 : 4));         // .dsk: on to half track 2
        c.io(0xC0E2, nullptr, 500);             // phase 1 off: on to half track 2
        c.update(600, 100);
        EXPECT_EQ(c.quarterTrack(0), 4);
    }
}

// The bit-level sequencer boots a WOZ: the boot ROM reads T0 S0 (10-bit
// syncs, nibbles through the shift register and latch) and runs it
TEST_CASE(apple2_woz_boot_sector) {
    std::string err;
    MachineDescriptor* m = apple2::create(apple2::Model::IIPlus, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    std::vector<uint8_t> d(35 * 4096, 0);
    const uint8_t boot[] = {0x01, 0xA9, 0xA5, 0x8D, 0x00, 0x03, 0xA9, 0xC2, 0x8D, 0x00, 0x04, 0x4C, 0x0B, 0x08};
    std::copy(boot, boot + sizeof boot, d.begin());
    Disk2Card* c = card(m);
    ASSERT(c);
    c->insert(0, Disk2Image::fromBytes(dskToWoz(d), ".woz"));
    m->onReset(*m);
    run(m, 3000000);
    IBus* bus = m->buses[0].bus;
    EXPECT_EQ(bus->peek8(0x300), 0xA5);
    EXPECT_EQ(m->cpus[0].cpu->pc() & 0xFFF0, 0x0800);
    delete m;
}

// DOS 3.3 from a WOZ (AppleWin's System Master made into one, if present):
// boots, and INIT formats a blank WOZ in drive 2 (every track added to the
// file, bit by bit), which then boots by itself and runs its HELLO
TEST_CASE(apple2_woz_dos33_init) {
    const std::string master = "../refs/apple2/AppleWin/bin/DOS 3.3 System Master - 680-0210-A.dsk";
    std::ifstream in(master, std::ios::binary);
    if (!in) { printf("  (skipped: no %s)\n", master.c_str()); return; }
    std::vector<uint8_t> dsk((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::string blank = "/tmp/mmemu_test_blank.woz";
    { auto w = makeWoz2({}, {}); std::ofstream out(blank, std::ios::binary); out.write((const char*)w.data(), (std::streamsize)w.size()); }
    std::string err;
    MachineDescriptor* m = apple2::create(apple2::Model::IIPlus, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    Disk2Card* c = card(m);
    c->insert(0, Disk2Image::fromBytes(dskToWoz(dsk), ".woz"));
    ASSERT(c->mountDisk(2, blank));
    m->onReset(*m);
    run(m, 20000000);
    EXPECT_TRUE(screenHas(m, "DOS VERSION 3.3"));
    IKeyboardMatrix* kbd = nullptr;
    std::vector<IOHandler*> hs;
    m->ioRegistry->enumerate(hs);
    for (auto* h : hs) if ((kbd = dynamic_cast<IKeyboardMatrix*>(h))) break;
    kbd->enqueueText("NEW\n10 PRINT 6*7\nINIT HELLO,D2\n");
    run(m, 60000000);
    c->ejectDisk(2);
    c->ejectDisk(1);
    ASSERT(c->mountDisk(1, blank));
    m->onReset(*m);
    run(m, 20000000);
    EXPECT_TRUE(screenHas(m, "42"));
    delete m;
    std::remove(blank.c_str());
}
