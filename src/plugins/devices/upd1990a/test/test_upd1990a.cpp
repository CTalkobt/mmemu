#include "test_harness.h"
#include "plugins/devices/upd1990a/main/upd1990a.h"

namespace {
void command(Upd1990a& r, int c) {
    r.setC((uint8_t)c);
    r.setStb(true);
    r.setStb(false);
}
// Shifts `bytes` in LSB first (register shift mode), as the Model 100 ROM does
void shiftIn(Upd1990a& r, const std::vector<uint8_t>& bytes) {
    command(r, 1);
    for (uint8_t b : bytes)
        for (int i = 0; i < 8; ++i) {
            r.setDataIn((b >> i) & 1);
            r.setClk(true);
            r.setClk(false);
        }
}
// Reads n bytes out: DATA OUT is bit 0; each CLK shifts the next one down
std::vector<uint8_t> shiftOut(Upd1990a& r, int n) {
    command(r, 1);
    std::vector<uint8_t> v;
    for (int k = 0; k < n; ++k) {
        uint8_t b = 0;
        for (int i = 0; i < 8; ++i) {
            if (r.dataOut()) b |= (uint8_t)(1 << i);
            r.setDataIn(false);
            r.setClk(true);
            r.setClk(false);
        }
        v.push_back(b);
    }
    return v;
}
}  // namespace

// Time set from the shift register, then time read and shift out
TEST_CASE(upd1990a_set_and_read) {
    Upd1990a r;
    r.hostClock = []() { return (std::time_t)1700000000; };
    // 12:34:56, the 25th, Thursday (4), November
    shiftIn(r, {0x56, 0x34, 0x12, 0x25, 0xB4});
    command(r, 2);
    ASSERT(r.held());
    ASSERT_EQ(r.counter()[0], 0x56);
    ASSERT_EQ(r.counter()[4], 0xB4);
    command(r, 3);                         // time read (releases the hold)
    ASSERT(!r.held());
    const auto v = shiftOut(r, 5);
    ASSERT_EQ(v[0], 0x56);
    ASSERT_EQ(v[1], 0x34);
    ASSERT_EQ(v[2], 0x12);
    ASSERT_EQ(v[3], 0x25);
    ASSERT_EQ(v[4], 0xB4);
    // CS low: STB is ignored
    r.setCs(false);
    command(r, 2);
    ASSERT_EQ(r.command(), 1);
    // OE low: DATA OUT floats high
    r.setCs(true);
    r.setOe(false);
    ASSERT(r.dataOut());
}

// The counter runs on emulated time: a second per 32768 crystal ticks, with carries
TEST_CASE(upd1990a_counting) {
    Upd1990a r(Upd1990a::Variant::Upd4990a);
    // 2023-12-31 23:59:59, Sunday (0)
    r.setCounter({0x59, 0x59, 0x23, 0x31, 0xC0, 0x23});
    r.tickCrystal(32767);
    ASSERT_EQ(r.counter()[0], 0x59);
    r.tickCrystal(1);
    const auto c = r.counter();
    ASSERT_EQ(c[0], 0x00);
    ASSERT_EQ(c[1], 0x00);
    ASSERT_EQ(c[2], 0x00);
    ASSERT_EQ(c[3], 0x01);
    ASSERT_EQ(c[4], 0x11);                 // January, Monday
    ASSERT_EQ(c[5], 0x24);
    // Leap year: 2024-02-28 -> 29
    r.setCounter({0x59, 0x59, 0x23, 0x28, 0x23, 0x24});
    r.tickCrystal(32768);
    ASSERT_EQ(r.counter()[3], 0x29);
    ASSERT_EQ(r.counter()[4] >> 4, 2);
    r.setCounter({0x59, 0x59, 0x23, 0x28, 0x23, 0x23});
    r.tickCrystal(32768);
    ASSERT_EQ(r.counter()[3], 0x01);
    ASSERT_EQ(r.counter()[4] >> 4, 3);
    // tick() at the CPU clock: 2.4576 MHz cycles
    r.setCounter({0x00, 0x00, 0x00, 0x01, 0x10, 0x00});
    r.tick(2457600);
    ASSERT_EQ(r.counter()[0], 0x01);
}

