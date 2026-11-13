#include "test_harness.h"
#include "coco_test_rom.h"
#include "plugins/6809/main/cpu6809.h"
#include "plugins/devices/wd17xx/main/wd17xx.h"
#include "libdevices/main/io_registry.h"
#include "libdevices/main/iaudio_output.h"
#include "libdevices/main/ivideo_output.h"
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <memory>
#include <cstdlib>
#include <unistd.h>

using namespace cocotest;

namespace {
coco::System* sysOf(MachineDescriptor* m) { return coco::systemOf(m); }
std::string screenText(MachineDescriptor* m, int n) {
    std::string s;
    for (int i = 0; i < n; ++i) {
        const uint8_t c = m->buses[0].bus->peek8((uint16_t)(MESSAGE_AT + i)) & 0x3F;
        s += (char)(c < 0x20 ? c + 0x40 : c);
    }
    return s;
}
}  // namespace

// A missing Color BASIC names the file it wants
TEST_CASE(coco_missing_rom_message) {
    const char* old = std::getenv("MMEMU_COCO_FIRMWARE");
    std::string keep = old ? old : "";
    setenv("MMEMU_COCO_FIRMWARE", "/nonexistent-coco-roms", 1);
    std::string err;
    MachineDescriptor* m = coco::create(coco::Model::CoCo2, &err);
    if (old) setenv("MMEMU_COCO_FIRMWARE", keep.c_str(), 1); else unsetenv("MMEMU_COCO_FIRMWARE");
    // The machine is made, with a warning, and a stand-in ROM says so on the screen
    ASSERT(m != nullptr);
    ASSERT(err.find("bas13.rom") != std::string::npos);
    ASSERT(err.find("MMEMU_COCO_FIRMWARE") != std::string::npos);
    ASSERT(!m->warnings.empty() && m->warnings[0].find("bas13.rom") != std::string::npos);
    run(m, 50000);
    std::string screen;
    for (int i = 0; i < 18; ++i) screen += (char)m->buses[0].bus->peek8((uint16_t)i);
    ASSERT_EQ(screen, std::string("NO COLOR BASIC ROM"));
    delete m;
}

// The stand-in BASIC boots: clears the screen, prints, and the VDG shows it
TEST_CASE(coco_boot_prints_via_vdg) {
    MachineDescriptor* m = make();
    ASSERT(m);
    run(m, 200000);
    ASSERT_EQ(screenText(m, 11), std::string("HELLO WORLD"));
    coco::System* sys = sysOf(m);
    ASSERT_EQ(sys->sam.f(), 0x0400);
    // 'H' with D6 set: inverse, dark on green; its row 3 is ..X...X. inverted
    const uint8_t fg = Mc6847::GREEN, bg = Mc6847::DARK_GREEN;
    const uint8_t row3[8] = {fg, fg, bg, fg, fg, fg, bg, fg};
    for (int x = 0; x < 8; ++x) ASSERT_EQ(sys->vdg.activeDot(x, 3), row3[x]);
    // A cleared cell (space with D6) is all green
    ASSERT_EQ(sys->vdg.activeDot(8 * 20, 5), Mc6847::GREEN);
    // The frontends' view
    std::vector<IOHandler*> hs;
    m->ioRegistry->enumerate(hs);
    IVideoOutput* video = nullptr;
    for (auto* h : hs) if (auto* v = dynamic_cast<IVideoOutput*>(h)) video = v;
    ASSERT(video);
    auto dim = video->getDimensions();
    ASSERT_EQ(dim.width, 320);
    ASSERT_EQ(dim.height, 240);
    std::vector<uint32_t> px((size_t)dim.width * dim.height);
    video->renderFrame(px.data());
    ASSERT_EQ(px[(size_t)(24 + 3) * 320 + 32 + 2], Mc6847::rgb(bg));
    ASSERT_EQ(px[(size_t)(24 + 3) * 320 + 32 + 0], Mc6847::rgb(fg));
    ASSERT_EQ(px[0], Mc6847::rgb(Mc6847::BLACK));
    delete m;
}

