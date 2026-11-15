#include "test_harness.h"
#include "plugins/devices/rp5c01/main/rp5c01.h"

namespace {
// Writes the time counter (block 0): yy-mm-dd wday hh:mm:ss, 24-hour
void setTime(Rp5c01& r, int yy, int mo, int dd, int wd, int hh, int mi, int ss) {
    r.write(13, 0x00);                     // block 0, timer stopped
    const int v[13] = {ss % 10, ss / 10, mi % 10, mi / 10, hh % 10, hh / 10, wd,
                       dd % 10, dd / 10, mo % 10, mo / 10, yy % 10, yy / 10};
    for (int i = 0; i < 13; ++i) r.write(i, (uint8_t)v[i]);
}
}  // namespace

TEST_CASE(rp5c01_registers_and_blocks) {
    Rp5c01 r;
    r.hostClock = []() { return (std::time_t)1700000000; };
    setTime(r, 24, 2, 28, 3, 23, 59, 59);
    ASSERT_EQ(r.read(0), 9);
    ASSERT_EQ(r.read(5), 2);
    ASSERT_EQ(r.read(12), 2);
    // Writes are masked: 10-seconds keeps 3 bits
    r.write(1, 0xF);
    ASSERT_EQ(r.read(1), 7);
    r.write(1, 5);
    // Block 1: the 12/24 select and the leap-year counter
    r.write(13, 0x01);
    ASSERT_EQ(r.read(10), 1);              // 24-hour after power-on
    r.write(11, 0);                        // a leap year
    // Blocks 2 and 3: the RAM's low and high nibbles
    r.write(13, 0x02);
    r.write(4, 0x5);
    r.write(13, 0x03);
    r.write(4, 0xA);
    ASSERT_EQ(r.ram(4), 0xA5);
    ASSERT_EQ(r.read(4), 0xA);
    r.write(13, 0x02);
    ASSERT_EQ(r.read(4), 0x5);
    ASSERT_EQ(r.read(13), 0x02);
    ASSERT_EQ(r.read(14), 0);              // write only
}

// The counter runs only with the timer enable bit; carries through the leap day
TEST_CASE(rp5c01_counting) {
    Rp5c01 r;
    r.hostClock = []() { return (std::time_t)1700000000; };
    setTime(r, 24, 2, 28, 3, 23, 59, 59);
    r.write(13, 0x01);
    r.write(11, 0);                        // leap counter: 2024 is a leap year
    r.write(13, 0x00);                     // timer off
    r.tickCrystal(32768 * 2);
    ASSERT_EQ(r.read(0), 9);
    r.write(13, 0x08);                     // timer on
    r.tickCrystal(32768);
    ASSERT_EQ(r.read(0), 0);
    ASSERT_EQ(r.read(4), 0);
    ASSERT_EQ(r.read(5), 0);
    ASSERT_EQ(r.read(7), 9);               // the 29th
    ASSERT_EQ(r.read(8), 2);
    ASSERT_EQ(r.read(6), 4);               // the weekday steps
    // Not a leap year: Feb 28 -> Mar 1
    setTime(r, 23, 2, 28, 2, 23, 59, 59);
    r.write(13, 0x01);
    r.write(11, 3);
    r.write(13, 0x08);
    r.tickCrystal(32768);
    ASSERT_EQ(r.read(7), 1);
    ASSERT_EQ(r.read(9), 3);
    // tick() at the CPU clock
    setTime(r, 23, 1, 1, 0, 0, 0, 0);
    r.write(13, 0x08);
    r.tick(2457600);
    ASSERT_EQ(r.read(0), 1);
}

// 12-hour mode: the 10-hour register's bit 1 is PM
TEST_CASE(rp5c01_12_hour) {
    Rp5c01 r;
    r.hostClock = []() { return (std::time_t)1700000000; };
    r.write(13, 0x01);
    r.write(10, 0);                        // 12-hour
    setTime(r, 23, 1, 1, 0, 11, 59, 59);   // 11:59:59 AM
    r.write(13, 0x08);
    r.tickCrystal(32768);
    ASSERT_EQ(r.read(4), 2);               // 12 PM
    ASSERT_EQ(r.read(5), 0x1 | 0x2);
}

// /ALARM: the 16 Hz and 1 Hz pulses (until turned off) and the alarm match
TEST_CASE(rp5c01_alarm_pin) {
    Rp5c01 r;
    r.hostClock = []() { return (std::time_t)1700000000; };
    int falls = 0;
    r.onAlarm = [&](bool v) { if (!v) ++falls; };
    r.write(15, 0x00);                     // both pulses on
    r.tickCrystal(32768);
    ASSERT(falls >= 8);                    // 16 Hz in the first half; the 1 Hz holds the second low
    r.write(15, 0x0C);                     // both off
    falls = 0;
    r.tickCrystal(32768);
    ASSERT_EQ(falls, 0);
    ASSERT(r.alarmPin());
    // An alarm at 10:31 on the 5th
    setTime(r, 23, 1, 5, 4, 10, 30, 59);
    r.write(13, 0x01);
    r.write(2, 1); r.write(3, 3); r.write(4, 0); r.write(5, 1); r.write(7, 5); r.write(8, 0);
    r.write(6, 4);
    r.write(13, 0x0C);                     // timer and alarm on, block 0
    ASSERT(r.alarmPin());
    r.tickCrystal(32768);
    ASSERT(!r.alarmPin());
    r.write(15, 0x0D);                     // alarm reset clears the alarm registers
    ASSERT(r.alarmPin());
}

TEST_CASE(rp5c01_offset_and_state) {
    std::time_t now = 1700000000;
    Rp5c01 r;
    r.hostClock = [&]() { return now; };
    r.syncToHost();
    ASSERT_EQ(r.offsetSeconds(), 0);
    std::tm tm{};
    localtime_r(&now, &tm);
    ASSERT_EQ(r.read(2) + 10 * r.read(3), tm.tm_min);
    Rp5c01 s;
    s.loadState(r.saveState());
    for (int i = 0; i < 13; ++i) ASSERT_EQ(s.read(i), r.read(i));
}
