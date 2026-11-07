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
