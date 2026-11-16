// The TRS-80's add-ons (#354): the WD1010 hard disk, the Exatron Stringy
// Floppy, the hi-res graphics boards, the Orchestra music cards, the Model
// I's second cassette and RS-232 sense switches, the Model 4's CRTC
// registers and the Model III/4's video waits.

#include "test_harness.h"
#include "trs80_test_rom.h"
#include "libdevices/main/io_registry.h"
#include "libdevices/main/ivideo_output.h"
#include <cstdio>
#include <fstream>
#include <string>
#include <unistd.h>

using namespace trs80test;
using trs80::Model;

namespace {

trs80::System* sysOf(MachineDescriptor* m) { return trs80::systemOf(m); }

std::vector<uint32_t> frame(MachineDescriptor* m, int& w, int& h) {
    std::vector<IOHandler*> hs;
    m->ioRegistry->enumerate(hs);
    IVideoOutput* v = nullptr;
    for (auto* x : hs) if (auto* vo = dynamic_cast<IVideoOutput*>(x)) v = vo;
    auto d = v->getDimensions();
    w = d.width;
    h = d.height;
    std::vector<uint32_t> px((size_t)w * h);
    v->renderFrame(px.data());
    return px;
}

std::string tmpPath(const std::string& name) {
    return "/tmp/mmsim_trs80_addons_" + std::to_string((long)getpid()) + "_" + name;
}

constexpr uint32_t FG = 0xFFF0F0F0;

// Select drive/head, cylinder and sector, then a command
void hdSeek(trs80::System* s, int drive, int head, int cyl, int sector) {
    s->out(0xCE, (uint8_t)(drive << 3 | head));
    s->out(0xCC, (uint8_t)cyl);
    s->out(0xCD, (uint8_t)(cyl >> 8));
    s->out(0xCB, (uint8_t)sector);
}

}  // namespace