// FS (60 Hz) on PIA0 CB1 interrupts the 6809 through /IRQ
TEST_CASE(coco_fs_irq_60hz) {
    MachineDescriptor* m = make();
    ASSERT(m);
    run(m, 200000);
    const uint8_t before = m->buses[0].bus->peek8(COUNTER);
    run(m, 894886);                       // a second at 0.89 MHz
    const int n = (uint8_t)(m->buses[0].bus->peek8(COUNTER) - before);
    ASSERT(n >= 59 && n <= 61);
    // 262 lines of 57 cycles: 14934 cycles a frame
    coco::System* sys = sysOf(m);
    uint64_t f0 = sys->vdg.frames();
    while (sys->vdg.frames() == f0) m->schedulerStep(*m);
    f0 = sys->vdg.frames();
    const uint64_t c0 = sys->slowCycles();
    while (sys->vdg.frames() < f0 + 10) m->schedulerStep(*m);
    const uint64_t per = (sys->slowCycles() - c0) / 10;
    ASSERT(per >= 14925 && per <= 14945);
    delete m;
}

// The keyboard: a column strobed low on PIA0 port B reads its keys' rows
// low on port A; SHIFT is row 6 column 7; typed text presses keys in turn
TEST_CASE(coco_keyboard_matrix) {
    MachineDescriptor* m = make();
    ASSERT(m);
    run(m, 100000);
    IBus* bus = m->buses[0].bus;
    auto scan = [&](int col) { bus->write8(0xFF02, (uint8_t)~(1 << col)); return (uint8_t)(bus->read8(0xFF00) & 0x7F); };
    ASSERT_EQ(scan(1), 0x7F);
    ASSERT(m->onKey("a", true));
    ASSERT_EQ(scan(1), 0x7E);             // A: row 0 column 1
    ASSERT_EQ(scan(2), 0x7F);
    m->onKey("a", false);
    m->onKey("return", true);             // ENTER: row 6 column 0
    ASSERT_EQ(scan(0), 0x3F);
    m->onKey("return", false);
    m->onKey("9", true);                  // row 5 column 1
    ASSERT_EQ(scan(1), 0x5F);
    m->onKey("9", false);
    // A shifted symbol: '!' is SHIFT with 1 (row 4 column 1)
    m->onKey("exclaim", true);
    ASSERT_EQ(scan(7), 0x3F);
    ASSERT_EQ(scan(1), 0x6F);
    m->onKey("exclaim", false);
    ASSERT_EQ(scan(7), 0x7F);
    ASSERT(!m->onKey("f13", true));
    // All columns at once (BASIC's "any key?" test)
    m->onKey("z", true);                  // row 3 column 2
    bus->write8(0xFF02, 0x00);
    ASSERT_EQ(bus->read8(0xFF00) & 0x7F, 0x77);
    m->onKey("z", false);
    // Typing
    coco::System* sys = sysOf(m);
    sys->keyboard.enqueueText("B");
    bool seen = false;
    for (int i = 0; i < 2000 && !seen; ++i) {
        run(m, 100);
        seen = scan(2) == 0x7E;
    }
    ASSERT(seen);
    delete m;
}

// Joysticks: the comparator (PIA0 PA7) compares the stick the selects
// pick (PIA0 CB2 right/left, CA2 X/Y) with the DAC (PIA1 PA2-7)
TEST_CASE(coco_joystick_comparator) {
    MachineDescriptor* m = make();
    ASSERT(m);
    run(m, 100000);
    IBus* bus = m->buses[0].bus;
    coco::System* sys = sysOf(m);
    auto cmp = [&](uint8_t dac) { bus->write8(0xFF20, (uint8_t)(dac << 2 | 2)); return (bus->read8(0xFF00) & 0x80) != 0; };
    sys->setAxis(0, 0, 40000);            // right X: about 39 of 63
    bus->write8(0xFF01, 0x34);            // CA2 low: X
    bus->write8(0xFF03, 0x35);            // CB2 low: right
    ASSERT(cmp(30));
    ASSERT(cmp(38));
    ASSERT(!cmp(39));
    ASSERT(!cmp(63));
    // Successive approximation, as BASIC's JOYSTK does
    int v = 0;
    for (int bit = 32; bit; bit >>= 1) if (cmp((uint8_t)(v | bit))) v |= bit;
    ASSERT_EQ(v, 38);
    sys->setAxis(1, 1, 10000);            // left Y
    bus->write8(0xFF01, 0x3C);            // CA2 high: Y
    bus->write8(0xFF03, 0x3D);            // CB2 high: left
    v = 0;
    for (int bit = 32; bit; bit >>= 1) if (cmp((uint8_t)(v | bit))) v |= bit;
    ASSERT_EQ(v, 9);
    // The host's digital stick: right to the end; fire on PA0
    bus->write8(0xFF01, 0x34);
    bus->write8(0xFF03, 0x35);
    m->onJoystick(0, 0x1F & ~0x08 & ~0x10);
    ASSERT(cmp(63));
    bus->write8(0xFF02, 0xFF);            // no column strobed
    ASSERT_EQ(bus->read8(0xFF00) & 0x01, 0);
    m->onJoystick(0, 0x1F);
    ASSERT_EQ(bus->read8(0xFF00) & 0x01, 1);
    delete m;
}

