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
 * The display, a raster line at a time as VICE draws it (vdc.c
 * vdc_raster_draw_alarm_handler, vdc-draw.c): the video signal side counts
 * character rows (R9 lines each, R4 + 1 rows, R5 lines of vertical fine
 * adjust), enables the display for R6 rows and starts vertical sync at row
 * R7; the drawing side fetches each character row's screen and attribute
 * bytes into a buffer as the row starts and draws every line with the
 * registers as they are then, so changes in mid-frame (raster splits, FLI
 * tricks) show. Screen and attribute addresses (R12/13, R20/21) are taken
 * after the last displayed row. Interlace (R8 = 3: sync and video) draws
 * the even and odd fields on alternate lines and steps the lines by two.
 * Text and bitmap modes, attributes (colour, alternate set, reverse,
 * underline at R29, blink), monochrome, 40-column double-pixel mode, cursor
 * and its blink, reverse screen, semigraphics, the R22 displayed width,
 * smooth scroll (R24 / R25), R27 row skip; the 16 RGBI colours; R34/R35
 * blanking as VICE has it. The left border follows the horizontal sync
 * position (R2). VICE's 856 x 288 (PAL; NTSC 240) picture is shown as
 * 768 x 272 lines, each drawn twice (or the two interlace fields).
 * A line lasts (R0 + 1) * 8 dots at 16 MHz.
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
    /** VICE's canvas: 856 dots a line; FRAME_W of them are shown from X_OFFSET. */
    static constexpr int CANVAS_W = 856;
    static constexpr int X_OFFSET = 44;
    /** An RGBI colour as 0xAARRGGBB (VICE vdc-color.c). */
    static uint32_t palette(int i);

    // IVideoOutput
    VideoDimensions getDimensions() const override { return {FRAME_W, FRAME_H, 640, 400}; }
    void renderFrame(uint32_t* buffer) override;

    /** Frames completed (the vertical sync ended) since reset. */
    uint32_t frameCounter() const { return m_frameCounter; }
    /** Interlace with video (R8 = 3): fields on alternate lines. */
    bool interlaced() const { return m_interlaced; }

private:
    uint32_t map(uint16_t addr) const;
    uint8_t ramRead(uint32_t addr) const { return m_ram[map((uint16_t)addr)]; }
    void writeData();
    void fillCopy();
    uint8_t status() const;
    void registerWritten(int reg, uint8_t oldval);
    void updateGeometry();
    void calculateCharwidth();
    void latchAddresses();
    void rasterLine();
    void drawLine(int mode);

    std::vector<uint8_t> m_ram;
    uint8_t m_regs[38] = {};
    uint8_t m_index = 0;
    uint32_t m_clockHz = 1022727;
    bool m_pal = false;
    uint32_t m_frameCounter = 0;                            // frames (cursor and attribute blink)

    // Line timing (vdc_calculate_xsync): CPU cycles a line, 16.16
    uint64_t m_acc = 0;
    int m_xcharsTotal = 127;
    // The video signal side (vdc.c)
    unsigned m_rowCounter = 0, m_rowCounterY = 0;
    bool m_rowCounterLatch = false, m_vertFineAdj = false, m_displayEnable = false;
    bool m_vsync = false;
    int m_vsyncCounter = 0, m_vsyncHeight = 25;
    unsigned m_interlaced = 0;
    // The drawing side
    bool m_primeDraw = false, m_drawActive = false, m_drawFinished = false, m_drawCounterLatch = false;
    unsigned m_drawCounter = 0, m_drawCounterY = 0;
    uint32_t m_memCounter = 0, m_memCounterInc = 0, m_bitmapCounter = 0;
    uint32_t m_screenAdr = 0, m_attributeAdr = 0, m_attributeOffset = 0;
    uint8_t m_scrnbuf[0x200] = {}, m_attrbuf[0x200] = {};
    unsigned m_bufDraw = 0;                                 // 0 or 0x100: the half being drawn
    bool m_attributeBlink = false;
    // Geometry (vdc_update_geometry) and the horizontal layout
    bool m_updateGeometry = true;
    int m_screenTextCols = 80, m_borderWidth = 112, m_charwidth = 8, m_xsmooth = 7;
    // The picture: canvas line (since the vertical sync ended), canvas
    // height (PAL 288, NTSC 240), its detection, the frames
    int m_currentLine = 0, m_canvasHeight = 288, m_canvasHeightOld = 0, m_stableSizeCount = 0;
    std::vector<uint32_t> m_back, m_front;
};