// The WD1010: Reed images, reads, writes, multiple sectors, errors
TEST_CASE(trs80_hard_disk_wd1010) {
    MachineDescriptor* m = make(Model::M4, idleRom(0x3800));
    ASSERT(m);
    auto* s = sysOf(m);
    // No drive: the controller isn't there
    ASSERT_EQ(s->in(0xCF), 0xFF);
    ASSERT_EQ(s->in(0xC0), 0xFF);

    // A blank 4-cylinder, 2-head image: the header's magic and checksum
    auto img = trs80::HardDisk::create(4, 2);
    ASSERT_EQ(img.size(), (size_t)(256 + 4 * 2 * 32 * 256));
    ASSERT_EQ(img[0], 0x56);
    ASSERT_EQ(img[1], 0xCB);
    ASSERT_EQ(img[28], 4);
    ASSERT_EQ(img[29], 64);
    unsigned sum = 0;
    for (int i = 0; i < 32; ++i) if (i != 3) sum += img[i];
    ASSERT_EQ(img[3], (uint8_t)(sum ^ 0x4C));
    // A known sector: cylinder 2, head 1, sector 5
    const size_t at = 256 + 256 * (size_t)(2 * 2 * 32 + 1 * 32 + 5);
    for (int i = 0; i < 256; ++i) img[at + i] = (uint8_t)(i ^ 0xA5);
    ASSERT(s->hard.load(0, img, ""));
    ASSERT(!s->hard.load(1, std::vector<uint8_t>(512, 0), ""));      // not a Reed image

    s->out(0xC1, 0x08);                                 // device enable
    hdSeek(s, 0, 1, 2, 5);
    ASSERT_EQ(s->in(0xCF), 0x50);                       // ready, seek complete
    s->out(0xCF, 0x20);                                 // read
    ASSERT_EQ(s->in(0xCF), 0x58);                       // DRQ
    bool ok = true;
    for (int i = 0; i < 256; ++i) ok &= s->in(0xC8) == (uint8_t)(i ^ 0xA5);
    ASSERT(ok);
    ASSERT_EQ(s->in(0xCF), 0x50);                       // done
    ASSERT_EQ(s->in(0xCC), 2);
    ASSERT_EQ(s->in(0xCE), 0x01);

    // Write cylinder 1 head 0 sectors 3 and 4 with one multiple-sector command
    hdSeek(s, 0, 0, 1, 3);
    s->out(0xCA, 2);
    s->out(0xCF, 0x34);
    for (int i = 0; i < 512; ++i) s->out(0xC8, (uint8_t)(i * 7));
    ASSERT_EQ(s->in(0xCF), 0x50);
    ASSERT_EQ(s->in(0xCB), 4);
    const auto& d = s->hard.image(0);
    const size_t w = 256 + 256 * (size_t)(1 * 2 * 32 + 3);
    ok = true;
    for (int i = 0; i < 512; ++i) ok &= d[w + i] == (uint8_t)(i * 7);
    ASSERT(ok);
    // And read them back
    hdSeek(s, 0, 0, 1, 3);
    s->out(0xCA, 2);
    s->out(0xCF, 0x24);
    ok = true;
    for (int i = 0; i < 512; ++i) ok &= s->in(0xC8) == (uint8_t)(i * 7);
    ASSERT(ok);

    // A head the drive hasn't: ID not found; a drive not there likewise
    hdSeek(s, 0, 3, 0, 0);
    s->out(0xCF, 0x20);
    ASSERT_EQ(s->in(0xCF), 0x51);
    ASSERT_EQ(s->in(0xC9), 0x10);
    hdSeek(s, 2, 0, 0, 0);
    s->out(0xCF, 0x70);                                 // seek
    ASSERT_EQ(s->in(0xCF) & 0x01, 1);
    // Restore: cylinder 0
    hdSeek(s, 0, 0, 3, 0);
    s->out(0xCF, 0x10);
    ASSERT_EQ(s->in(0xCF), 0x50);
    ASSERT_EQ(s->in(0xCC), 0);

    // Write protected (header byte 7 bit 7): $C0 shows it, writes fail
    img[7] = 0x80;
    ASSERT(s->hard.load(1, img, ""));
    ASSERT_EQ(s->in(0xC0), 0x42);
    hdSeek(s, 1, 0, 0, 0);
    s->out(0xCF, 0x30);
    ASSERT_EQ(s->in(0xCF) & 0x09, 0x01);

    // The directory cylinder written to sector 0 goes to header byte 31
    hdSeek(s, 0, 0, 0, 0);
    s->out(0xCF, 0x30);
    for (int i = 0; i < 256; ++i) s->out(0xC8, i == 2 ? 3 : 0);
    ASSERT_EQ(s->hard.image(0)[31], 3);

    // Mounted from a file, written back on eject; `-i` style mount picks the hard disk
    const std::string path = tmpPath("hd.hdv");
    {
        std::ofstream f(path, std::ios::binary);
        auto blank = trs80::HardDisk::create(2, 1);
        f.write(reinterpret_cast<const char*>(blank.data()), (std::streamsize)blank.size());
    }
    ASSERT(m->ioRegistry->mountDisk(3, path) != nullptr);
    ASSERT_EQ(s->hard.getMountedDiskPath(3), path);
    ASSERT(s->diskPath(2).empty());                     // not the floppy
    hdSeek(s, 2, 0, 1, 31);
    s->out(0xCF, 0x30);
    for (int i = 0; i < 256; ++i) s->out(0xC8, 0x5A);
    s->hard.ejectDisk(3);
    {
        std::ifstream f(path, std::ios::binary);
        std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        ASSERT_EQ(b.size(), (size_t)(256 + 2 * 32 * 256));
        ASSERT_EQ(b[256 + 256 * (32 + 31) + 100], 0x5A);
    }
    std::remove(path.c_str());
    delete m;
}

