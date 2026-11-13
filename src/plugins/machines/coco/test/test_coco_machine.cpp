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

// The hi-res joystick interface: the cassette output (DAC above half
// scale) starts a ramp; the comparator reads high once it passes the
// stick. A stand-in for the driver's timing loop counts until it does.
TEST_CASE(coco_hires_joystick) {
    MachineDescriptor* m = make();
    ASSERT(m);
    run(m, 100000);
    IBus* bus = m->buses[0].bus;
    coco::System* sys = sysOf(m);
    std::string err;
    ASSERT(m->expansionSlot("joyright")->install("hires", "", err));
    ASSERT_EQ(m->expansionSlot("joyright")->info(), std::string("hires"));
    ASSERT(!m->expansionSlot("joyleft")->install("cocomax", "", err));
    ASSERT(sys->joyInterface(1) == coco::JoyInterface::Normal);
    ASSERT_EQ(sys->mousePort(), 0);

    // At $1000: interrupts off, X/right selected, DAC non-zero (ramp held),
    // then DAC 0 and count 12-cycle loops until PA7 rises; the count at $0800
    cocotest::Asm a(0x1000, 0x40, 0x12);
    a.emit({0x1A, 0x50});                        // ORCC #$50
    a.emit({0x86, 0xFF, 0x1F, 0x8B});            // LDA #$FF; TFR A,DP
    a.emit({0x86, 0x34, 0x97, 0x01});            // CRA $34: CA2 low (X)
    a.emit({0x97, 0x03});                        // CRB $34: CB2 low (right)
    a.emit({0x86, 0x82, 0x97, 0x20});            // DAC 32: the ramp held
    a.emit({0x8E, 0x00, 0x00});                  // LDX #0
    a.emit({0x86, 0x02, 0x97, 0x20});            // DAC 0: the ramp runs
    const uint16_t loop = a.pc;
    a.emit({0x30, 0x01});                        // LEAX 1,X (5)
    a.emit({0x96, 0x00});                        // LDA <$00 (4)
    a.branch(0x2A, loop);                        // BPL loop (3)
    a.emit({0xBF, 0x08, 0x00});                  // STX $0800
    const uint16_t spin = a.pc;
    a.branch(0x20, spin);
    // (Bus writes from outside run ahead of the CPU's clock: let it catch up)
    auto poke = [&](uint16_t at, uint8_t v) { bus->write8(at, v); run(m, 20); };
    for (size_t i = 0; i < a.b.size(); ++i) poke((uint16_t)(0x1000 + i), a.b[i]);
    auto measure = [&](uint16_t pos) {
        sys->setAxis(0, 0, pos);
        poke(0x0800, 0xFF);
        poke(0x0801, 0xFF);
        m->cpus[0].cpu->setPc(0x1000);
        run(m, 8000);
        return (int)(bus->peek8(0x0800) << 8 | bus->peek8(0x0801));
    };
    // The ramp takes 560 us + the 10-bit position x 4856 us / 1023 (MAME's
    // figures); the first read of $FF00 is 9 cycles after the DAC's write,
    // then one every 12
    auto expect = [](uint16_t pos) {
        const double us = 560.0 + (double)(pos >> 6) * 4856.0 / 1023.0;
        const int t = (int)(us * 14318180.0 / 1e6) / 16;
        return t <= 9 ? 1 : (t - 9 + 11) / 12 + 1;
    };
    for (uint16_t pos : {0, 4096, 16384, 32768, 49152, 65535}) {
        const int n = measure(pos);
        ASSERT(n >= expect(pos) - 1 && n <= expect(pos) + 1);
    }
    ASSERT_EQ(expect(65535), 405);        // about 9 bits from this loop; tighter loops do better
    // Finer than the DAC's 64 steps: 1/256 of the travel moves the count
    const int q1 = measure(16384), q2 = measure(16384 + 256);
    ASSERT(q2 > q1);
    // The other port, without the interface, still compares with the DAC
    sys->setAxis(1, 0, 65535);
    bus->write8(0xFF20, 0x82);
    bus->write8(0xFF03, 0x3C);                   // CB2 high: left
    ASSERT((bus->read8(0xFF00) & 0x80) != 0);
    sys->setAxis(1, 0, 0);
    ASSERT((bus->read8(0xFF00) & 0x80) == 0);
    // The mouse follows the hi-res port
    ASSERT(m->expansionSlot("joyright")->install("normal", "", err));
    ASSERT(m->expansionSlot("joyleft")->install("hires", "", err));
    ASSERT_EQ(sys->mousePort(), 1);
    const uint16_t before = sys->axis(1, 0);
    m->onMouse(10, 0, 0, 0);
    ASSERT_EQ(sys->axis(1, 0), (uint16_t)std::min(65535, before + 2560));
    delete m;
}

// The PIA's /IRQ follows FS's fall on CB1 by 1 us (14 ticks): timed from
// the edge itself, so frame after frame the IRQ comes exactly a frame on
TEST_CASE(coco_pia_irq_delay) {
    MachineDescriptor* m = make();
    ASSERT(m);
    run(m, 200000);                       // the stand-in counts FS IRQs, clearing the flag
    coco::System* sys = sysOf(m);
    ASSERT_EQ(sys->pia0.irqDelay, 14u);
    std::vector<uint64_t> due;
    uint64_t last = sys->pia0.b.irq1At;
    while (due.size() < 4) {
        m->schedulerStep(*m);
        if (sys->pia0.b.irq1At != last) {
            last = sys->pia0.b.irq1At;
            due.push_back(last);
            // Flag set at once, the output only 14 ticks on
            ASSERT(sys->pia0.b.irq1);
            ASSERT_EQ(sys->pia0.irqB(), sys->pia0.now() >= last);
        }
    }
    for (size_t i = 1; i < due.size(); ++i) ASSERT_EQ(due[i] - due[i - 1], (uint64_t)(262 * 912));
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