// Time set holds the counter; the next second comes 31/32 s after the release
TEST_CASE(upd1990a_hold) {
    Upd1990a r;
    r.hostClock = []() { return (std::time_t)1700000000; };
    shiftIn(r, {0x00, 0x00, 0x10, 0x01, 0x10});
    command(r, 2);
    r.tickCrystal(5 * 32768);
    ASSERT_EQ(r.counter()[0], 0x00);       // held
    command(r, 0);                         // register hold releases it
    r.tickCrystal(31 * 1024 - 1);
    ASSERT_EQ(r.counter()[0], 0x00);
    r.tickCrystal(1);
    ASSERT_EQ(r.counter()[0], 0x01);
    // Register hold: DATA OUT is a 1 Hz square wave
    bool first = r.dataOut();
    r.tickCrystal(16384);
    ASSERT(r.dataOut() != first);
}

// TP: 64 / 256 / 2048 Hz
TEST_CASE(upd1990a_tp) {
    Upd1990a r;
    int rises = 0;
    r.onTp = [&](bool v) { if (v) ++rises; };
    const int cmds[3] = {4, 5, 6};
    const int hz[3] = {64, 256, 2048};
    for (int i = 0; i < 3; ++i) {
        command(r, cmds[i]);
        rises = 0;
        r.tickCrystal(32768);
        ASSERT(rises >= hz[i] - 1 && rises <= hz[i]);
    }
    // Test mode: the counter runs at 1024 seconds a second
    r.setCounter({0x00, 0x00, 0x00, 0x01, 0x10, 0x00});
    command(r, 7);
    ASSERT(r.testMode());
    r.tickCrystal(32 * 60);
    ASSERT_EQ(r.counter()[1], 0x01);
    command(r, 0);
    ASSERT(!r.testMode());
}

// The offset: a time set moves it so syncToHost keeps the program's time
TEST_CASE(upd1990a_host_offset) {
    std::time_t now = 1700000000;
    Upd1990a r;
    r.hostClock = [&]() { return now; };
    r.syncToHost();
    std::tm tm{};
    localtime_r(&now, &tm);
    ASSERT_EQ(r.counter()[1], (uint8_t)(((tm.tm_min / 10) << 4) | (tm.tm_min % 10)));
    ASSERT_EQ(r.offsetSeconds(), 0);
    // Set the clock one hour ahead of the host
    std::time_t ahead = now + 3600;
    std::tm a{};
    localtime_r(&ahead, &a);
    auto bcd = [](int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); };
    shiftIn(r, {bcd(a.tm_sec), bcd(a.tm_min), bcd(a.tm_hour), bcd(a.tm_mday),
                (uint8_t)(((a.tm_mon + 1) << 4) | a.tm_wday)});
    command(r, 2);
    ASSERT(r.offsetSeconds() >= 3599 && r.offsetSeconds() <= 3601);
    now += 100;
    r.syncToHost();
    std::time_t expect = now + 3600;
    std::tm e{};
    localtime_r(&expect, &e);
    ASSERT_EQ(r.counter()[2], bcd(e.tm_hour));
}

// uPD4990A serial command mode: C0-C2 high, the command shifted in on DATA IN
TEST_CASE(upd4990a_serial_commands) {
    Upd1990a r(Upd1990a::Variant::Upd4990a);
    auto serial = [&](int cmd) {
        r.setC(7);
        for (int i = 0; i < 4; ++i) {
            r.setDataIn((cmd >> i) & 1);
            r.setClk(true);
            r.setClk(false);
        }
        r.setStb(true);
        r.setStb(false);
    };
    int rises = 0;
    r.onTp = [&](bool v) { if (v) ++rises; };
    serial(0x7);                           // 4096 Hz
    ASSERT_EQ(r.command(), 7);
    r.tickCrystal(32768);
    ASSERT(rises >= 4095);
    serial(0x9);                           // 10 s interval
    rises = 0;
    r.tickCrystal(32768 * 30);
    ASSERT_EQ(rises, 3);
    serial(0xE);                           // interval stop
    rises = 0;
    r.tickCrystal(32768 * 30);
    ASSERT_EQ(rises, 0);
    Upd1990a s(Upd1990a::Variant::Upd4990a);
    s.loadState(r.saveState());
    ASSERT_EQ(s.counter(), r.counter());
    ASSERT_EQ(s.command(), r.command());
}