// The Stringy Floppy: flux runs written come back as the same runs
TEST_CASE(trs80_stringy_floppy) {
    MachineDescriptor* m = make(Model::M1Level2, idleRom());
    ASSERT(m);
    auto* s = sysOf(m);
    // Not fitted: the ports read $FF and $3000 is empty
    ASSERT_EQ(s->in(0xF0), 0xFF);
    std::vector<uint8_t> rom(0x7E0, 0x00);
    rom[0] = 0xC3;
    s->setStringyRom(rom);
    ASSERT_EQ(m->buses[0].bus->read8(0x3000), 0xFF);
    std::string err;
    ASSERT(m->expansionSlot("stringy")->install("stringy", "", err));
    ASSERT_EQ(m->buses[0].bus->read8(0x3000), 0xC3);    // the Exatron's ROM
    ASSERT_EQ(s->in(0xF1), 0x08);                       // no wafer

    trs80::Stringy& st = s->stringy;
    auto blank = trs80::Stringy::create(1, 60);
    ASSERT_EQ(blank.size(), (size_t)(12 + 1024 * 2 * 9 / 8));
    ASSERT(st.load(0, blank, ""));
    ASSERT_EQ(st.in(0xF0, 0), 0x00);

    // Write: flux runs of 1, 2, 3, 1, 1, 2 cells
    const uint32_t C = trs80::Stringy::CELL;
    const int runs[] = {1, 2, 3, 1, 1, 2, 3, 2};
    uint64_t t = 1000;
    uint8_t flux = 0;
    st.out(0xF0, 0x05, t);                              // motor, write gate
    for (int r : runs) {
        t += (uint64_t)r * C;
        flux ^= 0x80;
        st.out(0xF0, (uint8_t)(0x05 | flux), t);
    }
    t += 3 * C;
    st.out(0xF0, 0x00, t);                              // stop
    // Read: sample the flux every quarter cell and measure its runs
    t += 100000;
    st.out(0xF0, 0x01, t);                              // motor (reading from the start)
    std::vector<int> got;
    int last = -1;
    uint64_t since = 0;
    for (uint64_t k = 0; k < 4 * 20; ++k) {
        const uint64_t now = t + k * C / 4;
        const int f = (st.in(0xF0, now) & 0x80) ? 1 : 0;
        if (last >= 0 && f != last) { got.push_back((int)((now - since + C / 2) / C)); since = now; }
        if (last < 0) since = now;
        last = f;
    }
    // The first change is the start's; the runs follow
    ASSERT(got.size() >= 8);
    bool ok = true;
    for (int i = 0; i < 7; ++i) ok &= got[(size_t)i + 1] == runs[i];
    ASSERT(ok);
    st.out(0xF0, 0x00, t + 30 * C);

    // The leader: end of tape near the loop's end
    const int64_t len = (int64_t)(1024 * 2 * 9 / 8) * C * 8;
    st.out(0xF0, 0x01, 0);
    ASSERT_EQ(st.in(0xF0, (uint64_t)(len - 30 * C)) & 0x04, 0x04);
    ASSERT_EQ(st.in(0xF0, (uint64_t)(len + 1000)) & 0x04, 0x00);     // round again
    st.out(0xF0, 0x00, (uint64_t)(len + 2000));

    // Write protected wafers: bit 0, and nothing is written
    auto wp = trs80::Stringy::create(1, 60, true);
    ASSERT(st.load(1, wp, ""));
    ASSERT_EQ(st.in(0xF1, 0) & 0x01, 1);
    st.out(0xF1, 0x05, 0);
    st.out(0xF1, 0x85, 10 * C);
    st.out(0xF1, 0x00, 20 * C);
    ASSERT(st.image(1) == wp);

    // A file wafer: -i style mount fits the board; written back on eject
    MachineDescriptor* m2 = make(Model::M1Level2, idleRom());
    auto* s2 = sysOf(m2);
    const std::string path = tmpPath("w.esf");
    {
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(blank.data()), (std::streamsize)blank.size());
    }
    ASSERT(m2->ioRegistry->mountDisk(1, path) != nullptr);
    ASSERT(s2->cfg().stringy);
    ASSERT(s2->diskPath(0).empty());
    s2->stringy.out(0xF0, 0x05, 0);
    for (int i = 1; i <= 16; ++i) s2->stringy.out(0xF0, (uint8_t)(0x05 | ((i & 1) ? 0x80 : 0)), (uint64_t)i * C);
    s2->stringy.out(0xF0, 0x00, 17 * C);
    const auto written = s2->stringy.image(0);
    s2->stringy.ejectDisk(1);
    {
        std::ifstream f(path, std::ios::binary);
        std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        ASSERT(b == written);
        ASSERT(b != blank);
    }
    std::remove(path.c_str());
    delete m2;
    delete m;
}

