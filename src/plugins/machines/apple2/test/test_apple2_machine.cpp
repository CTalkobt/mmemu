#include "test_harness.h"
#include "plugins/machines/apple2/main/machine_apple2.h"
#include "plugins/machines/apple2/main/smartport_bus.h"
#include "libcore/main/machine_desc.h"
#include "libdevices/main/io_registry.h"
#include "libdevices/main/ikeyboard_matrix.h"
#include "libdevices/main/iaudio_output.h"
#include "libmem/main/ibus.h"
#include "libdevices/main/ivideo_output.h"
#include "libcore/main/icore.h"
#include <cstdio>
#include <fstream>
#include <iterator>
#include <cmath>
#include <filesystem>
#include <cstring>
#include <algorithm>
#include <set>
#include <map>
#include <string>
#include <vector>

namespace {
uint64_t run(MachineDescriptor* m, uint64_t cycles) {
    uint64_t t = 0;
    while (t < cycles) t += (uint64_t)m->schedulerStep(*m);
    return t;
}
// The 40-column text page ($0400): rows interleaved as the Apple II lays
// them out; characters are ASCII with bit 7 set (normal video)
std::string textRow(MachineDescriptor* m, int row) {
    IBus* bus = m->buses[0].bus;
    const uint16_t base = (uint16_t)(0x400 + (row & 7) * 0x80 + (row >> 3) * 0x28);
    std::string s;
    for (int i = 0; i < 40; ++i) s += (char)(bus->peek8(base + (uint16_t)i) & 0x7F);
    return s;
}
bool screenHas(MachineDescriptor* m, const std::string& text) {
    for (int r = 0; r < 24; ++r)
        if (textRow(m, r).find(text) != std::string::npos) return true;
    return false;
}
template <typename T> T* find(MachineDescriptor* m) {
    std::vector<IOHandler*> hs;
    m->ioRegistry->enumerate(hs);
    for (auto* h : hs) if (auto* p = dynamic_cast<T*>(h)) return p;
    return nullptr;
}
}  // namespace

