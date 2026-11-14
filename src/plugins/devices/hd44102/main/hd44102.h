#pragma once

#include <array>
#include <cstdint>
#include <vector>

/**
 * Hitachi HD44102 dot-matrix LCD column driver: 50 columns x 32 rows, with
 * its own display RAM of 4 pages x 50 bytes (each byte 8 vertical dots,
 * bit 0 at the top).
 *
 * After the HD44102CH data sheet, cross-checked with MAME's hd44102.cpp
 * (Curt Coder) and VirtualT's io.c / display.cpp (the Model 100's use of
 * it); mmsim's own code.
 *
 * Register select (offset & 1): 0 = instruction (write) / status (read),
 * 1 = display data.
 *
 * Instructions (data sheet encodings; anything else with Y > 49 is ignored):
 *  - $38 / $39   display off / on
 *  - $3A / $3B   count down / count up (the Y counter after each data access)
 *  - pp111110    display start page pp: RAM page pp shows on the top 8 rows
 *  - ppyyyyyy    set the address: page (X) pp, column (Y) 0-49
 *
 * Status: bit 7 busy, 6 up mode (1 = count up), 5 display off (1 = off),
 * 4 reset (the /RST pin held low). Bits 0-3 read 0.
 *
 * Data: a write stores to RAM[page][Y] and steps Y; a read returns the
 * output latch and reloads it from RAM[page][Y], stepping Y, so the first
 * read after setting the address is a dummy (the data sheet's, and what
 * VirtualT's "fresh pointer" logic amounts to). Y wraps 49 -> 0 counting up
 * and 0 -> 49 counting down; the page never changes by itself.
 *
 * Busy: each instruction or data access holds the busy flag for
 * `busyTime` units of tick() (0, the default: never busy). While busy,
 * instructions and data writes are ignored, as on the chip.
 *
 * Reset (/RST low): display off, count up, start page 0; the status reset
 * bit reads 1 while the pin is held. Power-on equals a reset.
 */
class Hd44102 {
public:
    static constexpr int COLUMNS = 50;
    static constexpr int ROWS = 32;
    static constexpr int PAGES = 4;

    static constexpr uint8_t STATUS_BUSY = 0x80;
    static constexpr uint8_t STATUS_UP = 0x40;
    static constexpr uint8_t STATUS_OFF = 0x20;
    static constexpr uint8_t STATUS_RESET = 0x10;

    Hd44102() { reset(); }

    /** The /RST pulse: display off, count up, start page 0 (RAM kept). */
    void reset();
    /** Hold or release /RST (true = held low: reset and the status bit set). */
    void setResetPin(bool held);

    /** offset & 1: 0 status, 1 data (the read steps Y). */
    uint8_t read(int offset);
    /** offset & 1: 0 instruction, 1 data. */
    void write(int offset, uint8_t v);
    /** The status register or the output latch, without side effects. */
    uint8_t peek(int offset) const;

    uint8_t status() const;
    void writeInstruction(uint8_t v);
    void writeData(uint8_t v);
    uint8_t readData();

    /** Time passes (whatever units busyTime is in). */
    void tick(uint64_t units);
    void setBusyTime(uint32_t units) { m_busyTime = units; }

    bool displayOn() const { return !m_off; }
    bool countUp() const { return m_up; }
    int startPage() const { return m_startPage; }
    int page() const { return m_page; }
    int column() const { return m_y; }

    /** Display RAM, page-major (RAM[page][column]). */
    uint8_t ram(int page, int column) const { return m_ram[page & 3][column % COLUMNS]; }
    void setRam(int page, int column, uint8_t v) { m_ram[page & 3][column % COLUMNS] = v; }

    /** The dot the panel shows at (x 0-49, y 0-31): false when the display is off. */
    bool pixel(int x, int y) const;
    /** The 50x32 panel image, one byte per dot (0/1), row-major. */
    void render(uint8_t* out, int stride = COLUMNS) const;

    std::vector<uint8_t> saveState() const;
    void loadState(const std::vector<uint8_t>& s);

private:
    void step();
    void startBusy() { m_busyLeft = m_busyTime; }

    std::array<std::array<uint8_t, COLUMNS>, PAGES> m_ram{};
    uint8_t m_latch = 0;
    int m_page = 0, m_y = 0, m_startPage = 0;
    bool m_up = true, m_off = true, m_resetHeld = false;
    uint32_t m_busyTime = 0;
    uint64_t m_busyLeft = 0;
};