// Sound: the 6-bit DAC through the analog mux (PIA1 CB2 on, source 0),
// the single-bit output (PIA1 PB1), and the Orchestra-90's DACs
TEST_CASE(coco_dac_sound) {
    MachineDescriptor* m = make();
    ASSERT(m);
    run(m, 100000);
    IBus* bus = m->buses[0].bus;
    coco::System* sys = sysOf(m);
    float buf[4096];
    auto drain = [&]() { while (sys->sound.pullSamples(buf, 4096) > 0) {} };
    auto last = [&]() { run(m, 2000); int n = sys->sound.pullSamples(buf, 4096); return n ? buf[n - 1] : -9.0f; };
    drain();
    bus->write8(0xFF20, 0xFC);            // DAC 63, mux off
    ASSERT(std::fabs(last()) < 0.01f);
    bus->write8(0xFF23, 0x3F);            // CB2 high: the mux on
    ASSERT(last() > 0.45f);
    bus->write8(0xFF20, 0x02);            // DAC 0
    ASSERT(last() < -0.45f);
    bus->write8(0xFF01, 0x3C);            // CA2 high: source 1 (the cassette), silent
    ASSERT(std::fabs(last()) < 0.01f);
    bus->write8(0xFF01, 0x34);
    bus->write8(0xFF23, 0x37);            // the mux off
    bus->write8(0xFF22, 0x02);            // single-bit sound high
    ASSERT(last() > 0.2f);
    bus->write8(0xFF22, 0x00);
    ASSERT(std::fabs(last()) < 0.01f);
    // The Orchestra-90 at $FF7A/$FF7B
    std::string err;
    ASSERT(m->expansionSlot("cart")->install("orch90", "", err));
    bus->write8(0xFF7A, 0xFF);
    bus->write8(0xFF7B, 0xFF);
    ASSERT(last() > 0.4f);
    auto* o = dynamic_cast<coco::Orchestra90*>(sys->cart());
    ASSERT(o && o->left() == 0xFF && o->right() == 0xFF);
    ASSERT_EQ(sys->sound.nativeSampleRate(), 44055);
    delete m;
}

// A ROM Pak starts itself: CART toggling on PIA1 CB1 raises FIRQ once BASIC
// enables it, and the handler jumps to $C000
TEST_CASE(coco_cartridge_autostart_firq) {
    MachineDescriptor* m = make();
    ASSERT(m);
    std::string err;
    ASSERT(coco::insertRomPak(m, romPak(0xAA), "test.ccc", err));
    m->onReset(*m);
    ASSERT_EQ(m->buses[0].bus->peek8(0xC000), 0x86);
    run(m, 300000);
    ASSERT_EQ(m->buses[0].bus->peek8(CART_MARK), 0xAA);
    ASSERT(coco::cartridgeInfo(m).find("test.ccc") != std::string::npos);
    // Without the pak nothing jumps there
    coco::removeRomPak(m);
    m->onReset(*m);
    run(m, 300000);
    ASSERT_EQ(m->buses[0].bus->peek8(CART_MARK), 0x00);
    ASSERT_EQ(m->buses[0].bus->peek8(0xC000), 0xFF);
    delete m;
}

