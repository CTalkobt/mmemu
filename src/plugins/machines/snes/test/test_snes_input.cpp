// The SNES controller ports (as Mesen): the auto joypad read's timing, the mouse, the multitap and the
// Super Scope.

#include "test_harness.h"
#include "plugins/machines/snes/main/machine_snes.h"
#include "libcore/main/machine_desc.h"
#include "libmem/main/ibus.h"
#include <cstdio>
#include <string>
#include <vector>

namespace {

/** A cartridge that sits in a loop; the tests drive the bus themselves. */
MachineDescriptor* idle() {
    std::vector<uint8_t> img(0x8000, 0);
    const char* title = "MMSIM INPUT          ";
    for (int i = 0; i < 21; ++i) img[0x7FC0 + i] = (uint8_t)title[i];
    img[0x7FD5] = 0x20; img[0x7FD7] = 0x05;
    img[0x7FDC] = 0xFF; img[0x7FDD] = 0xFF;
    img[0x7FFC] = 0x00; img[0x7FFD] = 0x80;
    img[0] = 0x80; img[1] = 0xFE;
    std::string err;
    MachineDescriptor* m = snes::create(false, &err);
    if (!snes::insertCartridge(m, img, "in.sfc", err)) { delete m; return nullptr; }
    return m;
}
void pass(IBus* bus) { bus->read8(0x004300); }      // 6 clocks
/** Run to the start of line `line`. */
void toLine(MachineDescriptor* m, int line) {
    IBus* bus = m->buses[0].bus;
    while (snes::scanline(m) == line) pass(bus);
    while (snes::scanline(m) != line) pass(bus);
}
/** Run until the auto read is over (to line 230). */
void autoRead(MachineDescriptor* m) { toLine(m, 230); }
uint16_t word(IBus* bus, uint32_t a) { return (uint16_t)(bus->read8(a) | bus->read8(a + 1) << 8); }

}  // namespace

// From the first multiple of 256 clocks after clock 130 of the vblank line, less 128: the strobe for 256
// clocks, $4212 bit 0 from 128 clocks in for 33 steps of 128
TEST_CASE(snes_auto_joypad_timing) {
    MachineDescriptor* m = idle();
    ASSERT(m);
    IBus* bus = m->buses[0].bus;
    bus->write8(0x004200, 0x01);
    toLine(m, 225);
    const uint64_t line = snes::masterClock(m) - (uint64_t)snes::hclock(m);
    const uint64_t from = line + 130;
    const uint64_t start = from + ((from & 0xFF) ? 256 - (from & 0xFF) : 0) - 128;
    uint64_t first = 0, last = 0;
    while (snes::scanline(m) < 230) {
        const uint64_t t = snes::masterClock(m);
        if (bus->read8(0x004212) & 1) { if (!first) first = t; last = t; }
    }
    // The read itself is 8 clocks into the cycle: within a cycle (6-12 clocks) of the edges
    EXPECT_TRUE(first + 12 >= start + 128 && first <= start + 128);
    EXPECT_TRUE(last + 12 >= start + 34 * 128 && last <= start + 34 * 128);
    if (!(first + 12 >= start + 128 && first <= start + 128)) std::printf("  first %llu want %llu\n", (unsigned long long)(first - line), (unsigned long long)(start + 128 - line));
    if (!(last + 12 >= start + 34 * 128 && last <= start + 34 * 128)) std::printf("  last %llu want %llu\n", (unsigned long long)(last - line), (unsigned long long)(start + 34 * 128 - line));
    delete m;
}

