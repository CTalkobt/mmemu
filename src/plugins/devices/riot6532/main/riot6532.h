#pragma once

#include "libdevices/main/io_handler.h"
#include <cstdint>
#include <functional>

/**
 * MOS 6532 RIOT (RAM, I/O, timer) as the Atari 2600 uses it, after Stella
 * (src/emucore/M6532.cxx), GPL-2.0-or-later, Stella team.
 *
 *   A9 = 0       128 bytes of RAM (address bits 0-6)
 *   A9 = 1, A2 = 0: the ports (A1-A0): SWCHA, SWACNT, SWCHB, SWBCNT
 *   A9 = 1, A2 = 1: read  INTIM (A0 = 0) or the interrupt flags (A0 = 1);
 *                   write A4 = 1: the timer, A1-A0 the interval (1, 8, 64,
 *                   1024 cycles); A4 = 0: the PA7 edge (A0 = 1 positive)
 *
 * The timer counts down every interval and, at zero, wraps and counts every
 * cycle with its flag (bit 7) set; reading INTIM or writing the timer clears
 * the flag. Bit 6 is the PA7 edge flag (cleared when the flags are read).
 * Port A reads the joysticks (inputs where SWACNT is 0), port B the console
 * switches. The chip is clocked lazily: sync() brings it to a CPU cycle.
 */
class Riot6532 : public IOHandler {
public:
    Riot6532();

    /** Port A's pins (bits 7-4 the left joystick, 3-0 the right; 0 = closed)
     *  and port B's (the console switches). */
    void setPortA(std::function<uint8_t()> pins) { m_portA = std::move(pins); }
    void setPortB(std::function<uint8_t()> pins) { m_portB = std::move(pins); }

    /** Bring the timer up to `cpuCycle`. */
    void sync(uint64_t cpuCycle);
    /** A read or write of the RIOT's address space ($80-$FF RAM, $280-$29F). */
    uint8_t read(uint16_t addr);
    uint8_t peek(uint16_t addr) const;
    void write(uint16_t addr, uint8_t value);

    uint8_t ram(int i) const { return m_ram[i & 0x7F]; }
    uint8_t timer() const { return m_timer; }
    uint8_t flags() const { return m_flags; }

    const char* name() const override { return "RIOT"; }
    uint32_t baseAddr() const override { return 0; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void reset() override;
    void tick(uint64_t) override {}

    static constexpr uint8_t TIMER_BIT = 0x80, PA7_BIT = 0x40;

private:
    void setTimer(uint8_t value, int interval);
    bool pa7() const;
    uint8_t portAPins() const { return m_portA ? m_portA() : 0xFF; }

    uint8_t m_ram[128] = {};
    uint8_t m_timer = 0;
    uint32_t m_divider = 1024, m_shift = 10, m_subTimer = 0;
    bool m_wrappedThisCycle = false;
    uint64_t m_lastCycle = 0;
    uint8_t m_ddrA = 0, m_ddrB = 0, m_outA = 0, m_outB = 0;
    uint8_t m_flags = 0;
    bool m_edgePositive = false, m_pa7Sync = true, m_pa7Stable = true;
    std::function<uint8_t()> m_portA, m_portB;
};