// The Multi-Pak: $FF7F routes /CTS (bits 4-5) and /SCS (bits 0-1); the
// switch picks both at reset; CART comes from the /CTS slot
TEST_CASE(coco_multipak_switching) {
    MachineDescriptor* m = make();
    ASSERT(m);
    std::string err;
    ASSERT(m->expansionSlot("cart")->install("mpi", "4", err));
    coco::System* sys = sysOf(m);
    auto* mpi = dynamic_cast<coco::MultiPak*>(sys->cart());
    ASSERT(mpi);
    mpi->insert(0, std::unique_ptr<coco::Cart>(new coco::RomPak(romPak(0x11), "one")));
    mpi->insert(3, std::unique_ptr<coco::Cart>(new coco::RomPak(romPak(0x44), "four")));
    m->onReset(*m);
    IBus* bus = m->buses[0].bus;
    ASSERT_EQ(bus->read8(0xFF7F), 0x33);
    ASSERT_EQ(bus->peek8(0xC001), 0x44);
    bus->write8(0xFF7F, 0x00);
    ASSERT_EQ(bus->peek8(0xC001), 0x11);
    bus->write8(0xFF7F, 0x10);            // slot 2: empty
    ASSERT_EQ(bus->peek8(0xC001), 0xFF);
    // Autostart from the switch's slot (4)
    m->onReset(*m);
    run(m, 300000);
    ASSERT_EQ(bus->peek8(CART_MARK), 0x44);
    // Switch 1, through the expansion and a ROM Pak in the switch's slot
    ASSERT(m->expansionSlot("cart")->install("mpi", "1", err));
    m->onReset(*m);
    run(m, 300000);
    ASSERT_EQ(bus->peek8(CART_MARK), 0x11);
    ASSERT(m->expansionSlot("mpi2")->install("rs232", "", err));
    ASSERT(mpi->slot(1) && mpi->slot(1)->kind() == "rs232");
    // A Deluxe RS-232 Pak answers at $FF68-$FF6B from any slot: its status
    bus->write8(0xFF7F, 0x00);
    ASSERT_EQ(bus->read8(0xFF69) & 0x10, 0x10);   // transmit empty
    delete m;
}

// The SAM's map: type 0 ROM above $7FFF, type 1 RAM to $FEFF; memory size
// on PIA1 PB2 (4K low, 16K high, 64K tied to PIA0 PB6)
TEST_CASE(coco_sam_map_and_ram_size) {
    MachineDescriptor* m = make();
    ASSERT(m);
    run(m, 100000);
    IBus* bus = m->buses[0].bus;
    ASSERT_EQ(bus->read8(0xA000), 0x10);  // Color BASIC's first byte
    bus->write8(0xA000, 0x5A);            // ROM: unchanged
    ASSERT_EQ(bus->read8(0xA000), 0x10);
    bus->write8(0xFFDF, 0);               // TY: all RAM
    bus->write8(0xA000, 0x5A);
    ASSERT_EQ(bus->read8(0xA000), 0x5A);
    ASSERT_EQ(bus->read8(0xFFFE), 0xA0);  // vectors still from ROM
    bus->write8(0xFFDE, 0);
    ASSERT_EQ(bus->read8(0xA000), 0x10);
    coco::System* sys = sysOf(m);
    ASSERT_EQ(sys->ramByte(0xA000), 0x5A);
    // 64K: PIA1 PB2 follows PIA0 PB6
    bus->write8(0xFF02, 0xBF);
    ASSERT_EQ(bus->read8(0xFF22) & 0x04, 0);
    bus->write8(0xFF02, 0xFF);
    ASSERT_EQ(bus->read8(0xFF22) & 0x04, 4);
    // 16K: high; 4K: low
    std::string err;
    ASSERT(m->expansionSlot("ram")->install("ram", "16K", err));
    m->onReset(*m);
    run(m, 100000);
    bus->write8(0xFF02, 0xBF);
    ASSERT_EQ(bus->read8(0xFF22) & 0x04, 4);
    ASSERT(m->expansionSlot("ram")->install("ram", "4K", err));
    m->onReset(*m);
    run(m, 100000);
    ASSERT_EQ(bus->read8(0xFF22) & 0x04, 0);
    ASSERT(!m->expansionSlot("ram")->install("ram", "8K", err));
    // 4K in 4K mode: $1000 is the missing second bank, $2000 mirrors $0000
    bus->write8(0xFFDC, 0);
    bus->write8(0xFFDA, 0);
    bus->write8(0x0123, 0x77);
    ASSERT_EQ(bus->read8(0x2123), 0x77);
    ASSERT_EQ(bus->read8(0x1123), 0xFF);
    delete m;
}

