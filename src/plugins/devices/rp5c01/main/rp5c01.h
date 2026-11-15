#pragma once

#include <array>
#include <cstdint>
#include <ctime>
#include <functional>
#include <vector>

/**
 * Ricoh RP5C01 real-time clock with 26 nibbles of RAM: the Model 200's RTC
 * (MAME's tandy200 and VirtualT's rp5c01_read/write both put it at I/O
 * $90-$9F; the Model 200 does not use the uPD1990AC).
 *
 * After the Ricoh RP5C01 data sheet; MAME's rp5c01.cpp (Curt Coder)
 * consulted. mmsim's own code.
 *
 * Sixteen 4-bit registers (address & 15). $D mode, $E test (write only),
 * $F reset (write only) are always there; $0-$C are one of four blocks,
 * chosen by mode bits 0-1:
 *  - block 0, the time counter: 0 1-sec, 1 10-sec, 2 1-min, 3 10-min,
 *    4 1-hour, 5 10-hour (bit 1 = PM in 12-hour mode), 6 day of week (0-6),
 *    7 1-day, 8 10-day, 9 1-month, A 10-month, B 1-year, C 10-year
 *  - block 1, the alarm: 2-8 as above (minute to 10-day), A bit 0 the
 *    12/24 select (1 = 24 hours), B bits 0-1 the leap-year counter (0 = a
 *    leap year)
 *  - blocks 2 and 3: RAM, 13 bytes, block 2 their low nibbles, block 3
 *    the high ones
 * Writes are masked to the bits each register has; reads return them in
 * bits 0-3 (bits 4-7 read 0 here; the chip drives only D0-D3).
 *
 * Mode: bit 2 alarm enable, bit 3 timer enable (the counter runs).
 * Reset: bit 0 resets the alarm registers, bit 1 the divider stages below
 * one second, bit 2 turns the 16 Hz pulse on /ALARM off, bit 3 the 1 Hz.
 *
 * /ALARM (alarmPin, true = high): low while the alarm matches (enabled),
 * or with the 16 Hz / 1 Hz square waves when those are on (they are, after
 * power-on, as on the chip: the reset register clears).
 *
 * Time: emulated (tick() at the input clock, with the 32768 Hz crystal
 * behind it), seeded from host local time + an offset (syncToHost); a
 * counter write moves the offset so a later sync keeps the program's
 * time. The year rolls over 99 -> 00 and advances the leap counter.
 */
class Rp5c01 {
public:
    Rp5c01();

    uint8_t read(int reg) const;
    void write(int reg, uint8_t v);

    void setInputHz(double hz) { m_inputHz = hz; }
    void tick(uint64_t cycles);
    void tickCrystal(uint64_t xtalTicks);

    bool alarmPin() const;
    std::function<void(bool)> onAlarm;

    uint8_t mode() const { return m_mode; }
    /** Block b's register r (0-12), as stored. */
    uint8_t reg(int block, int r) const { return m_reg[block & 1][r % 13]; }
    uint8_t ram(int i) const { return m_ram[i % 13]; }

    void setOffsetSeconds(int64_t s) { m_offset = s; }
    int64_t offsetSeconds() const { return m_offset; }
    void syncToHost();
    std::function<std::time_t()> hostClock;

    std::vector<uint8_t> saveState() const;
    void loadState(const std::vector<uint8_t>& s);

private:
    void crystal();
    void advanceSecond();
    void checkAlarm();
    void updatePin();
    bool is24h() const { return m_reg[1][10] & 1; }
    int hour24() const;
    void setHour24(int h);
    std::tm counterTm() const;

    std::array<std::array<uint8_t, 13>, 2> m_reg{};
    std::array<uint8_t, 13> m_ram{};
    uint8_t m_mode = 0x08, m_reset = 0, m_test = 0;
    double m_inputHz = 2457600.0, m_frac = 0;
    uint64_t m_div = 0;                  // crystal ticks into the second (0-32767)
    bool m_alarmMatch = false, m_pin = true;
    int64_t m_offset = 0;
};
