#include "test_harness.h"
#include "plugins/devices/i8155/main/i8155.h"

namespace {
// Start the timer: count n, mode m
void startTimer(I8155& c, uint16_t n, uint8_t mode, uint8_t portBits = 0) {
    c.writeIo(4, (uint8_t)n);
    c.writeIo(5, (uint8_t)((n >> 8) | (mode << 6)));
    c.writeIo(0, (uint8_t)(0xC0 | portBits));
}
}  // namespace

TEST_CASE(i8155_ram_and_ports) {
    I8155 c;
    c.writeRam(0x00, 0x12);
    c.writeRam(0xFF, 0x34);
    ASSERT_EQ(c.readRam(0x00), 0x12);
    ASSERT_EQ(c.readRam(0xFF), 0x34);
    int aWrites = 0;
    uint8_t aLast = 0, cLast = 0;
    c.outPa = [&](uint8_t v) { ++aWrites; aLast = v; };
    c.outPc = [&](uint8_t v) { cLast = v; };
    c.inPb = []() -> uint8_t { return 0x5A; };
    c.inPc = []() -> uint8_t { return 0x15; };
    // Inputs after reset: a write to port A only loads the latch
    c.writeIo(1, 0x77);
    ASSERT_EQ(aWrites, 0);
    ASSERT_EQ(c.readIo(2), 0x5A);
    ASSERT_EQ(c.readIo(3), 0xD5);          // 6 bits, 7-6 read 1
    // PA output: the latch appears at once
    c.writeIo(0, 0x01);
    ASSERT_EQ(aWrites, 1);
    ASSERT_EQ(aLast, 0x77);
    c.writeIo(1, 0x3C);
    ASSERT_EQ(aLast, 0x3C);
    ASSERT_EQ(c.readIo(1), 0x3C);          // an output reads its latch
    // Port C ALT2: all outputs
    c.writeIo(0, 0x0D);
    c.writeIo(3, 0xFF);
    ASSERT_EQ(cLast, 0x3F);
    ASSERT_EQ(c.readIo(3), 0xFF);
    ASSERT(c.pcMode() == I8155::PcMode::Alt2);
    c.reset();
    ASSERT_EQ(c.command(), 0);
    ASSERT_EQ(c.readIo(2), 0x5A);          // back to input
    ASSERT_EQ(c.readRam(0xFF), 0x34);      // RAM kept
}

// Continuous square wave: high for ceil(n/2), low for floor(n/2); TC sets the flag
TEST_CASE(i8155_timer_square_wave) {
    I8155 c;
    std::vector<bool> levels;
    c.onTimerOut = [&](bool v) { levels.push_back(v); };
    startTimer(c, 10, 1);
    ASSERT(c.timerRunning());
    ASSERT(c.timerOut());
    c.tick(4);
    ASSERT(c.timerOut());
    c.tick(1);                             // 5 clocks: the low half
    ASSERT(!c.timerOut());
    ASSERT(!c.timerFlag());
    c.tick(4);
    ASSERT(!c.timerOut());
    ASSERT_EQ(c.tick(1), 1u);              // the period ends: high, one rising edge
    ASSERT(c.timerOut());
    ASSERT(c.timerFlag());
    ASSERT_EQ(c.readIo(0) & 0x40, 0x40);
    ASSERT_EQ(c.readIo(0) & 0x40, 0);      // reading the status cleared it
    // Rising edges over many periods: one per period
    ASSERT_EQ(c.tick(1000), 100u);
    // Odd counts: the longer half first
    I8155 d;
    startTimer(d, 5, 1);
    d.tick(2);
    ASSERT(d.timerOut());
    d.tick(1);
    ASSERT(!d.timerOut());
    d.tick(2);
    ASSERT(d.timerOut());
}

// Single square wave and the pulse modes; stop and stop-after-TC
TEST_CASE(i8155_timer_modes) {
    I8155 c;
    startTimer(c, 6, 0);                   // single square wave
    c.tick(6);
    ASSERT(!c.timerRunning());
    ASSERT(c.timerOut());
    ASSERT(c.timerFlag());
    c.tick(100);
    ASSERT(c.timerOut());
    // Continuous pulse: low only in the last clock of each period
    I8155 p;
    int lows = 0;
    p.onTimerOut = [&](bool v) { if (!v) ++lows; };
    startTimer(p, 4, 3);
    p.tick(3);
    ASSERT(!p.timerOut());
    p.tick(1);
    ASSERT(p.timerOut());
    p.tick(8);
    ASSERT_EQ(lows, 3);
    // Single pulse stops after one
    I8155 s;
    startTimer(s, 4, 2);
    s.tick(10);
    ASSERT(!s.timerRunning());
    // Stop now
    I8155 t;
    startTimer(t, 10, 1);
    t.tick(7);
    ASSERT(!t.timerOut());
    t.writeIo(0, 0x40);
    ASSERT(!t.timerRunning());
    ASSERT(t.timerOut());                  // stopping drives it high
    // Stop after TC
    startTimer(t, 10, 1);
    t.tick(3);
    t.writeIo(0, 0x80);
    ASSERT(t.timerRunning());
    t.tick(7);
    ASSERT(!t.timerRunning());
    ASSERT(t.timerFlag());
    // Too small a count doesn't run
    startTimer(t, 1, 1);
    ASSERT(!t.timerRunning());
}