// Fast CPU rate (the "speed-up poke"): ROM runs at 1.78 MHz with R0
TEST_CASE(coco_cpu_rate) {
    MachineDescriptor* m = make();
    ASSERT(m);
    run(m, 200000);
    coco::System* sys = sysOf(m);
    // The idle loop (BRA * in ROM): 3 cycles, two from ROM, one dead ($FFFF)
    uint64_t t0 = sys->ticks();
    for (int i = 0; i < 1000; ++i) m->schedulerStep(*m);
    const uint64_t slow = sys->ticks() - t0;
    m->buses[0].bus->write8(0xFFD7, 0);   // R0: address-dependent
    t0 = sys->ticks();
    for (int i = 0; i < 1000; ++i) m->schedulerStep(*m);
    const uint64_t fast = sys->ticks() - t0;
    ASSERT(fast < slow * 2 / 3);
    delete m;
}

// Snapshots: RAM and the chips come back
TEST_CASE(coco_snapshot_roundtrip) {
    MachineDescriptor* m = make();
    ASSERT(m);
    run(m, 200000);
    IBus* bus = m->buses[0].bus;
    std::vector<uint8_t> st(bus->stateSize());
    bus->saveState(st.data());
    const uint16_t f = sysOf(m)->sam.f();
    bus->write8(0x0400, 0x01);
    bus->write8(0xFFC8, 0);
    bus->loadState(st.data());
    ASSERT_EQ(bus->peek8(0x0400), 'H');
    ASSERT_EQ(sysOf(m)->sam.f(), f);
    delete m;
}

// The CoCo 2B's VDG is the T1; machines register by name
TEST_CASE(coco_models) {
    MachineDescriptor* b = make(coco::Model::CoCo2B);
    ASSERT(b);
    ASSERT(sysOf(b)->vdg.isT1());
    ASSERT_EQ(b->machineId, std::string("coco2b"));
    delete b;
    MachineDescriptor* c1 = make(coco::Model::CoCo1, 16);
    ASSERT(c1);
    ASSERT(!sysOf(c1)->vdg.isT1());
    ASSERT_EQ(sysOf(c1)->ramKb(), 16);
    // RESET keeps RAM
    run(c1, 200000);
    c1->buses[0].bus->write8(0x0300, 0x42);
    c1->onKey("reset", true);
    c1->onKey("reset", false);
    run(c1, 1000);
    ASSERT_EQ(c1->buses[0].bus->peek8(0x0300), 0x42);
    // The artifact button cycles the phase
    c1->onKey("artifact", true);
    ASSERT(sysOf(c1)->vdg.artifact() == Mc6847::Artifact::BlueRed);
    c1->onKey("artifact", true);
    ASSERT(sysOf(c1)->vdg.artifact() == Mc6847::Artifact::RedBlue);
    delete c1;
}

// 6809 disassembly works on the CoCo's bus
TEST_CASE(coco_disassembly) {
    MachineDescriptor* m = make();
    ASSERT(m);
    char buf[64];
    m->cpus[0].cpu->disassembleOne(m->buses[0].bus, 0xA000, buf, sizeof buf);
    ASSERT(std::string(buf).find("LDS") != std::string::npos);
    delete m;
}

// The FD-500's latch: HALT enable halts the 6809 until DRQ; INTRQ with
// double density gives /NMI (the controller itself is wired separately)
TEST_CASE(coco_fd500_latch_hook) {
    MachineDescriptor* m = make();
    ASSERT(m);
    std::vector<uint8_t> diskBasic = romPak(0x33);
    char path[] = "/tmp/coco_disk_XXXXXX";
    int fd = mkstemp(path);
    ASSERT(fd >= 0);
    close(fd);
    { std::ofstream f(path, std::ios::binary); f.write((const char*)diskBasic.data(), (std::streamsize)diskBasic.size()); }
    std::string err;
    ASSERT(m->expansionSlot("cart")->install("fd500", path, err));
    std::remove(path);
    coco::System* sys = sysOf(m);
    auto* fdc = dynamic_cast<coco::Fd500*>(sys->cart());
    ASSERT(fdc);
    m->onReset(*m);
    run(m, 100000);
    IBus* bus = m->buses[0].bus;
    ASSERT_EQ(bus->peek8(0xC000), 0x86);  // Disk BASIC's ROM
    ASSERT(!fdc->controller().intrq());   // reset leaves it idle: no NMI before Disk BASIC is up
    bus->write8(0xFF48, 0xD0);            // the command register (force interrupt)
    ASSERT_EQ(fdc->controller().commandRegister(), 0xD0);
    bus->write8(0xFF40, 0x29);            // drive 0, motor on, single density (no NMI)
    ASSERT_EQ(fdc->controller().selectedDrive(), 0);
    ASSERT(fdc->controller().motor());
    ASSERT(!fdc->controller().doubleDensity());
    // HALT: the CPU stops until DRQ
    bus->write8(0xFF40, 0x89);
    const uint64_t c0 = m->cpus[0].cpu->cycles();
    run(m, 1000);
    ASSERT_EQ(m->cpus[0].cpu->cycles(), c0);
    fdc->setDrq(true);
    run(m, 1000);
    ASSERT(m->cpus[0].cpu->cycles() > c0);
    delete m;
}

