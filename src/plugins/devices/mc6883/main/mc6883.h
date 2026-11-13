#pragma once

#include <cstdint>
#include <functional>
#include <vector>

/**
 * Motorola MC6883 / SN74LS783 Synchronous Address Multiplexer (SAM), after
 * the data sheet; the video address counter's divider chain as XRoar
 * models it (mc6883.c, after Stewart Orchard's research).
 *
 * The 16-bit control register is set a bit at a time by writes to
 * $FFC0-$FFDF: an even address clears the bit (address >> 1 & 15), an odd
 * one sets it.
 *   bits 0-2   V0-V2  VDG addressing mode (X/Y dividers, see below)
 *   bits 3-9   F0-F6  display offset: video base = F << 9
 *   bit 10     P1     page #1 (64K: the lower 32K comes from the upper)
 *   bits 11-12 R0-R1  CPU rate: 0 0.89 MHz, 1 1.78 MHz for ROM and I/O
 *                     above $FF1F (address-dependent), 2-3 1.78 MHz always
 *   bits 13-14 M0-M1  memory size: 0 4K, 1 16K, 2-3 64K dynamic RAM
 *   bit 15     TY     map type: 0 32K RAM + 32K ROM, 1 64K RAM
 *
 * access() decodes a CPU cycle: S (the device select: 0 RAM read, 1 ROM
 * $8000, 2 ROM $A000 and the vectors $FFE0-$FFFF, 3 cartridge $C000-$FEFF,
 * 4 $FF00-$FF1F, 5 $FF20-$FF3F, 6 $FF40-$FF5F, 7 none), the RAM's row and
 * column and bank (RAS0/RAS1) for the memory size, and the cycle's length
 * in 14.31818 MHz ticks: 16 slow, 8 fast, with the extra ticks the SAM
 * takes changing rate (15, 17 or 25: it keeps the VDG's slots).
 *
 * Video: the VDG's HS clears the low bits of the counter (bits 1-4 or 1-3
 * by mode), FS loads it from F; videoBytes() hands out the next run of
 * addresses (to a 16-byte boundary), whose wrap clocks the X and Y
 * dividers: V 0 (X1 Y12, alphanumerics), 1 (X3 Y1), 2 (X1 Y3), 3 (X2 Y1),
 * 4 (X1 Y2), 5 (X1 Y1), 6 (X1 Y1), 7 (DMA).
 */
class Mc6883 {
public:
    enum Select : uint8_t { S_RAM, S_ROM0, S_ROM1, S_CTS, S_IO0, S_IO1, S_IO2, S_NONE };

    /** What one CPU cycle selects. */
    struct Cycle {
        uint8_t s = S_NONE;
        bool ram = false;        ///< RAM takes part (read or written)
        bool write = false;      ///< RAM written (/WE)
        uint8_t bank = 0;        ///< RAS1: the second bank of 4K/16K chips
        uint16_t row = 0, col = 0;
        int ticks = 16;
    };

    Mc6883();

    /** Called before a register write that moves the video address (V, F):
     *  the VDG draws up to now. */
    std::function<void()> onVideoChange;

    /** /RESET: the register cleared, the video counter loaded. */
    void reset();
    /** A CPU cycle at `a` (writes to $FFC0-$FFDF set the register). */
    Cycle access(uint16_t a, bool write);
    /** The device select for `a`, without side effects. */
    uint8_t select(uint16_t a, bool write) const;
    /** access()'s decode (select, RAM address) without timing or register writes. */
    Cycle decode(uint16_t a, bool write) const;

    uint16_t reg() const;
    void setReg(uint16_t v);
    unsigned v() const { return m_v; }
    uint16_t f() const { return m_f; }
    bool page() const { return m_p; }
    unsigned rate() const { return m_r; }
    unsigned memSize() const { return m_m; }
    bool mapType() const { return m_ty; }

    // Video address counter (the VDG's DA0, HS and FS)
    void hsync(bool level);
    void fsync(bool level);
    /** Up to `n` bytes of video data from the counter: returns how many
     *  follow on (to the next 16-byte boundary) from row/col. */
    int videoBytes(int n, uint16_t& row, uint16_t& col);
    /** The counter (A15-A0 of the next video byte). */
    uint16_t videoAddress() const;

    std::vector<uint8_t> saveState() const;
    void loadState(const std::vector<uint8_t>& s);

private:
    enum { C_B15_5, C_YDIV4, C_YDIV3, C_YDIV2, C_B4, C_XDIV3, C_XDIV2, C_B3_0, C_GROUND, NUM_COUNTERS };
    struct Counter {
        uint16_t value = 0;
        uint16_t mod = 1;
        uint16_t outMask = 0;
        bool in = false, out = false;
        int from = C_GROUND;
    };

    void update();                      // after a register change
    void setCounter(int i, int value);
    void clockCounter(int i);           // re-read its input; count on a fall
    void translate(uint16_t a, uint16_t& row, uint16_t& col, uint8_t& bank, bool cpu) const;

    unsigned m_v = 0, m_r = 0, m_m = 0;
    uint16_t m_f = 0;
    bool m_p = false, m_ty = false;
    unsigned m_vPrev = 0;

    bool m_runningFast = false, m_extendSlow = false;
    uint16_t m_lastRow = 0, m_lastCol = 0;       // the CPU's last RAM address (fast mode video)

    // Address multiplexer (by memory size)
    uint16_t m_rowMask = 0x7F, m_colMask = 0x3F, m_ras1Bit = 0x1000, m_ras1 = 0x80, m_pageBit = 0;
    uint8_t m_colShift = 6;

    int m_clear = 0;                    // HS clears bits 1-4 (4), 1-3 (3) or none (0)
    Counter m_c[NUM_COUNTERS];
};