// The hi-res boards: ports, counting, the screen alone and overlaid
TEST_CASE(trs80_hires_graphics) {
    MachineDescriptor* m = make(Model::M4, idleRom(0x3800));
    ASSERT(m);
    auto* s = sysOf(m);
    std::string err;
    ASSERT(m->expansionSlot("hires")->install("tandy", "", err));
    // Write a row of bytes with x counting up (y held after writes)
    s->out(0x83, 0x80);                                 // off; y doesn't move on writes
    s->out(0x80, 10);
    s->out(0x81, 20);
    for (int i = 0; i < 4; ++i) s->out(0x82, (uint8_t)(0xF0 + i));
    ASSERT_EQ(s->hires.x(), 14);
    ASSERT_EQ(s->hires.y(), 20);
    ASSERT_EQ(s->hires.byteAt(12, 20), 0xF2);
    // Read back with x and y both held
    s->out(0x83, 0x30);
    s->out(0x80, 11);
    ASSERT_EQ(s->in(0x82), 0xF1);
    ASSERT_EQ(s->in(0x82), 0xF1);
    // Count down
    s->out(0x83, 0x24 | 0x80);
    s->out(0x80, 11);
    ASSERT_EQ(s->in(0x82), 0xF1);
    ASSERT_EQ(s->in(0x82), 0xF0);

    // On, no overlay: the graphics alone, 640 x 240 (lines doubled)
    s->out(0x83, 0x01);
    int w, h;
    auto px = frame(m, w, h);
    ASSERT_EQ(w, 640);
    ASSERT_EQ(px[(size_t)(20 * 2) * w + 10 * 8], FG);          // $F0: dots 0-3
    ASSERT(px[(size_t)(20 * 2) * w + 10 * 8 + 4] != FG);
    ASSERT_EQ(px[(size_t)(20 * 2 + 1) * w + 13 * 8 + 7], FG);  // $F3: dot 7
    // The offsets scroll it
    s->out(0x8C, 2);
    s->out(0x8D, 4);
    px = frame(m, w, h);
    ASSERT_EQ(px[(size_t)(16 * 2) * w + 8 * 8], FG);
    s->out(0x8C, 0);
    s->out(0x8D, 0);
    // The overlay: XORed with the text (64 x 16: the text area at 64, 24)
    s->out(0x8E, 1);
    m->buses[0].bus->write8(0x3C00, 0xBF);              // all six blocks lit
    s->out(0x80, 0);
    s->out(0x81, 0);
    s->out(0x83, 0x81);
    s->out(0x82, 0xF0);
    px = frame(m, w, h);
    const int x0 = (640 - 64 * 8) / 2, y0 = (240 - 16 * 12) / 2;
    ASSERT(px[(size_t)(y0 * 2) * w + x0] != FG);        // lit text dot XOR graphics dot
    ASSERT_EQ(px[(size_t)(y0 * 2) * w + x0 + 4], FG);   // lit text, no graphics
    ASSERT_EQ(px[(size_t)(y0 * 2) * w + x0 + 8 + 4], 0xFF000000u);  // neither (column 1 is a space)

    // Micro-Labs on the Model III: mode bit 1 turns the text off; port $FF
    // and video RAM reach the graphics memory
    MachineDescriptor* m3 = make(Model::M3, idleRom(0x3800));
    auto* s3 = sysOf(m3);
    ASSERT_EQ(frame(m3, w, h).size(), (size_t)(512 * 384));
    ASSERT(m3->expansionSlot("hires")->install("grafyx", "", err));
    frame(m3, w, h);
    ASSERT_EQ(w, 640);                                  // the screen widens for it
    s3->out(0x83, 0x01);
    ASSERT(s3->hires.overlay());
    s3->out(0x83, 0x03);
    ASSERT(s3->hires.alone());
    s3->out(0xFF, 0xE0 | (5 << 1));                     // coordinates mode, line 5 of each row
    m3->buses[0].bus->write8(0x3C00 + 64 * 2 + 7, 0x81);
    ASSERT_EQ(s3->hires.byteAt(7, 2 * 12 + 5), 0x81);
    ASSERT_EQ(m3->buses[0].bus->read8(0x3C00 + 64 * 2 + 7), 0x81);
    s3->out(0xFF, 0x20);                                // off: video RAM again
    m3->buses[0].bus->write8(0x3C00, 'X');
    ASSERT_EQ(m3->buses[0].bus->read8(0x3C00), 'X');
    // Not fitted: port $82 reads $FF
    MachineDescriptor* m4 = make(Model::M4, idleRom(0x3800));
    ASSERT_EQ(sysOf(m4)->in(0x82), 0xFF);
    delete m4;
    delete m3;
    delete m;
}

