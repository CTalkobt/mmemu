#pragma once

#include <cstdint>
#include <functional>
#include <vector>

/**
 * Hitachi HD61830 / HD61830B LCD timing controller (the Model 200's
 * 240x128 panel), after the HD61830 data sheet, MAME's hd61830.cpp (Curt
 * Coder) and VirtualT's T200_Disp (display.cpp); mmsim's own code.
 *
 * Bus: register select (offset & 1): 1 = instruction register (write) /
 * status (read), 0 = data. The Model 200 decodes it at I/O $F0 (data) and
 * $F1 (instruction/status), mirrored through $FF.
 *
 * Instructions (IR, low 4 bits), each takes one data-register write:
 *  - $0 mode control: bit 0 external CG, 1 graphic (1) / character (0),
 *       2 cursor on, 3 blink, 4 master, 5 display on
 *  - $1 character pitch: bits 0-2 horizontal pitch - 1 (hp 6-8 used),
 *       bits 4-7 vertical pitch - 1 (vp 1-16)
 *  - $2 number of characters (bytes per line) - 1 (hn, made even)
 *  - $3 number of time divisions - 1 (nx: the duty, i.e. rows per half)
 *  - $4 cursor position: the character line the cursor sits on (0-15)
 *  - $8 / $9 display start address low / high
 *  - $A / $B cursor address low / high. Writing the low byte when the
 *       counter's bit 7 is set and the new bit 7 is clear carries into
 *       the high byte (the data sheet's "the high byte is incremented";
 *       VirtualT and MAME both do it)
 *  - $C write display data at the cursor address, then increment it
 *  - $D read display data: the data register returns the output latch
 *       and reloads it from the cursor address, incrementing it; the first
 *       read after setting the address is a dummy
 *  - $E / $F clear / set bit (data bits 0-2) at the cursor address,
 *       then increment it
 *
 * Status: bit 7 busy. Busy lasts `busyTime` tick() units per instruction
 * (0, the default: never busy, as in MAME, whose busy flag is disabled);
 * data writes while busy are ignored.
 *
 * Display RAM is the controller's external RAM: the Model 200 has 8 KB,
 * which the 16-bit addresses wrap through (setRamSize).
 *
 * Rendering: the panel is (hn x hp) dots wide and 2 x nx rows high (the
 * HD61830 drives an upper and a lower half, each nx rows, the lower one
 * continuing the upper one's memory).
 *  - Graphic mode: each line is hn bytes, each byte hp dots, bit 0 the
 *    leftmost. The Model 200 uses this (hp 6, hn 40, nx 64: 240x128).
 *  - Character mode: hn characters per row, each hp x vp dots; character
 *    rows are nx / vp per half. The cursor (if on) underlines the
 *    character at the cursor address on line `cursor position`; with
 *    blink on it blinks (cursor on) or the whole character cell blinks to
 *    solid (cursor off) every 32 frames (frame()).
 *
 * The character generator: an external CG (mode bit 0) is read through
 * `externalCg(code, line)`. The HD61830A's internal mask ROM is not
 * reproduced: no freely licensed dump exists (MAME's is "typed in from the
 * manual" and not distributable here). FONT_5X7 is a generated stand-in
 * for $20-$7F built from mmsim's own MC6847 glyphs; codes $A0-$DF (katakana)
 * and $E0-$FF (5x11) render blank. The Model 200 runs the panel in
 * graphic mode, so its display does not depend on the font.
 */
class Hd61830 {
public:
    static constexpr uint8_t MODE_EXTERNAL_CG = 0x01;
    static constexpr uint8_t MODE_GRAPHIC = 0x02;
    static constexpr uint8_t MODE_CURSOR = 0x04;
    static constexpr uint8_t MODE_BLINK = 0x08;
    static constexpr uint8_t MODE_MASTER = 0x10;
    static constexpr uint8_t MODE_DISPLAY_ON = 0x20;

    static const uint8_t FONT_5X7[96][7];

    explicit Hd61830(size_t ramSize = 8192) { setRamSize(ramSize); reset(); }

    /** Display RAM size (a power of two; addresses wrap). Clears it. */
    void setRamSize(size_t bytes);
    size_t ramSize() const { return m_ram.size(); }

    /** The /RES pin: display off, slave mode, hp 6 (MAME's reset). */
    void reset();

    /** offset & 1: 1 status, 0 data (reads with side effects). */
    uint8_t read(int offset);
    /** offset & 1: 1 instruction register, 0 data. */
    void write(int offset, uint8_t v);
    uint8_t peek(int offset) const { return (offset & 1) ? status() : m_dor; }

    uint8_t status() const { return m_busyLeft ? 0x80 : 0x00; }
    void writeInstruction(uint8_t v) { m_ir = v & 0x0F; }
    void writeData(uint8_t v);
    uint8_t readData();

    void tick(uint64_t units) { m_busyLeft = m_busyLeft > units ? m_busyLeft - units : 0; }
    void setBusyTime(uint32_t units) { m_busyTime = units; }
    /** A display frame has passed (drives the blink: every 32 frames). */
    void frame();
    bool blinkPhase() const { return m_blinkOn; }

    uint8_t ir() const { return m_ir; }
    uint8_t mode() const { return m_mcr; }
    int horizontalPitch() const { return m_hp; }
    int verticalPitch() const { return m_vp; }
    int charactersPerLine() const { return m_hn; }
    int timeDivisions() const { return m_nx; }
    int cursorLine() const { return m_cp; }
    uint16_t displayStart() const { return m_dsa; }
    uint16_t cursorAddress() const { return m_cac; }

    uint8_t ram(uint16_t addr) const { return m_ram[addr & m_mask]; }
    void setRam(uint16_t addr, uint8_t v) { m_ram[addr & m_mask] = v; }

    /** The panel size the registers describe. */
    int width() const { return m_hn * m_hp; }
    int height() const { return 2 * m_nx; }
    /** Renders into a w x h buffer of 0/1 dots (clipped; blank when off). */
    void render(uint8_t* out, int w, int h) const;
    std::vector<uint8_t> render(int w, int h) const;

    /** Reads an external character generator: (code, line) -> dots (bit 0 leftmost). */
    std::function<uint8_t(uint8_t code, int line)> externalCg;

    std::vector<uint8_t> saveState() const;
    void loadState(const std::vector<uint8_t>& s);

private:
    uint8_t charLine(uint8_t code, int line) const;
    void stepCursor() { m_cac = (uint16_t)(m_cac + 1); }

    std::vector<uint8_t> m_ram;
    uint16_t m_mask = 0x1FFF;
    uint8_t m_ir = 0, m_mcr = 0, m_dor = 0;
    int m_hp = 6, m_vp = 8, m_hn = 40, m_nx = 64, m_cp = 7;
    uint16_t m_dsa = 0, m_cac = 0;
    uint32_t m_busyTime = 0;
    uint64_t m_busyLeft = 0;
    int m_frames = 0;
    bool m_blinkOn = false;
};