// The FD-500 with its WD2793: a JVC image mounted as -i / `disk mount 1` does (DRIVE 0), a sector
// read through $FF48-$FF4B as Disk BASIC does it (HALT on DRQ for each byte, INTRQ to NMI at the end)
TEST_CASE(coco_fd500_reads_a_sector) {
    std::vector<uint8_t> diskBasic(0x2000, 0x12);
    coco::Fd500 fd(diskBasic);
    bool halt = false, nmi = false;
    fd.setHalt = [&](bool on) { halt = on; };
    fd.setNmi = [&](bool on) { nmi = on; };
    fd.reset();
    ASSERT(!nmi);

    // A 35-track single-sided JVC image (no header): 18 sectors of 256 bytes a track
    char path[] = "/tmp/coco_fd500_XXXXXX";
    int fdesc = mkstemp(path);
    ASSERT(fdesc >= 0);
    close(fdesc);
    std::string dsk = std::string(path) + ".dsk";
    {
        std::vector<uint8_t> img(35 * 18 * 256, 0xE5);
        for (int i = 0; i < 256; ++i) img[17 * 18 * 256 + 2 * 256 + i] = (uint8_t)(i ^ 0x5A);   // track 17, sector 3
        std::ofstream f(dsk, std::ios::binary);
        f.write((const char*)img.data(), (std::streamsize)img.size());
    }
    std::remove(path);
    ASSERT(fd.mountDisk(0, dsk));
    ASSERT_EQ(fd.diskPath(0), dsk);
    ASSERT(fd.describe().find("drive 0:") != std::string::npos);

    auto tickUntil = [&](std::function<bool()> done, uint64_t limit) {
        for (uint64_t t = 0; t < limit && !done(); t += 16) fd.clock(16);
        return done();
    };
    uint8_t d = 0;
    auto rd = [&](uint16_t a) { fd.read(a, false, true, d); return d; };
    fd.write(0xFF40, false, true, 0x09);                  // drive 0, motor, double density (INTRQ -> NMI)
    // Seek to track 17 (data register = target, then SEEK with the fastest step rate)
    fd.write(0xFF4B, false, true, 17);
    fd.write(0xFF48, false, true, 0x10);
    ASSERT(tickUntil([&] { return nmi; }, 14318180ull * 2));
    ASSERT_EQ(rd(0xFF49), 17);                            // track register
    rd(0xFF48);                                            // status read clears INTRQ
    ASSERT(!nmi);
    // Read sector 3 with HALT enabled: the CPU would sleep until each DRQ
    fd.write(0xFF4A, false, true, 3);
    fd.write(0xFF40, false, true, 0x89);                  // HALT enable
    fd.write(0xFF48, false, true, 0x80);                  // READ SECTOR
    std::vector<uint8_t> got;
    while (got.size() < 256) {
        ASSERT(tickUntil([&] { return !halt || nmi; }, 14318180ull));
        if (nmi) break;
        got.push_back(rd(0xFF4B));
        fd.write(0xFF40, false, true, 0x89);              // Disk BASIC re-enables HALT for each byte
    }
    ASSERT_EQ(got.size(), (size_t)256);
    for (int i = 0; i < 256; ++i) ASSERT_EQ(got[i], (uint8_t)(i ^ 0x5A));
    ASSERT(tickUntil([&] { return nmi; }, 14318180ull));   // INTRQ at the end: NMI, HALT released
    ASSERT(!halt);
    ASSERT_EQ(rd(0xFF48) & 0x1C, 0);                      // no record-not-found, CRC or lost data
    ASSERT(fd.ejectDisk(0));
    ASSERT(fd.diskPath(0).empty());
    std::remove(dsk.c_str());
}