// The Orchestra-85/90: two DACs, mixed into the sound
TEST_CASE(trs80_orchestra) {
    MachineDescriptor* m = make(Model::M4, idleRom(0x3800));
    ASSERT(m);
    auto* s = sysOf(m);
    s->out(0x79, 0x40);
    ASSERT_EQ(s->orchestraLeft(), 0);                   // not fitted
    std::string err;
    ASSERT(m->expansionSlot("orchestra")->install("orchestra", "", err));
    s->out(0x79, 0x40);
    s->out(0x75, 0xC0);
    ASSERT_EQ(s->orchestraLeft(), 0x40);
    ASSERT_EQ(s->orchestraRight(), -0x40);
    s->out(0x75, 0x40);
    s->sound.pullSamples(nullptr, 0);
    runFor(m, 0.01);
    float buf[512];
    const int n = s->sound.pullSamples(buf, 512);
    ASSERT(n > 100);
    ASSERT(buf[n - 1] > 0.2f);                          // (64 + 64) / 512
    // The Model I's are $B9 and $B5
    MachineDescriptor* m1 = make(Model::M1Level2, idleRom());
    ASSERT(m1->expansionSlot("orchestra")->install("orchestra", "", err));
    sysOf(m1)->out(0xB9, 0x10);
    sysOf(m1)->out(0xB5, 0x20);
    ASSERT_EQ(sysOf(m1)->orchestraLeft(), 0x10);
    ASSERT_EQ(sysOf(m1)->orchestraRight(), 0x20);
    delete m1;
    delete m;
}

// The Model I's second cassette ($37E4 bit 0) and the RS-232 sense switches
TEST_CASE(trs80_m1_cassette2_and_switches) {
    MachineDescriptor* m = make(Model::M1Level2, idleRom());
    ASSERT(m);
    auto* s = sysOf(m);
    IBus* bus = m->buses[0].bus;
    ASSERT(m->ioRegistry->findHandler("Tape 2") != nullptr);
    s->out(0xFF, 0x04);                                 // motor on: recorder 1
    ASSERT(s->tape.motor());
    ASSERT(!s->tape2.motor());
    bus->write8(0x37E4, 0x01);                          // select recorder 2
    ASSERT(!s->tape.motor());
    ASSERT(s->tape2.motor());
    ASSERT_EQ(s->cassetteSelected(), 1);
    s->out(0xFF, 0x00);
    ASSERT(!s->tape2.motor());
    // Playing recorder 2 sets the flip-flop port $FF reads
    s->tape2.loadCas(std::vector<uint8_t>(64, 0xFF));
    s->tape2.controlTape("play");
    s->out(0xFF, 0x04);
    runFor(m, 0.05);
    ASSERT(s->in(0xFF) & 0x80);
    bus->write8(0x37E4, 0x00);
    s->out(0xFF, 0x04);
    ASSERT(!(s->in(0xFF) & 0x80));                      // recorder 1 is empty
    ASSERT(s->tape.motor());

    // Sense switches: the slot's option
    std::string err;
    ASSERT(m->expansionSlot("rs232")->install("rs232", "", err));
    ASSERT_EQ(s->in(0xE9), 0x6F);                       // 9600 8N1
    ASSERT(m->expansionSlot("rs232")->install("rs232", "300-7E1", err));
    ASSERT_EQ(s->in(0xE9), 0xA2);
    ASSERT(m->expansionSlot("rs232")->install("rs232", "1200-8O2", err));
    ASSERT_EQ(s->in(0xE9), 0x74);
    ASSERT(m->expansionSlot("rs232")->install("rs232", "$15", err));
    ASSERT_EQ(s->in(0xE9), 0x15);
    ASSERT(m->expansionSlot("rs232")->info().find("$15") != std::string::npos);
    ASSERT(!m->expansionSlot("rs232")->install("rs232", "9601-8N1", err));
    ASSERT(!m->expansionSlot("rs232")->install("rs232", "9600-9N1", err));
    delete m;
}