// The multitap in port 2: $4201 bit 7 picks pads 1-2 (D0, D1) or 3-4
TEST_CASE(snes_multitap) {
    MachineDescriptor* m = idle();
    ASSERT(m);
    ASSERT(snes::setPortDevice(m, 1, "multitap"));
    EXPECT_EQ(snes::portDevice(m, 1), std::string("multitap"));
    IBus* bus = m->buses[0].bus;
    snes::setButtons(m, 0, snes::BTN_A);                         // player 1: port 1's pad
    snes::setButtons(m, 1, snes::BTN_B);                         // players 2-5: the multitap
    snes::setButtons(m, 2, snes::BTN_X);
    snes::setButtons(m, 3, snes::BTN_START);
    snes::setButtons(m, 4, snes::BTN_UP);
    bus->write8(0x004201, 0xFF);
    bus->write8(0x004200, 0x01);
    autoRead(m);
    EXPECT_EQ(word(bus, 0x004218), snes::BTN_A);
    EXPECT_EQ(word(bus, 0x00421A), snes::BTN_B);
    EXPECT_EQ(word(bus, 0x00421E), snes::BTN_X);
    EXPECT_EQ(word(bus, 0x00421C), 0);                           // port 1's D1: nothing
    bus->write8(0x004201, 0x7F);
    autoRead(m);
    EXPECT_EQ(word(bus, 0x00421A), snes::BTN_START);
    EXPECT_EQ(word(bus, 0x00421E), snes::BTN_UP);
    // Strobed, it answers D1 high and D0 low
    bus->write8(0x004016, 1);
    EXPECT_EQ(bus->read8(0x004017) & 3, 2);
    bus->write8(0x004016, 0);
    delete m;
}

// The mouse in port 1: the auto read has its first 16 bits, $4016 the Y and X bytes; reading it strobed
// steps the sensitivity
TEST_CASE(snes_mouse) {
    MachineDescriptor* m = idle();
    ASSERT(m);
    ASSERT(snes::setPortDevice(m, 0, "mouse"));
    ASSERT(m->onMouse);
    IBus* bus = m->buses[0].bus;
    bus->write8(0x004200, 0x01);
    toLine(m, 10);                                               // (past the power-on strobe)
    m->onMouse(5, -3, 1, 0);                                     // right 5, up 3, left button
    autoRead(m);
    EXPECT_EQ(word(bus, 0x004218), 0x0041);                      // $4219 0, $4218: left button, signature
    uint8_t y = 0, x = 0;
    for (int i = 0; i < 8; ++i) y = (uint8_t)(y << 1 | (bus->read8(0x004016) & 1));
    for (int i = 0; i < 8; ++i) x = (uint8_t)(x << 1 | (bus->read8(0x004016) & 1));
    EXPECT_EQ(y, 0x83);                                          // up, 3
    EXPECT_EQ(x, 0x05);                                          // right, 5
    bus->write8(0x004200, 0x00);
    bus->write8(0x004016, 1); bus->read8(0x004016); bus->write8(0x004016, 0);
    uint16_t w = 0;
    for (int i = 0; i < 16; ++i) w = (uint16_t)(w << 1 | (bus->read8(0x004016) & 1));
    EXPECT_EQ((w >> 4) & 3, 1);                                  // sensitivity 1
    delete m;
}

// The Super Scope in port 2: aimed with a button held, the PPU latches H/V where it points once the
// picture gets there (Mesen: 10 dots right, 3 lines up); the buttons in $421A/$421B
TEST_CASE(snes_super_scope) {
    MachineDescriptor* m = idle();
    ASSERT(m);
    ASSERT(snes::setPortDevice(m, 1, "superscope"));
    IBus* bus = m->buses[0].bus;
    bus->write8(0x004200, 0x01);
    toLine(m, 10);
    m->onMouse(100 - 128, 60 - 112, 1, 0);                       // to (100, 60), fire held
    autoRead(m);                                                 // aimed at vblank; read
    EXPECT_EQ(bus->read8(0x00421B), 0x80);                       // fire
    EXPECT_EQ(bus->read8(0x00421A), 0xFF);
    toLine(m, 40);                                               // the next frame, before (110, 57)
    bus->read8(0x00213F);
    EXPECT_EQ(bus->read8(0x00213F) & 0x40, 0);
    toLine(m, 58);                                               // after it
    EXPECT_EQ(bus->read8(0x00213F) & 0x40, 0x40);
    const uint8_t hl = bus->read8(0x00213C), hh = bus->read8(0x00213C);
    const uint8_t vl = bus->read8(0x00213D), vh = bus->read8(0x00213D);
    EXPECT_EQ((hl | (hh & 1) << 8), 110);
    EXPECT_EQ((vl | (vh & 1) << 8), 57);
    delete m;
}