// A start while running takes the new count at the next TC
TEST_CASE(i8155_timer_reload) {
    I8155 c;
    startTimer(c, 10, 1);
    c.tick(2);
    startTimer(c, 4, 1);
    ASSERT_EQ(c.loadedCount(), 10);
    c.tick(8);                             // TC of the first period
    ASSERT_EQ(c.loadedCount(), 4);
    ASSERT(c.timerOut());
    c.tick(2);
    ASSERT(!c.timerOut());
    // Readback (MAME's): count left in the half x 2, bit 0 set in the first half
    I8155 r;
    startTimer(r, 100, 1);
    ASSERT_EQ(r.peekIo(4) | ((r.peekIo(5) & 0x3F) << 8), 101);
    ASSERT_EQ(r.peekIo(5) >> 6, 1);
    r.tick(10);
    ASSERT_EQ(r.timerReadback(), (40 * 2) | 1);
    r.tick(50);
    ASSERT_EQ(r.timerReadback(), 80);
    r.writeIo(0, 0x40);
    ASSERT_EQ(r.timerReadback(), 100);     // stopped: the register
}

// The Model 100's baud clock: 2.4576 MHz / 8 = 307200 Hz = 16 x 19200
TEST_CASE(i8155_m100_baud_clock) {
    I8155 c;
    startTimer(c, 8, 1);
    ASSERT_EQ(c.tick(2457600), 307200u);
}

// ALT3: port A strobed input; ALT4 adds port B, here a strobed output
TEST_CASE(i8155_strobed_modes) {
    I8155 c;
    uint8_t pins = 0x11;
    c.inPa = [&]() { return pins; };
    int intrA = 0;
    c.onIntrA = [&](bool v) { if (v) ++intrA; };
    c.writeIo(0, 0x14);                    // ALT3, IEA, port A input
    ASSERT(c.pcMode() == I8155::PcMode::Alt3);
    c.setStrobeA(false);                   // latch
    pins = 0x99;                           // the pins move on; the latch keeps $11
    ASSERT_EQ(c.status() & 0x02, 0x02);    // A BF
    c.setStrobeA(true);
    ASSERT_EQ(intrA, 1);
    ASSERT_EQ(c.status() & 0x05, 0x05);    // INTR A, INTE A
    ASSERT_EQ(c.readIo(3) & 0x07, 0x07);   // PC0 INTR, PC1 BF, PC2 /STB high
    ASSERT_EQ(c.readIo(1), 0x11);
    ASSERT_EQ(c.status() & 0x03, 0);       // read: BF and INTR cleared
    // ALT4, port B strobed output with IEB
    c.writeIo(0, 0x2A);
    c.writeIo(2, 0x42);
    ASSERT_EQ(c.status() & 0x10, 0x10);    // B BF: full
    c.setStrobeB(false);                   // the peripheral takes it
    ASSERT_EQ(c.status() & 0x10, 0);
    c.setStrobeB(true);
    ASSERT_EQ(c.status() & 0x28, 0x28);    // INTR B, INTE B
    c.writeIo(2, 0x43);
    ASSERT_EQ(c.status() & 0x08, 0);       // a write clears INTR
}

TEST_CASE(i8155_save_state) {
    I8155 a;
    a.writeRam(5, 0x99);
    a.writeIo(0, 0x03);
    a.writeIo(1, 0x12);
    startTimer(a, 20, 1, 0x03);
    a.tick(13);
    I8155 b;
    b.loadState(a.saveState());
    ASSERT_EQ(b.readRam(5), 0x99);
    ASSERT_EQ(b.outputA(), 0x12);
    ASSERT(b.timerRunning());
    ASSERT_EQ(b.timerOut(), a.timerOut());
    ASSERT_EQ(b.tick(7), a.tick(7));
    ASSERT_EQ(b.timerOut(), a.timerOut());
}
