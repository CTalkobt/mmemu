#pragma once

#include "libdevices/main/io_handler.h"
#include <cstdint>
#include <vector>

/**
 * MOS 8563 VDC, the C128's 80-column video controller: the register and
 * memory interface, after VICE (src/vdc/vdc-mem.c), GPL-2.0-or-later.
 *
 *   $D600 write  register index (bits 0-5)
 *   $D600 read   status: bit 7 ready, bit 6 light pen, bit 5 vertical
 *                blank (outside the display rows), bits 0-2 the revision
 *   $D601        the selected register; R31 reads and writes the VDC RAM
 *                at the update address R18/R19 and advances it; a write to
 *                R30 (word count) fills (R24 bit 7 = 0, with R31's value)
 *                or copies (from the block address R32/R33)
 *
 * Both addresses mirror through $D600-$D6FF. Unused register bits read 1
 * (VICE's regmask); registers 38-63 read $FF. The RAM is 16 KB (the C128's
 * 4416 chips) or 64 KB (C128DCR, 4464), translated by R28 bit 4 as VICE
 * does. The status bit 7 is always set: accesses complete at once here.
 *
 * Not yet: the 80-column display itself (#22).
 */
class Vdc8563 : public IOHandler {
public:
    explicit Vdc8563(bool ram64k = false);

    void setClockHz(uint32_t hz) override { m_clockHz = hz ? hz : 1; }
    void setPal(bool pal) { m_pal = pal; }

    const char* name() const override { return "VDC"; }
    uint32_t baseAddr() const override { return 0xD600; }
    uint32_t addrMask() const override { return 0x00FF; }
    bool ioRead(IBus* bus, uint32_t addr, uint8_t* val) override;
    bool ioPeek(IBus* bus, uint32_t addr, uint8_t* val) override;
    bool ioWrite(IBus* bus, uint32_t addr, uint8_t val) override;
    void reset() override;
    void tick(uint64_t cycles) override;

    /** A VDC register (0-37) as stored. */
    uint8_t reg(int r) const { return r >= 0 && r < 38 ? m_regs[r] : 0xFF; }
    /** A VDC RAM byte by VDC address (R28 bit 4 translation applied). */
    uint8_t ramByte(uint16_t addr) const { return m_ram[map(addr)]; }
    bool ram64k() const { return m_ram.size() == 0x10000; }

private:
    uint32_t map(uint16_t addr) const;
    void writeData();
    void fillCopy();
    uint8_t status() const;

    std::vector<uint8_t> m_ram;
    uint8_t m_regs[38] = {};
    uint8_t m_index = 0;
    uint32_t m_clockHz = 1022727;
    bool m_pal = false;
    uint64_t m_frameCycle = 0;
};