// The II+ autostart monitor shows "APPLE ][" and boots the Disk II in slot
// 6, which waits for a disk; RESET gives Applesoft's "]" prompt; a typed
// line runs
TEST_CASE(apple2_boots_applesoft) {
    std::string err;
    MachineDescriptor* m = apple2::create(apple2::Model::IIPlus, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    run(m, 2000000);
    ASSERT(screenHas(m, "APPLE ]["));
    EXPECT_EQ(m->cpus[0].cpu->pc() & 0xFF00, 0xC600);     // the boot ROM, no disk
    m->onKey("reset", true);
    m->onKey("reset", false);
    run(m, 1000000);
    ASSERT(screenHas(m, "]"));
    auto* kbd = find<IKeyboardMatrix>(m);
    ASSERT(kbd);
    kbd->enqueueText("PRINT 6*7\n");
    run(m, 2000000);
    EXPECT_TRUE(screenHas(m, "42"));
    delete m;
}

// The original II starts in the monitor ("*" prompt)
TEST_CASE(apple2_boots_monitor) {
    std::string err;
    MachineDescriptor* m = apple2::create(apple2::Model::II, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    run(m, 2000000);
    EXPECT_TRUE(screenHas(m, "*"));
    delete m;
}

// The language card (LanguageCard.cpp): $C083 twice: read and write RAM
// bank 2; $C08B bank 1; $C082 ROM; one $C081 read does not write-enable
TEST_CASE(apple2_language_card) {
    std::string err;
    MachineDescriptor* m = apple2::create(apple2::Model::IIPlus, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    IBus* bus = m->buses[0].bus;
    const uint8_t romD000 = bus->read8(0xD000);
    bus->read8(0xC083); bus->read8(0xC083);          // RAM read, write enabled, bank 2
    bus->write8(0xD000, 0x22);
    bus->write8(0xE000, 0x33);
    EXPECT_EQ(bus->read8(0xD000), 0x22);
    bus->read8(0xC08B); bus->read8(0xC08B);          // bank 1
    EXPECT_EQ(bus->read8(0xE000), 0x33);             // $E000-$FFFF is shared
    bus->write8(0xD000, 0x11);
    EXPECT_EQ(bus->read8(0xD000), 0x11);
    bus->read8(0xC083);                              // bank 2 again
    EXPECT_EQ(bus->read8(0xD000), 0x22);
    bus->read8(0xC082);                              // ROM, write protected
    EXPECT_EQ(bus->read8(0xD000), romD000);
    bus->write8(0xD000, 0x99);
    bus->read8(0xC080);                              // RAM read, write protected
    EXPECT_EQ(bus->read8(0xD000), 0x22);
    bus->read8(0xC081);                              // ROM read; one read: not write-enabled
    bus->write8(0xD000, 0x55);
    bus->read8(0xC083); bus->read8(0xC083);
    EXPECT_EQ(bus->read8(0xD000), 0x22);
    delete m;
}

// Keyboard latch: $C000 bit 7 until $C010; the speaker toggles on $C030;
// paddles time out by position (11 cycles a step)
TEST_CASE(apple2_io) {
    std::string err;
    MachineDescriptor* m = apple2::create(apple2::Model::IIPlus, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    run(m, 500000);
    IBus* bus = m->buses[0].bus;
    m->onKey("a", true);
    m->onKey("a", false);
    EXPECT_EQ(bus->read8(0xC000), 0xC1);
    bus->read8(0xC010);
    EXPECT_EQ(bus->read8(0xC000) & 0x80, 0);
    m->onKey("left_shift", true);
    m->onKey("2", true);
    EXPECT_EQ(bus->read8(0xC000), 0x80 | '@');
    m->onKey("left_shift", false);
    m->onKey("control", true);
    m->onKey("c", true);
    EXPECT_EQ(bus->read8(0xC000), 0x83);
    m->onKey("control", false);
    // A tone: the program toggles the speaker; samples come out
    // $0300: LDY #$40; loop: BIT $C030; DEX; BNE *-1; DEY; BNE loop; BRK... JMP $0300
    const uint8_t prog[] = {0xA0, 0x40, 0x2C, 0x30, 0xC0, 0xCA, 0xD0, 0xFD, 0x88, 0xD0, 0xF7, 0x4C, 0x00, 0x03};
    for (size_t i = 0; i < sizeof prog; ++i) bus->write8(0x300 + (uint32_t)i, prog[i]);
    m->cpus[0].cpu->setPc(0x300);
    run(m, 200000);
    std::vector<IOHandler*> hs;
    m->ioRegistry->enumerate(hs);
    IAudioOutput* audio = nullptr;
    for (auto* h : hs) if ((audio = dynamic_cast<IAudioOutput*>(h))) break;
    ASSERT(audio);
    std::vector<float> s(20000);
    const int n = audio->pullSamples(s.data(), (int)s.size());
    EXPECT_TRUE(n > 5000);
    float lo = 1, hi = -1;
    for (int i = 0; i < n; ++i) { lo = std::min(lo, s[i]); hi = std::max(hi, s[i]); }
    EXPECT_TRUE(hi - lo > 0.12f);
    // Paddle 0 at the centre (127): active for ~1400 cycles after $C070
    m->onJoystick(0, 0x1F);
    bus->read8(0xC070);
    EXPECT_EQ(bus->read8(0xC064) & 0x80, 0x80);
    run(m, 2000);
    EXPECT_EQ(bus->read8(0xC064) & 0x80, 0);
    delete m;
}

namespace {
void press(MachineDescriptor* m, const char* k, bool down) { m->onKey(k, down); }
}  // namespace

// The IIe soft switches (Memory.cpp MemSetPaging/UpdatePaging): RAMRD and
// RAMWRT, ALTZP for pages 0-1 and the language card, 80STORE with PAGE2 for
// the text page (and HIRES for hi-res page 1); their status at $C013-$C018
TEST_CASE(apple2e_memory_switches) {
    std::string err;
    MachineDescriptor* m = apple2::create(apple2::Model::IIeEnhanced, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    IBus* bus = m->buses[0].bus;
    bus->write8(0x2000, 0x11);                       // main
    bus->write8(0xC005, 0);                          // RAMWRT on
    bus->write8(0x2000, 0x22);                       // aux
    EXPECT_EQ(bus->read8(0x2000), 0x11);             // reads still main
    EXPECT_EQ(bus->read8(0xC014) & 0x80, 0x80);
    bus->write8(0xC003, 0);                          // RAMRD on
    EXPECT_EQ(bus->read8(0x2000), 0x22);
    EXPECT_EQ(bus->read8(0xC013) & 0x80, 0x80);
    bus->write8(0xC002, 0); bus->write8(0xC004, 0);
    EXPECT_EQ(bus->read8(0x2000), 0x11);
    // ALTZP: zero page, stack and $D000-$FFFF
    bus->write8(0x0080, 0x33);
    bus->write8(0xC009, 0);
    EXPECT_EQ(bus->read8(0xC016) & 0x80, 0x80);
    bus->write8(0x0080, 0x44);
    bus->read8(0xC083); bus->read8(0xC083);          // LC RAM, bank 2, write enabled
    bus->write8(0xD000, 0x55);
    bus->write8(0xC008, 0);                          // main ZP and LC
    EXPECT_EQ(bus->read8(0x0080), 0x33);
    bus->write8(0xD000, 0x66);
    EXPECT_EQ(bus->read8(0xD000), 0x66);
    bus->write8(0xC009, 0);
    EXPECT_EQ(bus->read8(0x0080), 0x44);
    EXPECT_EQ(bus->read8(0xD000), 0x55);
    bus->write8(0xC008, 0);
    // 80STORE: PAGE2 picks aux for $0400-$07FF, with HIRES also $2000-$3FFF
    bus->write8(0xC001, 0);
    EXPECT_EQ(bus->read8(0xC018) & 0x80, 0x80);
    bus->read8(0xC055);                              // PAGE2
    bus->write8(0x0400, 0xC1);                       // aux text
    EXPECT_EQ(bus->read8(0x2000), 0x11);             // HIRES off: main
    bus->read8(0xC057);                              // HIRES
    EXPECT_EQ(bus->read8(0x2000), 0x22);
    bus->read8(0xC054);
    EXPECT_EQ(bus->read8(0x0400) == 0xC1, false);
    bus->read8(0xC055);
    EXPECT_EQ(bus->read8(0x0400), 0xC1);
    bus->read8(0xC054); bus->read8(0xC056);
    bus->write8(0xC000, 0);
    // INTCXROM: the internal ROM at $C600 instead of the Disk II's
    const uint8_t disk = bus->read8(0xC600);
    bus->write8(0xC007, 0);
    EXPECT_EQ(bus->read8(0xC015) & 0x80, 0x80);
    EXPECT_TRUE(bus->read8(0xC600) != disk || bus->read8(0xC601) != 0x20);
    bus->write8(0xC006, 0);
    EXPECT_EQ(bus->read8(0xC600), disk);
    // 80COL and ALTCHARSET status
    bus->write8(0xC00D, 0); bus->write8(0xC00F, 0);
    EXPECT_EQ(bus->read8(0xC01F) & 0x80, 0x80);
    EXPECT_EQ(bus->read8(0xC01E) & 0x80, 0x80);
    // Control-RESET puts the switches back
    bus->write8(0xC005, 0);
    press(m, "reset", true); press(m, "reset", false);
    EXPECT_EQ(bus->read8(0xC014) & 0x80, 0);
    EXPECT_EQ(bus->read8(0xC01F) & 0x80, 0);
    delete m;
}

// The IIe's built-in self-test (Open Apple, Closed Apple and Control-RESET):
// the MMU, IOU, ROM and RAM tests pass ("System OK" on the enhanced IIe,
// "KERNEL OK" on the original once the keys are let go)
TEST_CASE(apple2e_self_test) {
    for (auto model : {apple2::Model::IIeEnhanced, apple2::Model::IIe}) {
        std::string err;
        MachineDescriptor* m = apple2::create(model, &err);
        if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
        run(m, 1000000);
        press(m, "control", true); press(m, "alt", true); press(m, "right_alt", true);
        press(m, "reset", true); press(m, "reset", false);
        press(m, "control", false);
        run(m, 300000);
        press(m, "alt", false); press(m, "right_alt", false);
        uint64_t t = 0;
        const char* want = model == apple2::Model::IIe ? "KERNEL OK" : "System OK";
        while (t < 100000000 && !screenHas(m, want)) { run(m, 1000000); t += 1000000; }
        EXPECT_TRUE(screenHas(m, model == apple2::Model::IIe ? "KERNEL OK" : "System OK"));
        delete m;
    }
}

// The IIe's aux slot, fitted after the machine is made: a RamWorks III's
// 64 KB banks at $C073 (and $C071/5/7), for RAMRD/RAMWRT, ALTZP and the
// aux language card; the video keeps bank 0; banks past the memory float;
// a power cycle picks bank 0; back to the extended 80-column card
TEST_CASE(apple2e_ramworks) {
    EXPECT_EQ(MachineDescriptor::parseMemorySize("512K"), 512u << 10);
    EXPECT_EQ(MachineDescriptor::parseMemorySize("16mb"), 16u << 20);
    EXPECT_EQ(MachineDescriptor::parseMemorySize("65536"), 65536u);
    EXPECT_EQ(MachineDescriptor::parseMemorySize("1G"), 0u);
    EXPECT_EQ(MachineDescriptor::parseMemorySize("K"), 0u);
    std::string err;
    MachineDescriptor* m = apple2::create(apple2::Model::IIeEnhanced, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    auto* slot = m->expansionSlot("aux");
    EXPECT_TRUE(slot != nullptr);
    if (!slot) { delete m; return; }
    EXPECT_EQ(slot->info(), std::string("ext80 (extended 80-column card, 64 KB)"));
    EXPECT_TRUE(!slot->install("ramworks3", "100K", err));
    EXPECT_TRUE(!slot->install("ramworks3", "32M", err));
    EXPECT_TRUE(!slot->install("ext80", "1M", err));
    EXPECT_TRUE(!slot->install("", "", err));
    EXPECT_TRUE(!slot->install("reu", "", err));
    EXPECT_TRUE(slot->install("ramworks3", "", err));        // the default, 1 MB
    EXPECT_EQ(slot->info(), std::string("ramworks3 (RamWorks III, 1 MB)"));
    EXPECT_TRUE(slot->install("ramworks3", "8M", err));
    EXPECT_EQ(slot->info(), std::string("ramworks3 (RamWorks III, 8 MB)"));
    m->onReset(*m);
    IBus* bus = m->buses[0].bus;
    // A byte in each bank at $2000 (RAMRD/RAMWRT) and $0080 (ALTZP), then
    // back: each bank holds its own
    bus->write8(0xC005, 0); bus->write8(0xC003, 0);
    for (int b = 0; b < 128; ++b) {
        bus->write8(0xC073, (uint8_t)b);
        bus->write8(0x2000, (uint8_t)(b ^ 0x5A));
    }
    bus->write8(0xC009, 0);
    for (int b = 0; b < 128; ++b) { bus->write8(b & 1 ? 0xC071 : 0xC077, (uint8_t)b); bus->write8(0x0080, (uint8_t)~b); }
    bool ok = true;
    for (int b = 127; b >= 0; --b) {
        bus->write8(0xC075, (uint8_t)b);
        ok = ok && bus->read8(0x0080) == (uint8_t)~b;
    }
    bus->write8(0xC008, 0);
    for (int b = 0; b < 128; ++b) {
        bus->write8(0xC073, (uint8_t)b);
        ok = ok && bus->read8(0x2000) == (uint8_t)(b ^ 0x5A);
    }
    EXPECT_TRUE(ok);
    // $C070 (the paddles) and $C072 don't select
    bus->write8(0xC073, 3); bus->write8(0xC070, 9); bus->write8(0xC072, 9);
    EXPECT_EQ(bus->read8(0x2000), 3 ^ 0x5A);
    // Past 8 MB: writes are lost, bank 0 untouched
    bus->write8(0xC073, 200);
    bus->write8(0x2000, 0xEE);
    bus->write8(0xC073, 0);
    EXPECT_EQ(bus->read8(0x2000), 0x5A);
    // The aux language card follows the bank
    bus->write8(0xC009, 0);
    bus->read8(0xC083); bus->read8(0xC083);
    bus->write8(0xC073, 1); bus->write8(0xD000, 0x11);
    bus->write8(0xC073, 2); bus->write8(0xD000, 0x22);
    bus->write8(0xC073, 1);
    EXPECT_EQ(bus->read8(0xD000), 0x11);
    bus->write8(0xC008, 0);
    // A power cycle: bank 0, memory cleared
    bus->write8(0xC073, 5);
    m->onReset(*m);
    bus->write8(0xC005, 0); bus->write8(0xC003, 0);
    bus->write8(0x2000, 0x77);
    bus->write8(0xC073, 0);
    EXPECT_EQ(bus->read8(0x2000), 0x77);
    // Back to the 80-column card: $C073 does nothing
    EXPECT_TRUE(slot->install("ext80", "", err));
    m->onReset(*m);
    bus->write8(0xC005, 0); bus->write8(0xC003, 0);
    bus->write8(0x2000, 0x31);
    bus->write8(0xC073, 1);
    EXPECT_EQ(bus->read8(0x2000), 0x31);
    delete m;
    // The IIc has no aux slot to fill (its 128 KB are on the board)
    if (MachineDescriptor* c = apple2::create(apple2::Model::IIc, &err)) {
        EXPECT_TRUE(c->expansionSlot("aux") == nullptr);
        delete c;
    }
}

// The IIe's self-test passes with a RamWorks III in the aux slot (it tests
// bank 0's 64 KB as the 80-column card's)
TEST_CASE(apple2e_ramworks_self_test) {
    std::string err;
    MachineDescriptor* m = apple2::create(apple2::Model::IIeEnhanced, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    EXPECT_TRUE(m->expansionSlot("aux")->install("ramworks3", "3M", err));
    m->onReset(*m);
    run(m, 1000000);
    press(m, "control", true); press(m, "alt", true); press(m, "right_alt", true);
    press(m, "reset", true); press(m, "reset", false);
    press(m, "control", false);
    run(m, 300000);
    press(m, "alt", false); press(m, "right_alt", false);
    uint64_t t = 0;
    while (t < 100000000 && !screenHas(m, "System OK")) { run(m, 1000000); t += 1000000; }
    EXPECT_TRUE(screenHas(m, "System OK"));
    delete m;
}

// IIe keyboard: lower case with Caps Lock off; $C010 reads any-key-down;
// the enhanced IIe shows lower case; PR#3 gives 80 columns from aux memory
TEST_CASE(apple2e_keyboard_and_80_columns) {
    std::string err;
    MachineDescriptor* m = apple2::create(apple2::Model::IIeEnhanced, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    run(m, 2000000);
    press(m, "reset", true); press(m, "reset", false);     // no disk: to BASIC
    run(m, 1000000);
    IBus* bus = m->buses[0].bus;
    press(m, "a", true);
    EXPECT_EQ(bus->read8(0xC000), 0xC1);             // Caps Lock on at power-on
    EXPECT_EQ(bus->read8(0xC010) & 0x80, 0x80);      // still held
    press(m, "a", false);
    EXPECT_EQ(bus->read8(0xC010) & 0x80, 0);
    press(m, "caps_lock", true); press(m, "caps_lock", false);
    press(m, "a", true); press(m, "a", false);
    EXPECT_EQ(bus->read8(0xC000), 0xE1);
    bus->read8(0xC010);
    // Open Apple is button 0
    press(m, "alt", true);
    EXPECT_EQ(bus->read8(0xC061) & 0x80, 0x80);
    press(m, "alt", false);
    EXPECT_EQ(bus->read8(0xC061) & 0x80, 0);
    // 80 columns: alternate columns in aux text memory
    auto* kbd = find<IKeyboardMatrix>(m);
    kbd->enqueueText("PR#3\nHOME\nPRINT \"ABCD\"\n");
    run(m, 3000000);
    EXPECT_EQ(bus->read8(0xC01F) & 0x80, 0x80);      // 80COL
    // "ABCD" on row 2 (after the PRINT line): A, C in aux; B, D in main
    bus->write8(0xC001, 0);                          // 80STORE
    bus->read8(0xC055);
    const uint8_t a0 = bus->read8(0x0500), a1 = bus->read8(0x0501);
    bus->read8(0xC054);
    const uint8_t m0 = bus->read8(0x0500), m1 = bus->read8(0x0501);
    EXPECT_EQ(a0 & 0x7F, 'A');
    EXPECT_EQ(m0 & 0x7F, 'B');
    EXPECT_EQ(a1 & 0x7F, 'C');
    EXPECT_EQ(m1 & 0x7F, 'D');
    delete m;
}

// The Apple IIc (ROM FF, roms/apple2/apple2c-ff.rom, local only: skipped
// without it). Power-on: "Apple //c", and the empty drive's "Check Disk
// Drive."; with DOS 3.3's System Master it boots and CATALOGs
TEST_CASE(apple2c_boots) {
    std::string err;
    MachineDescriptor* m = apple2::create(apple2::Model::IIc, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    run(m, 3000000);
    EXPECT_TRUE(screenHas(m, "Apple //c"));
    run(m, 9000000);                                 // the drive gives up after a few seconds
    EXPECT_TRUE(screenHas(m, "Check Disk Drive."));
    delete m;
    const std::string master = "../refs/apple2/AppleWin/bin/DOS 3.3 System Master - 680-0210-A.dsk";
    if (FILE* f = std::fopen(master.c_str(), "rb")) {
        std::fclose(f);
        m = apple2::create(apple2::Model::IIc, &err);
        IOHandler* disk = m->ioRegistry->findHandler("Disk II");
        ASSERT(disk && disk->mountDisk(1, master));
        run(m, 12000000);
        find<IKeyboardMatrix>(m)->enqueueText("CATALOG\n");
        run(m, 6000000);
        if (!screenHas(m, "HELLO")) for (int r = 0; r < 24; ++r) printf("    |%s|\n", textRow(m, r).c_str());
        EXPECT_TRUE(screenHas(m, "HELLO"));
        delete m;
    }
}

// The IIc's IOU (MAME apple2e.cpp): with IOUDIS cleared ($C07F),
// $C05B enables the VBL interrupt; at vertical blanking VBLINT ($C019
// bit 7) rises and /IRQ is held until a $C07x access; $C041 reads the
// mask. The ACIAs answer at $C098 and $C0A8 (transmit empty after reset)
TEST_CASE(apple2c_iou_and_serial) {
    std::string err;
    MachineDescriptor* m = apple2::create(apple2::Model::IIc, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    run(m, 1000000);
    IBus* bus = m->buses[0].bus;
    ICore* cpu = m->cpus[0].cpu;
    bus->write8(0x300, 0x4C); bus->write8(0x301, 0x00); bus->write8(0x302, 0x03);   // JMP $300
    cpu->setPc(0x300);
    cpu->regWriteByName("P", 0x34);                  // interrupts masked
    bus->write8(0xC07F, 0);                          // CLRIOUDIS
    bus->write8(0xC05B, 0);                          // EnVBL
    EXPECT_TRUE(bus->read8(0xC041) & 0x80);
    run(m, 20000);                                   // a frame (17030 cycles)
    EXPECT_TRUE(bus->read8(0xC019) & 0x80);
    EXPECT_TRUE(bus->read8(0xC019) & 0x80);          // reading does not clear it
    bus->read8(0xC070);
    EXPECT_TRUE(!(bus->read8(0xC019) & 0x80));
    bus->write8(0xC05A, 0);                          // DisVBL
    run(m, 20000);
    EXPECT_TRUE(!(bus->read8(0xC019) & 0x80));
    bus->write8(0xC07E, 0);                          // SETIOUDIS
    EXPECT_TRUE(bus->read8(0xC07E) & 0x80);
    EXPECT_TRUE(bus->read8(0xC099) & 0x10);          // port 1's ACIA: transmit data register empty
    EXPECT_TRUE(bus->read8(0xC0A9) & 0x10);          // port 2's
    delete m;
}

// The Apple IIgs (ROM 01, roms/apple2/apple2gs-rom01.rom, local only:
// skipped without it). The text page is read from the Mega II's bank $E0.
namespace {
std::string gsRow(MachineDescriptor* m, int row) {
    IBus* bus = m->buses[0].bus;
    const uint32_t base = 0xE00400u + (uint32_t)((row & 7) * 0x80 + (row >> 3) * 0x28);
    std::string s;
    for (int i = 0; i < 40; ++i) {
        const uint8_t c = bus->peek8(base + (uint32_t)i) & 0x7F;
        s += (char)(c < 0x20 ? c + 0x40 : c);
    }
    return s;
}
bool gsHas(MachineDescriptor* m, const std::string& t) {
    for (int r = 0; r < 24; ++r) if (gsRow(m, r).find(t) != std::string::npos) return true;
    return false;
}
void gsDump(MachineDescriptor* m) {
    printf("    PC %06X\n", m->cpus[0].cpu->pc());
    for (int r = 0; r < 24; ++r) printf("    |%s|\n", gsRow(m, r).c_str());
}
}  // namespace

// Power-on: the start-up screen ("Apple IIgs", "ROM Version 01"), then no
// startup device; Control-Reset gives Applesoft, and a typed line runs
TEST_CASE(apple2gs_boots) {
    std::string err;
    MachineDescriptor* m = apple2::create(apple2::Model::IIgs, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    run(m, 1500000);
    EXPECT_TRUE(gsHas(m, "Apple IIgs"));
    EXPECT_TRUE(gsHas(m, "ROM Version 01"));
    run(m, 6000000);
    EXPECT_TRUE(gsHas(m, "Check startup device!"));
    m->onKey("control", true);
    m->onKey("reset", true);
    m->onKey("reset", false);
    m->onKey("control", false);
    run(m, 3000000);
    EXPECT_TRUE(gsHas(m, "]"));
    find<IKeyboardMatrix>(m)->enqueueText("PRINT 6*7\n");
    run(m, 3000000);
    if (!gsHas(m, "42")) gsDump(m);
    EXPECT_TRUE(gsHas(m, "42"));
    delete m;
}

// The IIgs bank map (KEGS moremem.c): writes to the text page of banks
// $00/$01 are shadowed into $E0/$E1 unless $C035 bit 0 inhibits it;
// RAMWRT sends bank $00's writes to $01 (and so $E1); the ROM's banks $FE/$FF;
// $C071-$C07F read the ROM; the language card in bank $00
TEST_CASE(apple2gs_memory_map) {
    std::string err;
    MachineDescriptor* m = apple2::create(apple2::Model::IIgs, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    IBus* bus = m->buses[0].bus;
    bus->write8(0x000400, 0xC1);
    EXPECT_EQ(bus->peek8(0xE00400), 0xC1);
    bus->write8(0x00C035, 0x01);                       // inhibit text page 1 shadowing
    bus->write8(0x000401, 0xC2);
    EXPECT_EQ(bus->peek8(0x000401), 0xC2);
    EXPECT_EQ(bus->peek8(0xE00401), 0x00);
    bus->write8(0x00C035, 0x00);
    bus->write8(0x00C005, 0);                          // RAMWRT: writes to bank $01
    bus->write8(0x000402, 0xC3);
    EXPECT_EQ(bus->peek8(0x010402), 0xC3);
    EXPECT_EQ(bus->peek8(0xE10402), 0xC3);
    EXPECT_EQ(bus->peek8(0x000402), 0x00);
    bus->write8(0x00C004, 0);
    bus->write8(0x052000, 0x5A);                       // fast RAM
    EXPECT_EQ(bus->peek8(0x052000), 0x5A);
    EXPECT_EQ(bus->read8(0x00FFFC), bus->peek8(0xFFFFFC));   // ROM at power-on (RDROM)
    EXPECT_EQ(bus->read8(0x00C071), bus->peek8(0xFFC071));
    bus->read8(0x00C083); bus->read8(0x00C083);        // language card RAM, read and write, bank 2
    bus->write8(0x00D000, 0x77);
    EXPECT_EQ(bus->read8(0x00D000), 0x77);
    bus->read8(0x00C08B); bus->read8(0x00C08B);        // bank 1
    EXPECT_EQ(bus->read8(0x00D000), 0x00);
    bus->read8(0x00C082);                              // ROM again
    EXPECT_EQ(bus->read8(0x00D000), bus->peek8(0xFFD000));
    delete m;
}

// Super Hi-Res (KEGS video.c): 320 mode with a palette, fill mode, 640
// mode's four palette groups, lines doubled inside the border; the
// scan-line interrupt sets $C023 bits 7 and 5 for a line whose SCB has
// bit 6, and $C032 clears it
TEST_CASE(apple2gs_super_hires) {
    std::string err;
    MachineDescriptor* m = apple2::create(apple2::Model::IIgs, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    IBus* bus = m->buses[0].bus;
    ICore* cpu = m->cpus[0].cpu;
    bus->write8(0x000300, 0x80); bus->write8(0x000301, 0xFE);     // BRA *: the CPU waits, interrupts masked
    cpu->setPc(0x000300);
    cpu->regWrite(8 /* P */, 0x34);
    auto pal = [&](int p, int i, uint16_t rgb) {
        bus->write8(0xE19E00u + (uint32_t)(p * 32 + i * 2), rgb & 0xFF);
        bus->write8(0xE19E01u + (uint32_t)(p * 32 + i * 2), rgb >> 8);
    };
    pal(0, 0, 0x000); pal(0, 1, 0xF00); pal(0, 2, 0x0F0); pal(0, 3, 0x00F);
    pal(1, 8, 0xFFF); pal(1, 12, 0x888); pal(1, 1, 0xF0F); pal(1, 7, 0x0FF);
    bus->write8(0xE12000, 0x12);                // line 0, 320 mode: red, green
    bus->write8(0xE12001, 0x30);                // blue, then 0
    bus->write8(0xE19D01, 0x20);                // line 1: fill mode
    bus->write8(0xE120A0, 0x20);                // green, 0 (fill: green)
    bus->write8(0xE19D02, 0x81);                // line 2: 640 mode, palette 1
    bus->write8(0xE12140, 0x07);                // pixels 0,0,1,3: colours 8, 12, 1, 7
    bus->write8(0xE19D0A, 0x40);                // line 10: scan-line interrupt
    bus->write8(0x00C029, 0xC1);                // Super Hi-Res on
    bus->write8(0x00C023, 0x02);                // scan-line interrupts enabled
    run(m, 17030 * 3 * 2);                     // two frames of 1 MHz cycles at 2.8 MHz
    auto* v = find<IVideoOutput>(m);
    ASSERT(v);
    const auto d = v->getDimensions();
    std::vector<uint32_t> fb((size_t)d.width * d.height);
    v->renderFrame(fb.data());
    const int x0 = (d.width - 640) / 2, y0 = (d.height - 400) / 2;
    auto px = [&](int x, int line) { return fb[(size_t)(y0 + 2 * line) * d.width + x0 + x] & 0xFFFFFF; };
    EXPECT_EQ(px(0, 0), 0xFF0000u); EXPECT_EQ(px(1, 0), 0xFF0000u);
    EXPECT_EQ(px(2, 0), 0x00FF00u);
    EXPECT_EQ(px(4, 0), 0x0000FFu);
    EXPECT_EQ(px(6, 0), 0x000000u);              // colour 0, no fill
    EXPECT_EQ(px(2, 1), 0x00FF00u);              // fill: colour 0 repeats green
    EXPECT_EQ(px(0, 2), 0xFFFFFFu);
    EXPECT_EQ(px(1, 2), 0x888888u);
    EXPECT_EQ(px(2, 2), 0xFF00FFu);
    EXPECT_EQ(px(3, 2), 0x00FFFFu);
    EXPECT_EQ(fb[(size_t)(y0 + 1) * d.width + x0] & 0xFFFFFF, 0xFF0000u);   // line doubled
    EXPECT_EQ(bus->read8(0x00C023) & 0xA2, 0xA2);    // VGC and scan-line interrupt flags
    bus->write8(0x00C032, 0x00);
    EXPECT_EQ(bus->read8(0x00C023) & 0xA0, 0x00);
    delete m;
}

// The IIgs's 5.25" drives (the Disk II emulation behind the IWM): DOS 3.3's
// System Master boots from slot 6 and CATALOGs
TEST_CASE(apple2gs_boots_525) {
    const std::string master = "../refs/apple2/AppleWin/bin/DOS 3.3 System Master - 680-0210-A.dsk";
    FILE* f = std::fopen(master.c_str(), "rb");
    if (!f) { printf("  (skipped: no %s)\n", master.c_str()); return; }
    std::fclose(f);
    std::string err;
    MachineDescriptor* m = apple2::create(apple2::Model::IIgs, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    IOHandler* disk = m->ioRegistry->findHandler("Disk II");
    ASSERT(disk && disk->mountDisk(1, master));
    run(m, 40000000);
    find<IKeyboardMatrix>(m)->enqueueText("CATALOG\n");
    run(m, 20000000);
    if (!gsHas(m, "HELLO")) gsDump(m);
    EXPECT_TRUE(gsHas(m, "HELLO"));
    delete m;
}

#include "plugins/machines/apple2/main/gs_drive35.h"

// The 3.5" format (KEGS iwm_nibblize_track_35 / iwm_denib_track35): every
// track side of an 800 KB disk nibblizes and decodes back; a track's
// nibbles rewritten (a sector's data changed) decode into the image; the
// drive's status and commands
TEST_CASE(apple2gs_drive35_format) {
    using apple2::Gs35Drive;
    std::vector<uint8_t> img(Gs35Drive::IMAGE_BYTES);
    for (size_t i = 0; i < img.size(); ++i) img[i] = (uint8_t)(i * 7 + (i >> 9) * 13);
    EXPECT_EQ(Gs35Drive::firstBlock(79, 1) + Gs35Drive::sectorsOn(79), 1600);
    int bad = 0;
    std::vector<uint8_t> nib, bits, out(12 * 512);
    for (int t = 0; t < Gs35Drive::TRACKS; ++t)
        for (int s = 0; s < 2; ++s) {
            const uint8_t* src = &img[(size_t)Gs35Drive::firstBlock(t, s) * 512];
            Gs35Drive::nibblize(src, t, s, nib, bits);
            std::fill(out.begin(), out.end(), 0);
            const int n = Gs35Drive::denibblize(nib, t, s, out.data());
            if (n != Gs35Drive::sectorsOn(t) || std::memcmp(out.data(), src, (size_t)n * 512) != 0) ++bad;
        }
    EXPECT_EQ(bad, 0);
    // 10-bit syncs, D5 AA 96 address fields: 12 on track 0
    Gs35Drive::nibblize(img.data(), 0, 0, nib, bits);
    int fields = 0;
    for (size_t i = 0; i + 2 < nib.size(); ++i) fields += nib[i] == 0xD5 && nib[i + 1] == 0xAA && nib[i + 2] == 0x96;
    EXPECT_EQ(fields, 12);
    EXPECT_EQ(bits[0], 10);

    Gs35Drive d;
    EXPECT_EQ(d.status(0x2, 0), 1);                  // no disk
    ASSERT(d.insertBytes(img));
    EXPECT_EQ(d.status(0x2, 0), 0);
    EXPECT_EQ(d.status(0xA, 0), 0);                  // track 0
    d.command(0x8, 0);                               // motor on
    EXPECT_EQ(d.status(0x8, 0), 0);
    d.command(0x0, 0); d.command(0x4, 0); d.command(0x4, 0);   // step in twice
    EXPECT_EQ(d.track(), 2);
    EXPECT_EQ(d.status(0xA, 0), 1);
    d.status(0x3, 0);                                // upper head
    EXPECT_EQ(d.side(), 1);
    // Read a while: the address fields go by
    int seen = 0;
    uint8_t prev2 = 0, prev = 0;
    for (uint64_t t = 0; t < 14318180 / 5; t += 100) {           // a fifth of a second
        const uint8_t v = d.readNibble(t);
        if (!v) continue;
        if (prev2 == 0xD5 && prev == 0xAA && v == 0x96) ++seen;
        prev2 = prev; prev = v;
    }
    EXPECT_TRUE(seen >= 12);
}

// The IIgs boots a 3.5" disk from slot 5 (the ROM's SmartPort firmware
// through the IWM): block 0 at $0800 shows "BOOT OK", then reads the last
// block (1599: track 79, side 1) through the slot 5 block driver
TEST_CASE(apple2gs_boots_35) {
    std::string err;
    MachineDescriptor* m = apple2::create(apple2::Model::IIgs, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    std::vector<uint8_t> img(apple2::Gs35Drive::IMAGE_BYTES, 0);
    const uint8_t boot[] = {
        0x01,
        0xA9, 0x01, 0x85, 0x42,             // LDA #1 / STA $42: read
        0x86, 0x43,                         // STX $43: the unit (slot 5, drive 1)
        0xA9, 0x00, 0x85, 0x44,             // buffer $1000
        0xA9, 0x10, 0x85, 0x45,
        0xA9, 0x3F, 0x85, 0x46,             // block $063F
        0xA9, 0x06, 0x85, 0x47,
        0xAD, 0xFF, 0xC5,                   // LDA $C5FF: the driver's offset
        0x8D, 0x1E, 0x08,                   // STA $081E
        0x20, 0x00, 0xC5,                   // $081D JSR $C5xx
        0xA2, 0x00,                         // $0820 LDX #0
        0xBD, 0x00, 0x10,                   // LDA $1000,X
        0x9D, 0x00, 0x04,                   // STA $0400,X
        0xE8, 0xE0, 0x08, 0xD0, 0xF5,       // INX / CPX #8 / BNE
        0xA2, 0x00,                         // $082D LDX #0
        0xBD, 0x40, 0x08,                   // LDA $0840,X
        0x9D, 0x80, 0x04,                   // STA $0480,X
        0xE8, 0xE0, 0x07, 0xD0, 0xF5,       // INX / CPX #7 / BNE
        0x4C, 0x3A, 0x08};                  // $083A JMP $083A
    std::copy(boot, boot + sizeof boot, img.begin());
    const char* ok = "BOOT OK";
    for (int i = 0; i < 7; ++i) img[0x40 + i] = (uint8_t)(ok[i] | 0x80);
    const char* last = "BLK1599!";
    for (int i = 0; i < 8; ++i) img[1599 * 512 + i] = (uint8_t)(last[i] | 0x80);
    auto* drives = dynamic_cast<apple2::Gs35Drives*>(m->ioRegistry->findHandler("3.5\" Drive"));
    ASSERT(drives);
    ASSERT(drives->drive[0].insertBytes(img));
    run(m, 30000000);
    if (!gsHas(m, "BLK1599!")) gsDump(m);
    EXPECT_TRUE(gsHas(m, "BOOT OK"));
    EXPECT_TRUE(gsHas(m, "BLK1599!"));
    delete m;
}

// Writing a 3.5" disk: the boot block writes itself (from $0800) to block
// 1000 through the slot 5 block driver; the image then holds it there
TEST_CASE(apple2gs_writes_35) {
    std::string err;
    MachineDescriptor* m = apple2::create(apple2::Model::IIgs, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    std::vector<uint8_t> img(apple2::Gs35Drive::IMAGE_BYTES, 0);
    const uint8_t boot[] = {
        0x01,
        0xA9, 0x02, 0x85, 0x42,             // LDA #2 / STA $42: write
        0x86, 0x43,                         // STX $43
        0xA9, 0x00, 0x85, 0x44,             // buffer $0800 (this block)
        0xA9, 0x08, 0x85, 0x45,
        0xA9, 0xE8, 0x85, 0x46,             // block $03E8 (1000)
        0xA9, 0x03, 0x85, 0x47,
        0xAD, 0xFF, 0xC5,                   // LDA $C5FF
        0x8D, 0x1E, 0x08,                   // STA $081E
        0x20, 0x00, 0xC5,                   // $081D JSR $C5xx
        0xA9, 0xD7, 0x90, 0x02, 0xA9, 0xC5, // $0820 LDA #'W' / BCC +2 / LDA #'E'
        0x8D, 0x00, 0x04,                   // STA $0400
        0x4C, 0x29, 0x08};                  // $0829 JMP $0829
    std::copy(boot, boot + sizeof boot, img.begin());
    for (int i = 0x100; i < 0x200; ++i) img[i] = (uint8_t)(i * 3);   // something in the second half
    auto* drives = dynamic_cast<apple2::Gs35Drives*>(m->ioRegistry->findHandler("3.5\" Drive"));
    ASSERT(drives && drives->drive[0].insertBytes(img));
    run(m, 30000000);
    if (!gsHas(m, "W")) gsDump(m);
    EXPECT_TRUE(gsHas(m, "W"));
    const auto& b = drives->drive[0].blocks();
    int diff = 0;                                    // the block as it is in memory (its JSR patched)
    for (int i = 0; i < 512; ++i) diff += b[1000 * 512 + i] != m->buses[0].bus->peek8(0x000800u + (uint32_t)i);
    EXPECT_EQ(diff, 0);
    EXPECT_EQ(b[1000 * 512 + 0x100], img[0x100]);
    delete m;
}

// GS/OS System 6.0.1 (Apple's freely distributed System Disk, 800 KB .2mg
// from the Asimov archive; read from ../refs/apple2gs, skipped without it)
// boots from the 3.5" drive to the Finder: Super Hi-Res 640 mode with the
// white menu bar across the top; the ADB mouse opens the Special menu
TEST_CASE(apple2gs_boots_gsos) {
    const std::string disk = "../refs/apple2gs/sys601-2.2mg";
    FILE* f = std::fopen(disk.c_str(), "rb");
    if (!f) { printf("  (skipped: no %s)\n", disk.c_str()); return; }
    std::fclose(f);
    std::string err;
    MachineDescriptor* m = apple2::create(apple2::Model::IIgs, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    // A copy: GS/OS may write to its boot disk
    const std::string copy = (std::filesystem::temp_directory_path() / "mmemu_test_gsos.2mg").string();
    std::filesystem::copy_file(disk, copy, std::filesystem::copy_options::overwrite_existing);
    IOHandler* d = m->ioRegistry->findHandler("3.5\" Drive");
    ASSERT(d && d->mountDisk(1, copy));
    run(m, 160000000);                               // about 25 s of IIgs time
    IBus* bus = m->buses[0].bus;
    EXPECT_EQ(bus->read8(0x00C029) & 0x80, 0x80);    // Super Hi-Res
    EXPECT_EQ(bus->peek8(0xE19D05) & 0x80, 0x80);    // 640 mode
    auto* v = find<IVideoOutput>(m);
    const auto dim = v->getDimensions();
    std::vector<uint32_t> fb((size_t)dim.width * dim.height);
    v->renderFrame(fb.data());
    const int x0 = (dim.width - 640) / 2, y0 = (dim.height - 400) / 2;
    int white = 0;                                   // the menu bar, line 5
    for (int x = 100; x < 600; ++x) white += (fb[(size_t)(y0 + 10) * dim.width + x0 + x] & 0xFFFFFF) == 0xFFFFFF;
    EXPECT_TRUE(white > 300);
    // The mouse: to the top left, across to "Special", the button held: the
    // menu drops down (white where the desktop pattern was)
    m->onMouse(-1500, -1000, 0, 0);
    run(m, 10000000);
    m->onMouse(330, 10, 0, 0);
    run(m, 10000000);
    m->onMouse(0, 0, 1, 0);
    run(m, 10000000);
    v->renderFrame(fb.data());
    int menu = 0;                                    // line 36, below the title
    for (int x = 330; x < 400; ++x) menu += (fb[(size_t)(y0 + 72) * dim.width + x0 + x] & 0xFFFFFF) == 0xFFFFFF;
    EXPECT_TRUE(menu > 60);
    m->onMouse(0, 0, 0, 0);
    delete m;
    std::filesystem::remove(copy);
}

// A 3.5" WOZ2 (INFO disk type 2, TMAP by track * 2 + side): track 0 side
// 0 has an address-field-like run with 16 zero bits in it; past three zero
// bits the read amplifier gives noise, so those nibbles differ between
// revolutions; a write keeps the track's length and lands in the file
TEST_CASE(apple2gs_woz35_weak_bits) {
    // 10-bit syncs (so the IWM falls into step), the mark, 16 zero bits, more syncs
    std::vector<uint8_t> track(96, 0);
    uint32_t nbits = 0;
    auto out = [&](uint8_t v, int len) {
        for (int k = 0; k < len; ++k, ++nbits)
            if (k < 8 && ((v >> (7 - k)) & 1)) track[nbits >> 3] |= (uint8_t)(0x80 >> (nbits & 7));
    };
    for (int i = 0; i < 30; ++i) out(0xFF, 10);
    for (uint8_t b : {0xD5, 0xAA, 0x96, 0x00, 0x00, 0xDE, 0xAA}) out(b, 8);
    while (nbits + 10 <= 96 * 8) out(0xFF, 10);
    nbits = 96 * 8;
    std::vector<uint8_t> f(1536 + 512, 0);
    auto put32 = [&](size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) f[at + i] = (uint8_t)(v >> (8 * i)); };
    std::memcpy(&f[0], "WOZ2\xFF\x0A\x0D\x0A", 8);
    std::memcpy(&f[12], "INFO", 4); put32(16, 60);
    f[20] = 2; f[21] = 2; f[22] = 0;                                 // version 2, a 3.5" disk, not protected
    f[20 + 37] = 2; f[20 + 39] = 16; f[20 + 44] = 1;                 // two sides, 2 us bits, largest track 1 block
    std::memcpy(&f[80], "TMAP", 4); put32(84, 160);
    std::fill(f.begin() + 88, f.begin() + 248, 0xFF);
    f[88] = 0;                                                       // track 0, side 0
    std::memcpy(&f[248], "TRKS", 4); put32(252, 1280 + 512);
    f[256] = 3; f[258] = 1; put32(260, nbits);  // block 3, 1 block
    std::copy(track.begin(), track.end(), f.begin() + 1536);
    put32(8, apple2::Disk2Image::crc32(f.data() + 12, f.size() - 12));
    const std::string path = (std::filesystem::temp_directory_path() / "mmemu_test_35.woz").string();
    { FILE* o = std::fopen(path.c_str(), "wb"); ASSERT(o); std::fwrite(f.data(), 1, f.size(), o); std::fclose(o); }

    apple2::Gs35Drive d;
    std::string err;
    ASSERT(d.insert(path, &err));
    EXPECT_TRUE(d.isWoz());
    d.command(0x8, 0);                                               // motor on
    const double nibble = apple2::Gs35Drive::TICKS_PER_BIT * 8;
    std::set<std::string> seen;
    std::string cur;
    bool inField = false;
    uint8_t p2 = 0, p1 = 0;
    for (double t = 0; t < nibble * 96 * 12; t += nibble / 4) {      // about 12 revolutions
        const uint8_t v = d.readNibble((uint64_t)t);
        if (!v) continue;
        if (inField) {                                               // the three nibbles after the mark
            cur += (char)v;
            if (cur.size() == 3) { seen.insert(cur); inField = false; }
        }
        if (p2 == 0xD5 && p1 == 0xAA && v == 0x96) { inField = true; cur.clear(); }
        p2 = p1; p1 = v;
    }
    EXPECT_TRUE(seen.size() >= 2);                                   // the weak bits read differently

    // Write mode: eight bytes at the head, then the track back to the file
    for (int i = 0; i < 8; ++i) d.writeNibble(0xB7, (uint64_t)(nibble * (1000 + i)));
    d.endWrite();
    auto img = apple2::Disk2Image::open(path, &err);
    ASSERT(img);
    std::vector<uint8_t> bits(4096);
    uint32_t count = 0;
    img->wozReadTrack(0, bits.data(), count);
    EXPECT_EQ(count, (uint32_t)track.size() * 8);
    int b7 = 0;
    for (size_t i = 0; i + 1 < track.size(); ++i) b7 += bits[i] != track[i];
    EXPECT_TRUE(b7 > 0);
    std::filesystem::remove(path);
}

// Copy-protected IIgs originals from the woz-a-day collection (Internet
// Archive; read from ../refs/apple2gs/woz, skipped without them): Qix's key
// disk check passes (its weak bits) and the title comes up instead of
// "Please insert the QIX key disk"; Block Out reaches its title
TEST_CASE(apple2gs_boots_woz_originals) {
    struct Game { const char* file; int x, y; };
    for (const Game& g : {Game{"qix.woz", 200, 150}, Game{"blockout.woz", 352, 170}}) {
        const std::string src = std::string("../refs/apple2gs/woz/") + g.file;
        FILE* f = std::fopen(src.c_str(), "rb");
        if (!f) { printf("  (skipped: no %s)\n", src.c_str()); continue; }
        std::fclose(f);
        std::string err;
        MachineDescriptor* m = apple2::create(apple2::Model::IIgs, &err);
        if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
        const std::string copy = (std::filesystem::temp_directory_path() / g.file).string();
        std::filesystem::copy_file(src, copy, std::filesystem::copy_options::overwrite_existing);
        ASSERT(m->ioRegistry->findHandler("3.5\" Drive")->mountDisk(1, copy));
        run(m, 230000000);
        EXPECT_EQ(m->buses[0].bus->read8(0x00C029) & 0x80, 0x80);
        auto* v = find<IVideoOutput>(m);
        const auto dim = v->getDimensions();
        std::vector<uint32_t> fb((size_t)dim.width * dim.height);
        v->renderFrame(fb.data());
        const uint32_t px = fb[(size_t)g.y * dim.width + g.x] & 0xFFFFFF;
        if (px == 0xFFDDAA) printf("    %s: the key disk notice\n", g.file);
        EXPECT_TRUE(px != 0xFFDDAA && px != 0);
        delete m;
        std::filesystem::remove(copy);
    }
}

#include "plugins/machines/apple2/main/gs_smartport.h"

// The slot 7 SmartPort card (KEGS smartport.c) with a 1.44 MB image: the
// IIgs boots it (block 0 at $0801, X = $70); the boot block reads the last
// block (2879) through the ProDOS entry ($C70A) and block 2814 through the
// SmartPort entry ($C70D, an inline command and list)
TEST_CASE(apple2gs_smartport_boots) {
    std::string err;
    MachineDescriptor* m = apple2::create(apple2::Model::IIgs, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    std::vector<uint8_t> img(2880 * 512, 0);
    const uint8_t boot[] = {
        0x01,
        0xA9, 0x01, 0x85, 0x42, 0x86, 0x43,             // read, unit (X = $70)
        0xA9, 0x00, 0x85, 0x44, 0xA9, 0x10, 0x85, 0x45, // buffer $1000
        0xA9, 0x3F, 0x85, 0x46, 0xA9, 0x0B, 0x85, 0x47, // block $0B3F (2879)
        0x20, 0x0A, 0xC7,                               // JSR $C70A
        0x20, 0x0D, 0xC7, 0x01, 0x80, 0x08,             // JSR $C70D: read, list $0880
        0xA2, 0x00,                                     // $0820 LDX #0
        0xBD, 0x00, 0x10, 0x9D, 0x00, 0x04,             // LDA $1000,X / STA $0400,X
        0xBD, 0x00, 0x11, 0x9D, 0x80, 0x04,             // LDA $1100,X / STA $0480,X
        0xE8, 0xE0, 0x08, 0xD0, 0xEF,                   // INX / CPX #8 / BNE
        0x4C, 0x33, 0x08};                              // $0833 JMP $0833
    std::copy(boot, boot + sizeof boot, img.begin());
    const uint8_t list[] = {0x03, 0x01, 0x00, 0x11, 0xFE, 0x0A, 0x00};   // 3 parameters, unit 1, $1100, block 2814
    std::copy(list, list + sizeof list, img.begin() + 0x80);
    for (int i = 0; i < 8; ++i) img[2879 * 512 + i] = (uint8_t)("PRODOS!!"[i] | 0x80);
    for (int i = 0; i < 8; ++i) img[2814 * 512 + i] = (uint8_t)("SMARTPT!"[i] | 0x80);
    auto* sp = dynamic_cast<apple2::GsSmartPort*>(m->ioRegistry->findHandler("SmartPort"));
    ASSERT(sp && sp->insertBytes(1, img));
    run(m, 20000000);
    if (!gsHas(m, "SMARTPT!")) gsDump(m);
    EXPECT_TRUE(gsHas(m, "PRODOS!!"));
    EXPECT_TRUE(gsHas(m, "SMARTPT!"));
    delete m;
}

// GS/OS System 6 on a 5 MB hard disk image (SYSTEM6.2MG from Asimov's
// system6.hdv.zip; ../refs/apple2gs, skipped without it) boots from the
// slot 7 SmartPort card to the Finder
TEST_CASE(apple2gs_boots_hard_disk) {
    const std::string disk = "../refs/apple2gs/SYSTEM6.2MG";
    FILE* f = std::fopen(disk.c_str(), "rb");
    if (!f) { printf("  (skipped: no %s)\n", disk.c_str()); return; }
    std::fclose(f);
    std::string err;
    MachineDescriptor* m = apple2::create(apple2::Model::IIgs, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    const std::string copy = (std::filesystem::temp_directory_path() / "mmemu_test_hd.2mg").string();
    std::filesystem::copy_file(disk, copy, std::filesystem::copy_options::overwrite_existing);
    ASSERT(m->ioRegistry->findHandler("SmartPort")->mountDisk(1, copy));
    run(m, 300000000);
    EXPECT_EQ(m->buses[0].bus->read8(0x00C029) & 0x80, 0x80);
    auto* v = find<IVideoOutput>(m);
    const auto dim = v->getDimensions();
    std::vector<uint32_t> fb((size_t)dim.width * dim.height);
    v->renderFrame(fb.data());
    const int x0 = (dim.width - 640) / 2, y0 = (dim.height - 400) / 2;
    int white = 0;                                   // the Finder's menu bar
    for (int x = 100; x < 600; ++x) white += (fb[(size_t)(y0 + 10) * dim.width + x0 + x] & 0xFFFFFF) == 0xFFFFFF;
    EXPECT_TRUE(white > 300);
    delete m;
    std::filesystem::remove(copy);
}

// The ADB mouse (KEGS mouse_read_c024): $C027 bit 7 data waiting, bit 1
// the Y delta next; $C024 the X then the Y delta (7 bits signed, at most 63
// a read), the button (0 = down) in the Y read's bit 7; moves in picture
// pixels, halved for 320 mode's X and the doubled lines' Y
TEST_CASE(apple2gs_adb_mouse) {
    std::string err;
    MachineDescriptor* m = apple2::create(apple2::Model::IIgs, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    IBus* bus = m->buses[0].bus;
    EXPECT_EQ(bus->read8(0x00C027) & 0x82, 0x00);
    m->onMouse(10, 20, 0, 0);                        // 320 mode: 5, 10
    EXPECT_EQ(bus->read8(0x00C027) & 0x82, 0x80);
    EXPECT_EQ(bus->read8(0x00C024), 0x85);           // X +5
    EXPECT_EQ(bus->read8(0x00C027) & 0x02, 0x02);    // Y next
    EXPECT_EQ(bus->read8(0x00C024), 0x8A);           // Y +10, the button up
    EXPECT_EQ(bus->read8(0x00C027) & 0x82, 0x00);    // nothing more
    m->onMouse(-6, 0, 1, 0);                         // -3, the button down
    EXPECT_EQ(bus->read8(0x00C024), 0x80 | (uint8_t)(-3 & 0x7F));
    EXPECT_EQ(bus->read8(0x00C024), 0x00);
    m->onMouse(0, 0, 0, 0);                          // released
    EXPECT_EQ(bus->read8(0x00C027) & 0x80, 0x80);
    bus->read8(0x00C024);
    EXPECT_EQ(bus->read8(0x00C024), 0x80);
    m->onMouse(400, 0, 0, 0);                        // +200: 63 a read
    int total = 0;
    for (int i = 0; i < 4; ++i) { total += bus->read8(0x00C024) & 0x7F; bus->read8(0x00C024); }
    EXPECT_EQ(total, 200);
    EXPECT_EQ(bus->read8(0x00C027) & 0x80, 0x00);
    delete m;
}

#include "plugins/machines/apple2/main/gs_doc.h"

// The DOC (MAME es5503.cpp) through the sound GLU: a 256-byte square wave
// in sound RAM, oscillator 0 free-running at a set frequency, the GLU at
// full volume: the output swings at the expected pitch; a one-shot halts at
// the table's end and raises its interrupt, which register $E0 names and
// clears. And the ROM's start-up beep comes out of the mix.
TEST_CASE(apple2gs_doc_sound) {
    std::string err;
    MachineDescriptor* m = apple2::create(apple2::Model::IIgs, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    auto* snd = dynamic_cast<apple2::GsSound*>(m->ioRegistry->findHandler("Sound"));
    ASSERT(snd);
    auto* ao = dynamic_cast<IAudioOutput*>(snd);
    // The start-up beep
    run(m, 2000000);
    std::vector<float> buf(44100);
    int n = ao->pullSamples(buf.data(), (int)buf.size());
    float peak = 0;
    for (int i = 0; i < n; ++i) peak = std::max(peak, std::fabs(buf[(size_t)i]));
    EXPECT_TRUE(peak > 0.01f);

    // A square wave: osc 0, table at $0000 (256 bytes), resolution 0
    IBus* bus = m->buses[0].bus;
    snd->reset();
    for (int i = 0; i < 256; ++i) snd->ram()[i] = i < 128 ? 0x20 : 0xE0;
    auto doc = [&](uint8_t reg, uint8_t v) {
        bus->write8(0x00C03C, 0x0F);                 // registers, volume 15
        bus->write8(0x00C03E, reg);
        bus->write8(0x00C03D, v);
    };
    doc(0xE1, 31 << 1);                              // 32 oscillators: 7.16 MHz / 8 / 34 = 26.3 kHz
    doc(0x00, 0x00); doc(0x20, 0x02);                // frequency 512: acc >> 9, one table step a sample
    doc(0x40, 0xFF);                                 // volume
    doc(0x80, 0x00);                                 // table at $0000
    doc(0xC0, 0x00);                                 // 256 bytes, resolution 0
    doc(0xA0, 0x00);                                 // run, free
    while (ao->pullSamples(buf.data(), 4096) > 0) {}
    run(m, 3000000);                                 // about 0.4 s at 2.8 MHz
    n = ao->pullSamples(buf.data(), (int)buf.size());
    ASSERT(n > 10000);
    int crossings = 0;
    for (int i = 1; i < n; ++i) crossings += (buf[(size_t)i - 1] < 0) != (buf[(size_t)i] < 0);
    // Expected: 894886 / 34 / 256 = 102.8 Hz, two crossings a cycle
    const double hz = crossings / 2.0 / (n / 44100.0);
    if (!(hz > 95 && hz < 110)) printf("    %.1f Hz\n", hz);
    EXPECT_TRUE(hz > 95 && hz < 110);

    // One-shot with its interrupt: halts at the end, $E0 names oscillator 0
    doc(0xA0, 0x01);                                 // halt
    doc(0xA0, 0x0A);                                 // one-shot, interrupt enabled, running
    run(m, 200000);
    bus->write8(0x00C03C, 0x0F);
    bus->write8(0x00C03E, 0xA0);
    bus->read8(0x00C03D);
    EXPECT_EQ(bus->read8(0x00C03D) & 1, 1);          // halted
    bus->write8(0x00C03E, 0xE0);
    bus->read8(0x00C03D);
    EXPECT_EQ(bus->read8(0x00C03D) & 0xBE, 0x00);    // bit 7 clear: oscillator 0 interrupted
    EXPECT_TRUE(!snd->irq());
    delete m;
}

#include "plugins/machines/apple2/main/gs_scc.h"

// The Z8530 SCC (KEGS scc.c) at the register level: channel B (port 2) at
// 9600 baud, 8 bits, 1 stop (x16 from the 3.6864 MHz crystal, time
// constant 10); a byte out reaches the far end and the Tx buffer is busy
// for a character's time; bytes in fill the FIFO, raise the Rx interrupt
// (RR3, RR2B's vector) and read back; reset highest IUS clears it; local
// loopback brings a byte straight back
TEST_CASE(apple2gs_scc_registers) {
    apple2::GsScc scc(1020484.0);
    auto wr = [&](int port, int reg, uint8_t v) { scc.writeReg(port, (uint8_t)reg); scc.writeReg(port, v); };
    auto rd = [&](int port, int reg) { scc.writeReg(port, (uint8_t)reg); return scc.readReg(port); };
    const int B = 1;
    wr(B, 9, 0x40);                                  // reset channel B
    wr(B, 4, 0x44);                                  // x16, 1 stop bit
    wr(B, 3, 0xC0);                                  // Rx 8 bits
    wr(B, 5, 0x60);                                  // Tx 8 bits
    wr(B, 11, 0x50);                                 // Rx and Tx clocks from the baud rate generator
    wr(B, 12, 10); wr(B, 13, 0);                     // time constant 10: 9600 baud
    wr(B, 14, 0x01);                                 // generator on, from the crystal
    wr(B, 3, 0xC1); wr(B, 5, 0x68);                  // Rx, Tx on
    const double perChar = 1020484.0 / 9600.0 * 10.0;
    EXPECT_TRUE(std::fabs(scc.charCycles(B) - perChar) < perChar * 0.01);
    std::string out;
    scc.onTransmit(B, [&](uint8_t b) { out += (char)b; });
    EXPECT_EQ(rd(B, 0) & 0x04, 0x04);                // Tx empty
    scc.writeData(B, 'A');
    EXPECT_EQ(rd(B, 0) & 0x04, 0x00);                // busy
    scc.tick((uint64_t)perChar + 1);
    EXPECT_EQ(rd(B, 0) & 0x04, 0x04);
    scc.writeData(B, 'B');
    EXPECT_EQ(out, std::string("AB"));
    // Receive with interrupts: Rx on all characters, master enable
    wr(B, 1, 0x10);
    wr(B, 9, 0x08);
    scc.receive(B, 'x');
    scc.receive(B, 'y');
    EXPECT_EQ(rd(B, 0) & 0x01, 0x01);
    EXPECT_TRUE(scc.irq());
    EXPECT_EQ(rd(0, 3), 0x04);                       // RR3 (channel A): channel B Rx
    EXPECT_EQ(rd(B, 2), 0x04);                       // RR2B: code 2, Ch B Rx
    EXPECT_EQ(scc.readData(B), 'x');
    scc.tick((uint64_t)perChar + 1);                 // the next character arrives
    EXPECT_EQ(scc.readData(B), 'y');
    EXPECT_TRUE(!scc.irq());
    scc.receive(B, 'z');
    scc.tick((uint64_t)perChar + 1);                 // one character time apart
    EXPECT_TRUE(scc.irq());
    wr(B, 0, 0x38);                                  // reset the highest IUS
    EXPECT_TRUE(!scc.irq());
    EXPECT_EQ(scc.readData(B), 'z');
    // Local loopback
    wr(B, 14, 0x11);
    scc.tick(100000);
    scc.writeData(B, 'L');
    scc.tick(10);
    EXPECT_EQ(scc.readData(B), 'L');
}

// The ROM's printer port firmware (slot 1) drives the SCC: from Applesoft,
// PR#1 and PRINT send the text out of port 1
TEST_CASE(apple2gs_scc_printer_port) {
    std::string err;
    MachineDescriptor* m = apple2::create(apple2::Model::IIgs, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    auto* scc = dynamic_cast<apple2::GsScc*>(m->ioRegistry->findHandler("SCC"));
    ASSERT(scc);
    std::string out;
    scc->onTransmit(0, [&](uint8_t b) { out += (char)(b & 0x7F); });
    run(m, 6000000);
    m->onKey("control", true); m->onKey("reset", true); m->onKey("reset", false); m->onKey("control", false);
    run(m, 3000000);
    find<IKeyboardMatrix>(m)->enqueueText("PR#1\nPRINT \"HELLO SCC\"\nPR#0\n");
    run(m, 20000000);
    if (out.find("HELLO SCC") == std::string::npos) printf("    sent: '%s'\n", out.c_str());
    EXPECT_TRUE(out.find("HELLO SCC") != std::string::npos);
    delete m;
}

// ROM 03 (roms/apple2/apple2gs-rom03.rom: 341-0728 for banks $FC-$FD and
// 341-0748 for $FE-$FF, local only): the start-up screen says ROM Version
// 3; ADB answers version 6; GS/OS 6.0.1 boots to the Finder
TEST_CASE(apple2gs_rom03_boots) {
    std::string err;
    MachineDescriptor* m = apple2::create(apple2::Model::IIgsRom03, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    run(m, 1500000);
    if (!gsHas(m, "ROM Version  3")) gsDump(m);
    EXPECT_TRUE(gsHas(m, "Apple IIgs"));
    EXPECT_TRUE(gsHas(m, "ROM Version  3"));
    IBus* bus = m->buses[0].bus;
    EXPECT_EQ(bus->peek8(0xFCFFFF) == 0 && bus->peek8(0xFC0000) == 0, false);   // banks $FC-$FD hold code
    delete m;
    const std::string disk = "../refs/apple2gs/sys601-2.2mg";
    if (!std::filesystem::exists(disk)) { printf("  (GS/OS skipped: no %s)\n", disk.c_str()); return; }
    m = apple2::create(apple2::Model::IIgsRom03, &err);
    const std::string copy = (std::filesystem::temp_directory_path() / "mmemu_test_gsos3.2mg").string();
    std::filesystem::copy_file(disk, copy, std::filesystem::copy_options::overwrite_existing);
    ASSERT(m->ioRegistry->findHandler("3.5\" Drive")->mountDisk(1, copy));
    run(m, 160000000);
    auto* v = find<IVideoOutput>(m);
    const auto dim = v->getDimensions();
    std::vector<uint32_t> fb((size_t)dim.width * dim.height);
    v->renderFrame(fb.data());
    const int x0 = (dim.width - 640) / 2, y0 = (dim.height - 400) / 2;
    int white = 0;
    for (int x = 100; x < 600; ++x) white += (fb[(size_t)(y0 + 10) * dim.width + x0 + x] & 0xFFFFFF) == 0xFFFFFF;
    EXPECT_TRUE(white > 300);
    delete m;
    std::filesystem::remove(copy);
}

// The battery RAM survives a new machine: the ROM sets it up on the first
// run (its checksum), and the next machine reads the same bytes back
TEST_CASE(apple2gs_bram_persists) {
    std::string err;
    MachineDescriptor* m = apple2::create(apple2::Model::IIgs, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    run(m, 6000000);
    delete m;
    const char* dir = std::getenv("MMEMU_DATA_DIR");
    ASSERT(dir);
    const std::string path = std::string(dir) + "/apple2gs-bram-rom01.bin";
    std::ifstream f(path, std::ios::binary);
    ASSERT(f);
    std::vector<uint8_t> bram((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    EXPECT_EQ(bram.size(), 256u);
    int nonzero = 0;
    for (uint8_t b : bram) nonzero += b != 0;
    EXPECT_TRUE(nonzero > 0);                        // the ROM's defaults and checksum
}

// The IIc mouse (MAME apple2e.cpp update_iic_mouse) through the ROM's own
// mouse firmware (slot 4): Applesoft turns it on (PR#4: PRINT CHR$(1)),
// the mouse moves right and down, IN#4 / INPUT reads the position back
TEST_CASE(apple2c_mouse_firmware) {
    std::string err;
    MachineDescriptor* m = apple2::create(apple2::Model::IIc, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    run(m, 3000000);
    m->onKey("control", true); m->onKey("reset", true); m->onKey("reset", false); m->onKey("control", false);
    run(m, 1000000);
    auto* kbd = find<IKeyboardMatrix>(m);
    // A program: the mouse on, wait for a key (the mouse moves meanwhile), read it
    kbd->enqueueText("10 PR#4: PRINT CHR$(1): PR#0\n20 GET A$\n30 IN#4: INPUT \"\";X,Y,S: IN#0\n"
                     "40 PRINT \"POS \";X;\" \";Y\nRUN\n");
    run(m, 4000000);
    m->onMouse(200, 120, 0, 0);                      // +100, +60 dots
    run(m, 3000000);
    kbd->enqueueText(" ");
    run(m, 4000000);
    bool found = false;
    int x = -1, y = -1;
    for (int r = 0; r < 24 && !found; ++r) {
        const std::string row = textRow(m, r);
        const size_t at = row.find("POS ");
        if (at != std::string::npos && std::sscanf(row.c_str() + at, "POS %d %d", &x, &y) == 2) found = true;
    }
    if (!found) for (int r = 0; r < 24; ++r) printf("    |%s|\n", textRow(m, r).c_str());
    ASSERT(found);
    // Right and down, in proportion (the firmware counts the interrupts it
    // sees; steps come a scanline apart, as MAME makes them)
    EXPECT_TRUE(x > 10);
    EXPECT_TRUE(y > 5);
    EXPECT_TRUE(x > y);
    delete m;
}

// The IIc's 32 KB ROMs (00, 03, 04; local only), as MAME runs them: the IWM
// in slot 6 (its registers in front of the 5.25" drives, the SmartPort bus
// answering as empty) and, for 03 and 04, the 1 MB memory expansion at
// $C0C0-$C0C3. Each comes up with "Check Disk Drive."; DOS 3.3 boots and
// CATALOGs; the expansion's pointer steps through its RAM
TEST_CASE(apple2c_32k_roms) {
    const std::string master = "../refs/apple2/AppleWin/bin/DOS 3.3 System Master - 680-0210-A.dsk";
    const bool haveMaster = std::filesystem::exists(master);
    for (auto model : {apple2::Model::IIcRom00, apple2::Model::IIcRom03, apple2::Model::IIcRom04}) {
        std::string err;
        MachineDescriptor* m = apple2::create(model, &err);
        if (!m) { printf("  (skipped: %s)\n", err.c_str()); continue; }
        run(m, 12000000);
        EXPECT_TRUE(screenHas(m, "Apple //c"));
        EXPECT_TRUE(screenHas(m, "Check Disk Drive."));
        IBus* bus = m->buses[0].bus;
        if (model != apple2::Model::IIcRom00) {        // the memory expansion
            bus->write8(0xC0C0, 0x00); bus->write8(0xC0C1, 0x34); bus->write8(0xC0C2, 0x0A);
            bus->write8(0xC0C3, 0x5A); bus->write8(0xC0C3, 0xA5);
            EXPECT_EQ(bus->read8(0xC0C0), 0x02);       // the pointer stepped on
            EXPECT_EQ(bus->read8(0xC0C2), 0xFA);       // bank $0A, $F0 ORed in
            bus->write8(0xC0C0, 0x00);
            EXPECT_EQ(bus->read8(0xC0C3), 0x5A);
            EXPECT_EQ(bus->read8(0xC0C3), 0xA5);
        }
        delete m;
        if (!haveMaster) continue;
        m = apple2::create(model, &err);
        IOHandler* disk = m->ioRegistry->findHandler("Disk II");
        ASSERT(disk && disk->mountDisk(1, master));
        run(m, 12000000);
        find<IKeyboardMatrix>(m)->enqueueText("CATALOG\n");
        run(m, 6000000);
        if (!screenHas(m, "HELLO")) for (int r = 0; r < 24; ++r) printf("    |%s|\n", textRow(m, r).c_str());
        EXPECT_TRUE(screenHas(m, "HELLO"));
        delete m;
    }
}


// SmartPort on the disk port, driven as the host firmware does: bus reset
// (phases 0 and 2), enable (1 and 3), REQ on phase 0, ACK read back; INIT,
// STATUS (the DIB), READBLOCK, WRITEBLOCK (a data packet follows), and a
// bad checksum ignored
namespace {
struct SpHost {
    apple2::SmartPortBus& bus;
    uint64_t t = 0;
    static std::vector<uint8_t> packet(uint8_t dest, uint8_t type, const std::vector<uint8_t>& data) {
        std::vector<uint8_t> out = {0xFF, 0x3F, 0xCF, 0xF3, 0xFC, 0xFF, 0xC3};
        const size_t odd = data.size() % 7, grp = data.size() / 7;
        const uint8_t hdr[7] = {(uint8_t)(0x80 | dest), 0x80, (uint8_t)(0x80 | type), 0x80, 0x80,
                                (uint8_t)(0x80 | odd), (uint8_t)(0x80 | grp)};
        uint8_t chk = 0;
        for (uint8_t b : hdr) { out.push_back(b); chk ^= b; }
        for (uint8_t b : data) chk ^= b;
        if (odd) {
            uint8_t msb = 0x80;
            for (size_t i = 0; i < odd; ++i) if (data[i] & 0x80) msb |= (uint8_t)(0x40 >> i);
            out.push_back(msb);
            for (size_t i = 0; i < odd; ++i) out.push_back(data[i] | 0x80);
        }
        for (size_t g = 0; g < grp; ++g) {
            uint8_t msb = 0x80;
            for (size_t j = 0; j < 7; ++j) if (data[odd + g * 7 + j] & 0x80) msb |= (uint8_t)(0x40 >> j);
            out.push_back(msb);
            for (size_t j = 0; j < 7; ++j) out.push_back(data[odd + g * 7 + j] | 0x80);
        }
        out.push_back(chk | 0xAA);
        out.push_back((uint8_t)((chk >> 1) | 0xAA));
        out.push_back(0xC8);
        return out;
    }
    void step() { t += 40; }
    // Send a packet: true if the unit took it (ACK fell)
    bool send(const std::vector<uint8_t>& pkt) {
        if (!bus.ack(t)) return false;
        bus.phases(0xB, t);
        for (uint8_t b : pkt) { bus.write(b, t); step(); }
        for (int i = 0; i < 4; ++i) step();
        const bool took = !bus.ack(t);
        bus.phases(0xA, t);
        step();
        return took;
    }
    // Read the reply: its status and contents
    bool receive(uint8_t& status, std::vector<uint8_t>& data) {
        for (int i = 0; i < 4 && !bus.ack(t); ++i) step();
        if (!bus.ack(t)) return false;
        bus.phases(0xB, t);
        std::vector<uint8_t> in;
        bool begun = false;
        for (int i = 0; i < 2000; ++i) {
            step();
            const uint8_t v = bus.read(t);
            if (!v) continue;
            if (!begun) { begun = v == 0xC3; continue; }
            in.push_back(v);
            if (v == 0xC8 && in.size() >= 10) {
                const size_t odd = in[5] & 0x7F, grp = in[6] & 0x7F;
                if (in.size() == 7 + (odd ? odd + 1 : 0) + grp * 8 + 3) break;
            }
        }
        for (int i = 0; i < 4; ++i) step();
        const bool done = !bus.ack(t);
        bus.phases(0xA, t);
        step();
        if (in.size() < 10) return false;
        status = in[4] & 0x7F;
        const size_t odd = in[5] & 0x7F, grp = in[6] & 0x7F;
        size_t pos = 7;
        uint8_t chk = 0;
        for (int i = 0; i < 7; ++i) chk ^= in[(size_t)i];
        data.clear();
        if (odd) {
            const uint8_t msb = in[pos++];
            for (size_t i = 0; i < odd; ++i) data.push_back((uint8_t)((in[pos++] & 0x7F) | ((msb << (1 + i)) & 0x80)));
        }
        for (size_t g = 0; g < grp; ++g) {
            const uint8_t msb = in[pos++];
            for (size_t j = 0; j < 7; ++j) data.push_back((uint8_t)((in[pos++] & 0x7F) | ((msb << (1 + j)) & 0x80)));
        }
        for (uint8_t b : data) chk ^= b;
        return done && (uint8_t)((in[pos] & 0x55) | ((in[pos + 1] & 0x55) << 1)) == chk;
    }
};
}  // namespace

TEST_CASE(smartport_bus_packets) {
    apple2::SmartPortBus bus(1.0);
    std::vector<uint8_t> disk(819200);
    for (size_t i = 0; i < disk.size(); ++i) disk[i] = (uint8_t)(i * 7 + i / 512);
    ASSERT(bus.insertBytes(1, disk));
    EXPECT_EQ(bus.units(), 1);
    SpHost h{bus};
    bus.phases(0x5, h.t);                                // bus reset
    bus.phases(0x0, h.t);
    bus.phases(0xA, h.t);                                // enabled
    EXPECT_TRUE(bus.active());
    uint8_t st = 0;
    std::vector<uint8_t> data;
    // INIT: the unit takes ID 1; the last on the chain replies $FF
    ASSERT(h.send(SpHost::packet(1, 0, {0x05, 0x00, 0, 0, 0, 0, 0, 0, 0})));
    ASSERT(h.receive(st, data));
    EXPECT_EQ(st, 0x7F);
    EXPECT_EQ(bus.unitId(1), 1);
    // STATUS 3: the DIB
    ASSERT(h.send(SpHost::packet(1, 0, {0x00, 0x03, 0, 0, 0x03, 0, 0, 0, 0})));
    ASSERT(h.receive(st, data));
    EXPECT_EQ(st, 0);
    ASSERT(data.size() == 25);
    EXPECT_EQ(data[0], 0xF8);
    EXPECT_EQ(data[1] | data[2] << 8, 1600);
    EXPECT_TRUE(std::memcmp(&data[5], "UNIDISK 3.5", 11) == 0);
    // READBLOCK 300
    ASSERT(h.send(SpHost::packet(1, 0, {0x01, 0x03, 0, 0, 0x2C, 0x01, 0, 0, 0})));
    ASSERT(h.receive(st, data));
    EXPECT_EQ(st, 0);
    ASSERT(data.size() == 512);
    EXPECT_TRUE(std::equal(data.begin(), data.end(), disk.begin() + 300 * 512));
    // WRITEBLOCK 5: the command, then the data
    std::vector<uint8_t> blk(512);
    for (int i = 0; i < 512; ++i) blk[(size_t)i] = (uint8_t)(255 - i);
    ASSERT(h.send(SpHost::packet(1, 0, {0x02, 0x03, 0, 0, 0x05, 0, 0, 0, 0})));
    ASSERT(h.send(SpHost::packet(1, 2, blk)));
    ASSERT(h.receive(st, data));
    EXPECT_EQ(st, 0);
    EXPECT_TRUE(std::equal(blk.begin(), blk.end(), bus.image(1).begin() + 5 * 512));
    // Past the end: a bad block
    ASSERT(h.send(SpHost::packet(1, 0, {0x01, 0x03, 0, 0, 0x40, 0x06, 0, 0, 0})));
    ASSERT(h.receive(st, data));
    EXPECT_EQ(st, 0x2D);
    // A bad checksum, or another unit's ID: no ACK
    auto bad = SpHost::packet(1, 0, {0x00, 0x03, 0, 0, 0x00, 0, 0, 0, 0});
    bad[bad.size() - 2] ^= 0x01;
    EXPECT_TRUE(!h.send(bad));
    EXPECT_TRUE(!h.send(SpHost::packet(2, 0, {0x00, 0x03, 0, 0, 0x00, 0, 0, 0, 0})));
    // A bus reset clears the ID
    bus.phases(0x5, h.t);
    EXPECT_EQ(bus.unitId(1), 0);
}

namespace {
// ProDOS 2.4.3 (AppleWin's 140 KB volume) on an 800 KB UniDisk image
std::string uniDiskProdos() {
    const std::string src = "../refs/apple2/AppleWin/bin/ProDOS_2_4_3.po";
    if (!std::filesystem::exists(src)) return "";
    const std::string copy = (std::filesystem::temp_directory_path() / "mmemu_test_unidisk.po").string();
    std::filesystem::copy_file(src, copy, std::filesystem::copy_options::overwrite_existing);
    std::filesystem::resize_file(copy, 819200);
    return copy;
}
}  // namespace

// The IIc's 32 KB ROMs boot a UniDisk 3.5 (slot 5) when the internal drive
// is empty: ProDOS 2.4.3 comes up to its Bitsy Bye menu, the volume in S5,D1
TEST_CASE(apple2c_unidisk_boots) {
    const std::string disk = uniDiskProdos();
    if (disk.empty()) { printf("  (skipped: no ProDOS 2.4.3 image)\n"); return; }
    std::string err;
    MachineDescriptor* m = apple2::create(apple2::Model::IIcRom04, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    IOHandler* uni = m->ioRegistry->findHandler("UniDisk");
    ASSERT(uni && uni->mountDisk(1, disk));
    run(m, 40000000);
    if (!screenHas(m, "S5,D1:/PRODOS.2.4.3")) for (int r = 0; r < 24; ++r) printf("    |%s|\n", textRow(m, r).c_str());
    EXPECT_TRUE(screenHas(m, "S5,D1:/PRODOS.2.4.3"));
    EXPECT_TRUE(screenHas(m, "BASIC.SYSTEM"));
    EXPECT_EQ(static_cast<apple2::SmartPortBus*>(uni)->unitId(1), 1);
    delete m;
    std::filesystem::remove(disk);
}

// The IIgs with only a UniDisk (no Apple 3.5 drives: none holds a disk):
// slot 5 boots GS/OS 6.0.1 from it to the Finder, writing to the disk
TEST_CASE(apple2gs_unidisk_gsos) {
    const std::string src = "../refs/apple2gs/sys601-2.2mg";
    if (!std::filesystem::exists(src)) { printf("  (skipped: no %s)\n", src.c_str()); return; }
    std::string err;
    MachineDescriptor* m = apple2::create(apple2::Model::IIgs, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    const std::string copy = (std::filesystem::temp_directory_path() / "mmemu_test_unidisk.2mg").string();
    std::filesystem::copy_file(src, copy, std::filesystem::copy_options::overwrite_existing);
    auto* uni = find<apple2::SmartPortBus>(m);
    ASSERT(uni && uni->mountDisk(1, copy));
    run(m, 160000000);
    EXPECT_EQ(uni->unitId(1), 1);
    EXPECT_TRUE(uni->commands() > 1000);
    EXPECT_EQ(m->buses[0].bus->read8(0x00C029) & 0x80, 0x80);    // Super Hi-Res
    auto* v = find<IVideoOutput>(m);
    const auto dim = v->getDimensions();
    std::vector<uint32_t> fb((size_t)dim.width * dim.height);
    v->renderFrame(fb.data());
    const int x0 = (dim.width - 640) / 2, y0 = (dim.height - 400) / 2;
    int white = 0;                                   // the menu bar
    for (int x = 100; x < 600; ++x) white += (fb[(size_t)(y0 + 10) * dim.width + x0 + x] & 0xFFFFFF) == 0xFFFFFF;
    EXPECT_TRUE(white > 300);
    std::vector<uint8_t> a, b;
    {
        std::ifstream fa(src, std::ios::binary), fb2(copy, std::ios::binary);
        a.assign(std::istreambuf_iterator<char>(fa), {});
        b.assign(std::istreambuf_iterator<char>(fb2), {});
    }
    EXPECT_TRUE(a.size() == b.size() && a != b);      // GS/OS wrote to it
    delete m;
    std::filesystem::remove(copy);
}

// A Mockingboard fitted in slot 4 at run time: an AY register written the
// way the drivers do it (DDRs, reset, latch, write through port B), read
// back; a tone in the machine's sound; VIA 1's timer 1 interrupting the 6502
// through the II+ monitor's IRQ vector ($03FE)
#include "plugins/machines/apple2/main/mockingboard.h"
TEST_CASE(apple2_mockingboard) {
    std::string err;
    MachineDescriptor* m = apple2::create(apple2::Model::IIPlus, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    auto* slot = m->expansionSlot("slot4");
    ASSERT(slot != nullptr);
    EXPECT_TRUE(m->expansionSlot("slot5") != nullptr);
    EXPECT_TRUE(!slot->install("no-such-card", "", err));
    ASSERT(slot->install("mockingboard", "", err));
    EXPECT_EQ(slot->info(), std::string("mockingboard (Mockingboard)"));
    m->onReset(*m);
    run(m, 1500000);
    IBus* bus = m->buses[0].bus;
    auto* mb = dynamic_cast<apple2::Mockingboard*>(m->ioRegistry->findHandler("Mockingboard (slot 4)"));
    ASSERT(mb != nullptr);
    auto ay = [&](uint16_t base, int reg, uint8_t v) {
        bus->write8(base + 1, (uint8_t)reg); bus->write8(base, 0x07); bus->write8(base, 0x04);   // latch
        bus->write8(base + 1, v); bus->write8(base, 0x06); bus->write8(base, 0x04);              // write
    };
    for (uint16_t base : {0xC400, 0xC480}) {
        bus->write8(base + 3, 0xFF);                // DDRA: out
        bus->write8(base + 2, 0x07);                // DDRB: BC1, BDIR, /RESET
        bus->write8(base, 0x00); bus->write8(base, 0x04);                 // reset the AY
    }
    ay(0xC400, 0, 0xFE);                            // tone A: period $1FE (~125 Hz)
    ay(0xC400, 1, 0x01);
    ay(0xC400, 7, 0x3E);                            // mixer: tone A only
    ay(0xC400, 8, 0x0F);                            // volume A
    ay(0xC480, 7, 0x3F);                            // the other chip: mixer all off (a level)
    ay(0xC480, 8, 0x05);
    EXPECT_EQ(mb->ayReg(0, 0), 0xFE);
    EXPECT_EQ(mb->ayReg(0, 7), 0x3E);
    EXPECT_EQ(mb->ayReg(1, 8), 0x05);
    EXPECT_EQ(mb->ayReg(1, 0), 0x00);
    // Read back through port A (DDRA in, BC1 high)
    bus->write8(0xC401, 8); bus->write8(0xC400, 0x07); bus->write8(0xC400, 0x04);
    bus->write8(0xC403, 0x00);
    bus->write8(0xC400, 0x05);
    EXPECT_EQ(bus->read8(0xC401), 0x0F);
    bus->write8(0xC400, 0x04);
    bus->write8(0xC403, 0xFF);
    // A square wave in the sound
    auto* ao = dynamic_cast<IAudioOutput*>(m->ioRegistry->findHandler("Speaker"));
    ASSERT(ao != nullptr);
    std::vector<float> drain(100000);
    while (ao->pullSamples(drain.data(), (int)drain.size()) > 0) {}
    run(m, 102048);                                 // 0.1 s
    std::vector<float> buf(8000);
    const int n = ao->pullSamples(buf.data(), (int)buf.size());
    EXPECT_TRUE(n > 4000);
    float lo = 1, hi = -1;
    int crossings = 0;
    for (int i = 0; i < n; ++i) {
        lo = std::min(lo, buf[i]); hi = std::max(hi, buf[i]);
        if (i && (buf[i - 1] < 0) != (buf[i] < 0)) ++crossings;
    }
    EXPECT_TRUE(hi - lo > 0.12f);
    if (crossings < 15 || crossings > 35) printf("  %d crossings, %.3f..%.3f\n", crossings, lo, hi);
    EXPECT_TRUE(crossings >= 15 && crossings <= 35);   // ~125 Hz: ~25 sign changes in 0.1 s
    // Timer 1 free-running at $0FFF with its IRQ on; the handler acknowledges
    // it (reading T1C-L) and counts
    static const uint8_t PROGRAM[] = {0x58, 0x4C, 0x01, 0x03};                         // $0300: CLI; JMP $0301
    static const uint8_t HANDLER[] = {0xAD, 0x04, 0xC4, 0xE6, 0x06, 0xA5, 0x45, 0x40};  // $0310: LDA $C404; INC $06; LDA $45; RTI
    for (size_t i = 0; i < sizeof PROGRAM; ++i) bus->write8(0x0300 + (uint32_t)i, PROGRAM[i]);
    for (size_t i = 0; i < sizeof HANDLER; ++i) bus->write8(0x0310 + (uint32_t)i, HANDLER[i]);
    bus->write8(0x03FE, 0x10); bus->write8(0x03FF, 0x03);
    bus->write8(0x06, 0);
    bus->write8(0xC40B, 0x40);                      // ACR: T1 free-running
    bus->write8(0xC40E, 0xC0);                      // IER: T1
    bus->write8(0xC404, 0xFF); bus->write8(0xC405, 0x0F);
    m->cpus[0].cpu->setPc(0x0300);
    run(m, 41000);                                  // ~10 periods of 4097 cycles
    const int irqs = bus->peek8(0x06);
    if (irqs < 9 || irqs > 11) printf("  %d IRQs\n", irqs);
    EXPECT_TRUE(irqs >= 9 && irqs <= 11);
    // Taken out: $C400 floats again and the IRQ count stops
    ASSERT(slot->install("", "", err));
    EXPECT_TRUE(m->ioRegistry->findHandler("Mockingboard (slot 4)") == nullptr);
    delete m;
}

// A Super Serial Card in slot 2 with its own firmware (AppleWin's SSC.rom)
// and the Hayes modem behind it: PR#2 sends "ATI" out of the 6551 (Apple
// characters, bit 7 set); the firmware set the chip up from the DIP
// switches (9600 8N1); the modem answers. The card's $C800 ROM came in when
// its $C200 page was used.
#include "plugins/machines/apple2/main/super_serial.h"
#include "plugins/devices/acia6551/main/hayes_modem.h"
TEST_CASE(apple2_super_serial_card) {
    std::string err;
    MachineDescriptor* m = apple2::create(apple2::Model::IIPlus, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    auto* slot = m->expansionSlot("slot2");
    ASSERT(slot != nullptr);
    EXPECT_TRUE(!slot->install("ssc", "57600", err));
    if (!slot->install("ssc", "", err)) { printf("  (skipped: %s)\n", err.c_str()); delete m; return; }
    EXPECT_EQ(slot->info(), std::string("ssc (Super Serial Card, 9600 bps)"));
    auto* ssc = find<apple2::SuperSerialCard>(m);
    ASSERT(ssc);
    EXPECT_EQ(ssc->dipSwitch1(), 0xE0);              // 9600, communications mode
    std::vector<uint8_t> sent;
    ssc->acia().onTransmit([&](uint8_t b) { sent.push_back(b); });
    ASSERT(ssc->attachSerial("modem"));
    m->onReset(*m);
    run(m, 2000000);
    m->onKey("reset", true); m->onKey("reset", false);
    run(m, 1000000);
    IBus* bus = m->buses[0].bus;
    EXPECT_EQ(bus->peek8(0xC200), 0x2C);             // the firmware's slot page
    find<IKeyboardMatrix>(m)->enqueueText("PR#2:PRINT \"ATI\":PR#0\n");
    // Collect what the modem sends back (the firmware isn't reading it)
    std::string got;
    for (int i = 0; i < 3000000; i += 20) {
        run(m, 20);
        if (ssc->devicePeek(9) & 0x08) got += (char)HayesModem::petsciiToAscii((uint8_t)ssc->deviceIo(8, nullptr, 0));
    }
    EXPECT_TRUE(sent == std::vector<uint8_t>({0xC1, 0xD4, 0xC9, 0x8D}));
    EXPECT_EQ(ssc->devicePeek(11), 0x1E);            // control: 9600 8N1, as SW1 says
    EXPECT_TRUE(got.find("MMEMU HAYES MODEM") != std::string::npos);
    EXPECT_TRUE(got.find("OK") != std::string::npos);
    EXPECT_TRUE(screenHas(m, "]"));                  // back at the prompt
    ASSERT(slot->install("", "", err));
    EXPECT_TRUE(find<apple2::SuperSerialCard>(m) == nullptr);
    delete m;
}

// An AppleMouse II card in slot 4 driven through its own firmware (AppleWin's
// MouseInterface.rom): the Pascal-style entry points at $C412-$C419 give
// INITMOUSE, SETMOUSE, READMOUSE, CLAMPMOUSE and SERVEMOUSE; READMOUSE
// leaves the position and buttons in the slot's screen holes. A VBL
// interrupt comes when SETMOUSE asks; SERVEMOUSE takes it down.
#include "plugins/machines/apple2/main/mouse_card.h"
TEST_CASE(apple2_mouse_card) {
    std::string err;
    MachineDescriptor* m = apple2::create(apple2::Model::IIeEnhanced, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    auto* slot = m->expansionSlot("slot4");
    ASSERT(slot != nullptr);
    EXPECT_TRUE(!slot->install("mouse", "x", err));
    if (!slot->install("mouse", "", err)) { printf("  (skipped: %s)\n", err.c_str()); delete m; return; }
    EXPECT_EQ(slot->info(), std::string("mouse (AppleMouse II card)"));
    auto* card = find<apple2::MouseCard>(m);
    ASSERT(card);
    m->onReset(*m);
    run(m, 500000);
    IBus* bus = m->buses[0].bus;
    // The firmware's identification bytes
    EXPECT_EQ(bus->peek8(0xC405), 0x38);
    EXPECT_EQ(bus->peek8(0xC407), 0x18);
    EXPECT_EQ(bus->peek8(0xC40B), 0x01);
    EXPECT_EQ(bus->peek8(0xC40C), 0x20);
    EXPECT_EQ(bus->peek8(0xC4FB), 0xD6);
    // Calls into the firmware from $0300, then a loop at the end
    auto call = [&](std::vector<std::pair<uint8_t, int>> calls) {
        std::vector<uint8_t> p = {0x78};                                    // SEI
        for (auto& [entry, a] : calls) {
            if (a >= 0) { p.push_back(0xA9); p.push_back((uint8_t)a); }     // LDA #a
            const uint8_t lo = bus->peek8(0xC400u + entry);
            for (uint8_t b : {0xA2, 0xC4, 0xA0, 0x40, 0x20, (int)lo, 0xC4}) p.push_back((uint8_t)b);
            for (uint8_t b : {0x08, 0x68, 0x85, 0x06}) p.push_back(b);     // PHP; PLA; STA $06 (C from the call)
        }
        const uint16_t end = (uint16_t)(0x0300 + p.size());
        for (uint8_t b : {0x4C, (int)(end & 0xFF), (int)(end >> 8)}) p.push_back((uint8_t)b);
        for (size_t i = 0; i < p.size(); ++i) bus->write8(0x0300 + (uint32_t)i, p[i]);
        m->cpus[0].cpu->setPc(0x0300);
        run(m, 30000);
        EXPECT_EQ(m->cpus[0].cpu->pc(), end);
    };
    const uint8_t SET = 0x12, SERVE = 0x13, READ = 0x14, CLAMP = 0x17, INIT = 0x19;
    call({{INIT, -1}, {SET, 0x01}});                                        // on, no interrupts
    EXPECT_EQ(card->mode(), 0x01);
    m->onMouse(40, 20, 1, 0);                                               // +20, +10; button 0 down
    call({{READ, -1}});
    EXPECT_EQ(bus->peek8(0x047C), 20);                                      // X low
    EXPECT_EQ(bus->peek8(0x057C), 0);                                       // X high
    EXPECT_EQ(bus->peek8(0x04FC), 10);                                      // Y low
    EXPECT_EQ(bus->peek8(0x077C) & 0xA0, 0xA0);                             // button 0 down, moved
    // CLAMPMOUSE X to 0-15 (the bounds in $0478/$04F8/$0578/$05F8)
    bus->write8(0x0478, 0); bus->write8(0x0578, 0);
    bus->write8(0x04F8, 15); bus->write8(0x05F8, 0);
    call({{CLAMP, 0x00}});
    m->onMouse(200, 0, 0, 0);
    call({{READ, -1}});
    EXPECT_EQ(bus->peek8(0x047C), 15);
    EXPECT_EQ(bus->peek8(0x077C) & 0x80, 0x00);                             // button 0 up
    // The VBL interrupt (mode $08), and SERVEMOUSE: carry clear, VBL the cause
    EXPECT_TRUE(!card->irq());                                              // none asked for
    call({{SET, 0x08}});                                                    // runs more than a frame
    EXPECT_TRUE(card->irq());
    // SERVEMOUSE, then nothing for the rest of the frame
    std::vector<uint8_t> serve = {0x78, 0xA2, 0xC4, 0xA0, 0x40, 0x20, bus->peek8(0xC400u + SERVE), 0xC4,
                                  0x08, 0x68, 0x85, 0x06, 0x4C, 0x0C, 0x03};
    for (size_t i = 0; i < serve.size(); ++i) bus->write8(0x0300 + (uint32_t)i, serve[i]);
    m->cpus[0].cpu->setPc(0x0300);
    run(m, 2000);
    EXPECT_EQ(m->cpus[0].cpu->pc(), 0x030C);
    EXPECT_TRUE(!card->irq());
    EXPECT_EQ(bus->peek8(0x06) & 0x01, 0x00);                               // C clear: the card's interrupt
    EXPECT_EQ(bus->peek8(0x077C) & 0x08, 0x08);                             // VBL
    ASSERT(slot->install("", "", err));
    EXPECT_TRUE(find<apple2::MouseCard>(m) == nullptr);
    delete m;
}

// A No-Slot Clock under the IIe's internal ROM ($C3xx with INTCXROM on) and
// the II+'s $F8 ROM: a read with A2 set starts the pattern over; 64
// accesses with A2 clear give $5CA33AC55CA33AC5 a bit at a time on A0; 64
// reads with A2 set then give the time in BCD, low bit first. Without the
// pattern, or without the clock, the ROM reads as usual.
#include "plugins/machines/apple2/main/no_slot_clock.h"
TEST_CASE(apple2_no_slot_clock) {
    for (apple2::Model model : {apple2::Model::IIeEnhanced, apple2::Model::IIPlus}) {
        std::string err;
        MachineDescriptor* m = apple2::create(model, &err);
        if (!m) { printf("  (skipped: %s)\n", err.c_str()); continue; }
        const bool iie = model == apple2::Model::IIeEnhanced;
        auto* slot = m->expansionSlot("clock");
        ASSERT(slot != nullptr);
        EXPECT_TRUE(!slot->install("nsc", "x", err));
        ASSERT(slot->install("nsc", "", err));
        EXPECT_EQ(slot->info(), std::string("nsc (No-Slot Clock)"));
        m->onReset(*m);
        run(m, 100000);
        IBus* bus = m->buses[0].bus;
        if (iie) bus->write8(0xC007, 0);                 // INTCXROM: the internal ROM at $C100-$CFFF
        else bus->read8(0xC082);                         // the II+: the ROM read, the language card write-protected
                                                         // (AppleWin's IO_F8xx: not while it's writable)
        const uint16_t base = iie ? 0xC300 : 0xF800;
        const uint8_t romByte = bus->peek8(base + 4);
        auto readTime = [&]() {
            bus->read8(base + 4);                        // start the pattern over
            for (int i = 0; i < 64; ++i) bus->read8((uint16_t)(base + ((apple2::NoSlotClock::INIT_SEQUENCE >> i) & 1)));
            uint64_t bits = 0;
            for (int i = 0; i < 64; ++i) bits |= (uint64_t)(bus->read8(base + 4) & 1) << i;
            return bits;
        };
        // Tuesday 9 November 2026, 21:45:07
        std::tm t{};
        t.tm_year = 126; t.tm_mon = 10; t.tm_mday = 9; t.tm_wday = 2;
        t.tm_hour = 21; t.tm_min = 45; t.tm_sec = 7;
        apple2::NoSlotClock* nsc = apple2::noSlotClockOf(m);
        ASSERT(nsc);
        nsc->now = [t]() { return t; };
        nsc->hundredths = []() { return 0; };
        // Hundredths, seconds, minutes, hours, weekday (1 = Sunday), date,
        // month, year: BCD bytes from the low end
        EXPECT_EQ(readTime(), 0x2611090321450700ull);
        if (iie) {
            // NS.CLOCK.SYSTEM's own clock routine (its $13F5-$1470, from the
            // disassembly in AppleWin's docs/NoSlotClock) on the 65C02, through
            // the internal $C3 ROM (SLOTC3ROM off, INTCXROM off): ProDOS's
            // date and time at $BF90-$BF93
            static const uint8_t DRIVER[] = {
                0x08, 0x78, 0xAD, 0xFF, 0xCF, 0x48, 0x8D, 0x00, 0xC3, 0xAD, 0x04, 0xC3, 0xA2, 0x08, 0xBD, 0x68, 0x14, 0x38, 0x6A, 0x48,
                0xA9, 0x00, 0x2A, 0xA8, 0xB9, 0x00, 0xC3, 0x68, 0x4A, 0xD0, 0xF4, 0xCA, 0xD0, 0xEC, 0xA2, 0x08, 0xA0, 0x08, 0xAD, 0x04,
                0xC3, 0x6A, 0x7E, 0xFF, 0x01, 0x88, 0xD0, 0xF6, 0xBD, 0xFF, 0x01, 0x4A, 0x4A, 0x4A, 0x4A, 0xA8, 0xF0, 0x0E, 0xBD, 0xFF,
                0x01, 0x29, 0x0F, 0x18, 0x69, 0x0A, 0x88, 0xD0, 0xFB, 0x9D, 0xFF, 0x01, 0xCA, 0xD0, 0xD9, 0xAD, 0x04, 0x02, 0x8D, 0x93,
                0xBF, 0xAD, 0x05, 0x02, 0x8D, 0x92, 0xBF, 0xAD, 0x01, 0x02, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0D, 0x02, 0x02, 0x8D, 0x90,
                0xBF, 0xAD, 0x00, 0x02, 0x2A, 0x8D, 0x91, 0xBF, 0x68, 0x30, 0x03, 0x8D, 0xFF, 0xCF, 0x28, 0x60, 0x5C, 0xA3, 0x3A, 0xC5,
                0x5C, 0xA3, 0x3A, 0xC5};
            for (size_t i = 0; i < sizeof DRIVER; ++i) bus->write8(0x13F5 + (uint32_t)i, DRIVER[i]);
            static const uint8_t CALL[] = {0x20, 0xF5, 0x13, 0x4C, 0x03, 0x03};    // JSR $13F5; JMP *
            for (size_t i = 0; i < sizeof CALL; ++i) bus->write8(0x0300 + (uint32_t)i, CALL[i]);
            for (uint32_t a = 0xBF90; a < 0xBF94; ++a) bus->write8(a, 0);
            bus->write8(0xC006, 0);                      // INTCXROM off: $C3 is internal by SLOTC3ROM alone
            m->cpus[0].cpu->setPc(0x0300);
            run(m, 20000);
            EXPECT_EQ(m->cpus[0].cpu->pc(), 0x0303);
            EXPECT_EQ(bus->peek8(0xBF91), 0x35);         // year 26, month's bit 3
            EXPECT_EQ(bus->peek8(0xBF90), 0x69);         // month 11's low bits, date 9
            EXPECT_EQ(bus->peek8(0xBF93), 21);           // hour
            EXPECT_EQ(bus->peek8(0xBF92), 45);           // minute
            bus->write8(0xC007, 0);
        }
        // With the pattern broken (one wrong bit) the ROM answers
        bus->read8(base + 4);
        bus->read8((uint16_t)(base + 0));                // the pattern's first bit is 1
        EXPECT_EQ(bus->read8(base + 4), romByte);
        EXPECT_TRUE(!nsc->open());
        // Taken out: the ROM again after the pattern
        ASSERT(slot->install("", "", err));
        EXPECT_EQ(slot->info(), std::string());
        bus->read8(base + 4);
        for (int i = 0; i < 64; ++i) bus->read8((uint16_t)(base + ((apple2::NoSlotClock::INIT_SEQUENCE >> i) & 1)));
        EXPECT_EQ(bus->read8(base + 4), romByte);
        delete m;
    }
}