// The Model 4's CRTC registers; the Model III/4's video waits (port $EC bit 5)
TEST_CASE(trs80_crtc_and_video_waits) {
    // A loop storing to video RAM: LD HL,$3C00; loop: LD (HL),A; JR loop
    Asm a(0, 0x3800);
    a.emit({0xF3, 0x3E, 0x20, 0xD3, 0xEC});            // DI; LD A,$20; OUT ($EC),A (waits on)
    a.emit({0x21, 0x00, 0x3C});
    const uint16_t loop = a.pc;
    a.emit({0x77});
    a.jr(0x18, loop);
    auto waits = a.b;
    waits[2] = 0x20;
    auto none = a.b;
    none[2] = 0x00;
    MachineDescriptor* mw = make(Model::M3, waits);
    MachineDescriptor* mn = make(Model::M3, none);
    ASSERT(mw && mn);
    auto* sw = sysOf(mw);
    ASSERT_EQ(sw->videoWaitAt(0), 1024u);               // a shown line: to its blanking
    ASSERT_EQ(sw->videoWaitAt(1000), 24u);
    ASSERT_EQ(sw->videoWaitAt(1100), 0u);               // blanking
    ASSERT_EQ(sw->videoWaitAt(1280 * 200), 0u);         // below the 192 shown lines
    // The same number of instructions takes longer with the waits
    uint64_t tw = 0, tn = 0;
    for (int i = 0; i < 20000; ++i) { tw += (uint64_t)mw->schedulerStep(*mw); tn += (uint64_t)mn->schedulerStep(*mn); }
    ASSERT(sw->videoWaits());
    ASSERT(!sysOf(mn)->videoWaits());
    ASSERT(tw * 2 > tn * 3);
    // The debugger's peek doesn't wait
    mw->buses[0].bus->peek8(0x3C00);

    MachineDescriptor* m4 = make(Model::M4, idleRom(0x3800));
    auto* s4 = sysOf(m4);
    s4->out(0x88, 10);
    s4->out(0x89, 0x60);
    s4->out(0x88, 12);
    s4->out(0x89, 0x01);
    s4->out(0x88, 13);
    s4->out(0x89, 0x40);
    ASSERT_EQ(s4->crtcRegister(10), 0x60);
    ASSERT_EQ(s4->crtcRegister(12), 0x01);
    ASSERT_EQ(s4->crtcStart(), 0x140);
    delete m4;
    delete mw;
    delete mn;
}

// Snapshots keep the add-ons' state
TEST_CASE(trs80_addons_snapshot) {
    MachineDescriptor* m = make(Model::M4, idleRom(0x3800));
    auto* s = sysOf(m);
    std::string err;
    ASSERT(m->expansionSlot("hires")->install("tandy", "", err));
    ASSERT(m->expansionSlot("orchestra")->install("orchestra", "", err));
    s->out(0x80, 3);
    s->out(0x81, 4);
    s->out(0x83, 0xC1);
    s->out(0x82, 0x99);
    s->out(0x79, 0x11);
    s->out(0x88, 7);
    s->out(0x89, 0x22);
    std::vector<uint8_t> snap(s->stateSize());
    s->saveState(snap.data());
    s->out(0x82, 0x00);
    s->out(0x83, 0x00);
    s->out(0x79, 0x00);
    s->out(0x89, 0x00);
    s->loadState(snap.data());
    ASSERT_EQ(s->hires.byteAt(3, 4), 0x99);
    ASSERT(s->hires.enabled());
    ASSERT_EQ(s->orchestraLeft(), 0x11);
    ASSERT_EQ(s->crtcRegister(7), 0x22);
    delete m;
}
