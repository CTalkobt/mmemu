#include "test_harness.h"
#include "plugins/machines/apple2/main/machine_apple2.h"
#include "libcore/main/machine_desc.h"
#include "libdevices/main/io_registry.h"
#include "libdevices/main/ikeyboard_matrix.h"
#include "libdevices/main/iaudio_output.h"
#include "libmem/main/ibus.h"
#include <cstdio>
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
    EXPECT_TRUE(hi - lo > 0.2f);
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
