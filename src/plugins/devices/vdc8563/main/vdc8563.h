#pragma once

#include "libdevices/main/io_handler.h"
#include "libdevices/main/ivideo_output.h"
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
 * The display (after vdc-draw.c, vdc.c), drawn a frame at a time from the
 * registers: R1 columns of (R22 >> 4) + 1 dots (doubled with R25 bit 4),
 * R6 rows of (R9 & 31) + 1 lines (R23: lines with character data), text
 * from the screen (R12/13) and character set (R28 bits 5-7, 16 or 32 bytes a
 * character) or a bitmap (R25 bit 7), attributes (R20/21, R25 bit 6:
 * colour, alternate set, reverse, underline at R29, blink), R26 colours,
 * reverse screen (R24 bit 6), semigraphics (R25 bit 5), the R22 displayed
 * width, smooth scroll (R24 / R25 low bits), the cursor (R10/R11, R14/15)
 * and its blink, R27 row skip; the 16 RGBI colours. The frame is 768 dots
 * by 272 lines, each line drawn twice (768 x 544), the picture centred;
 * the frame lasts (R4 + 1) * ((R9 & 31) + 1) + R5 lines of (R0 + 1) * 8
 * dots at 16 MHz. Not drawn: interlace, mid-frame register changes, the
 * sync and display-enable window (R2, R3, R7, R34, R35).
 */
class Vdc8563 : public IOHandler, public IVideoOutput {
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

    static constexpr int FRAME_W = 768;
    static constexpr int FRAME_H = 544;                     // 272 lines, doubled
    /** An RGBI colour as 0xAARRGGBB (VICE vdc-color.c). */
    static uint32_t palette(int i);

    // IVideoOutput
    VideoDimensions getDimensions() const override { return {FRAME_W, FRAME_H, 640, 400}; }
    void renderFrame(uint32_t* buffer) override;

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
    uint32_t m_frameCounter = 0;                            // frames drawn (cursor and attribute blink)
    uint64_t frameCycles() const;
    uint64_t lineCycles() const;
};
