#pragma once

#include <cstdint>
#include <string>
#include <vector>

/**
 * Thomson EF9345 semigraphic display processor (the Matra Alice 32 and
 * Alice 90, the Minitel), after MAME's ef9345.cpp (Daniel Coulom, Sandro
 * Ronco; GPL-2.0+), which follows the data sheet; mmsim's own code.
 *
 * The host sees eight registers R0-R7 (offsets 0-7; R0 read is the
 * status: bit 7 busy, 6 Al, 5 LXm, 4 LXa, 2 vertical sync). A write at
 * offset 8-15 writes R(offset & 7) and then executes the command in R0:
 * KRF/KRG (24/16-bit character codes to and from the video RAM), CLF/CLG
 * (clear a page), OCT (bytes), KRL (80-column codes), IND (the indirect
 * registers TGS, MAT, PAT, DOR, ROR and the character ROM), INY, the MVB/
 * MVD/MVT block moves, NOP/VSM/VRM. Each takes its time (the busy flag);
 * KRC, the 8-bit 80-column write, is not done (as MAME).
 *
 * The 16 KB video RAM is the chip's own (`vram`). The 8 KB character ROM
 * (the chip's, user-supplied) holds G0 (alphanumerics), G10/G11
 * (semigraphics) and the accented letters built from them.
 *
 * Modes (TGS b7-6, PAT b7): 40 columns long codes (24 bits: C, B, A),
 * 40 columns short codes (16 bits, with serial attributes and
 * delimiters), 80 columns (12 bits); 40 columns variable and 80 columns
 * 8-bit codes are blank (not done, as in MAME). 25 rows: the service row 0 and 24 more from ROR's
 * origin, double height and width, flash, conceal, negative, underline,
 * the cursor (MAT b6-4), quadrichrome characters from the video RAM.
 *
 * Time: lines of 64 us, 312 to a frame (50 Hz). As in MAME, a row of ten
 * lines is drawn at its first line; a command running then waits 104 us
 * more (the display has the memory). The output is OUT_H lines of
 * width() pixels: 336 (42 cells of 8) in 40 columns, 492 (82 of 6) in
 * 80, the margin (MAT's colour) round 40 x 25 cells of 8 x 10 dots.
 */
class Ef9345 {
public:
    static constexpr int OUT_H = 270;
    static constexpr int W40 = 336, W80 = 492;
    static constexpr int LINES = 312;
    static constexpr size_t VRAM_SIZE = 0x4000;
    static constexpr size_t CHARSET_SIZE = 0x2000;

    Ef9345();
    /** The character ROM (8 KB; shorter is padded with zeros). */
    void setCharset(const std::vector<uint8_t>& rom);
    bool hasCharset() const { return m_haveCharset; }
    /** The rate of advance()'s ticks (Hz). */
    void setClock(double hz);
    void reset();

    /** The host bus: offsets 0-15 (see above). */
    uint8_t read(uint8_t offset);
    uint8_t peek(uint8_t offset) const;
    void write(uint8_t offset, uint8_t v);

    /** Time passes, in ticks of setClock()'s rate. */
    void advance(uint64_t ticks);

    int width() const { return is80() ? W80 : W40; }
    /** The frame: OUT_H lines of width() pixels, 0xAARRGGBB. */
    void render(uint32_t* out) const;
    /** A pixel's colour (0-7: bit 0 red, 1 green, 2 blue). */
    uint8_t pixel(int x, int y) const { return m_out[(size_t)y * W80 + (size_t)x] & 7; }

    uint8_t vram(uint16_t a) const { return m_vram[a & (VRAM_SIZE - 1)]; }
    void setVram(uint16_t a, uint8_t v) { m_vram[a & (VRAM_SIZE - 1)] = v; }
    uint8_t reg(int r) const { return m_reg[r & 7]; }
    uint8_t tgs() const { return m_tgs; }
    uint8_t mat() const { return m_mat; }
    uint8_t pat() const { return m_pat; }
    uint8_t dor() const { return m_dor; }
    uint8_t ror() const { return m_ror; }
    bool busy() const { return m_busy; }
    uint32_t frames() const { return m_frames; }

    /** Displayed row `y` (0 the service row, 1-24) as text: the characters
     *  of the G0 set as ASCII, others '.' (tests and the debugger). */
    std::string rowText(int y) const;

    std::vector<uint8_t> saveState() const;
    void loadState(const std::vector<uint8_t>& s);

private:
    // 40 columns: 24-bit, variable, 16-bit codes; 80 columns: 12-bit, 8-bit codes
    enum class Mode : uint8_t { Long40, Var40, Short40, Bits12x80, Bits8x80 };
    bool is80() const { return m_mode == Mode::Bits12x80 || m_mode == Mode::Bits8x80; }
    uint64_t nsToTicks(uint64_t ns) const { return (uint64_t)((double)ns * m_hz / 1e9); }

    uint16_t indexRam(int r) const;
    void incX(int r);
    void incY(int r);
    void setBusy(uint64_t ns);
    void setVideoMode();
    void exec(uint8_t cmd);
    void scanline(int line);

    uint8_t readChar(uint8_t index, uint16_t addr) const;
    uint8_t dial(int x, uint8_t attrib);
    static void zoom(uint8_t* pix, uint16_t n);
    uint16_t indexBlock(int x, int y) const;
    uint16_t indexRow(int y) const;
    void makeColours(uint8_t& c0, uint8_t& c1, bool& underline, bool insert, bool flash, bool conceal, bool negative,
                     bool cursor) const;
    void makeChar(int x, int y);
    void makeChar16x40(int x, int y);
    void makeChar24x40(int x, int y);
    void makeChar12x80(int x, int y);
    void bichrome40(uint8_t type, uint16_t address, uint8_t dl, uint16_t iblock, int x, int y, uint8_t c0, uint8_t c1,
                    bool insert, bool flash, bool conceal, bool negative, bool underline);
    void quadrichrome40(uint8_t c, uint8_t b, uint8_t a, int x, int y);
    void bichrome80(uint8_t c, uint8_t a, int x, int y, bool cursor);
    void drawChar40(const uint8_t* c, int x, int y);
    void drawChar80(const uint8_t* c, int x, int y);
    void drawBorder(int row);

    std::vector<uint8_t> m_vram, m_charset, m_accented, m_out;
    bool m_haveCharset = false;
    double m_hz = 1e6;
    uint64_t m_lineTicks = 64;

    uint8_t m_reg[8] = {};
    uint8_t m_state = 0;
    uint8_t m_tgs = 0, m_mat = 0, m_pat = 0, m_dor = 0, m_ror = 0;
    Mode m_mode = Mode::Long40;
    uint8_t m_border[80] = {};
    uint16_t m_block = 0;
    uint16_t m_ramBase[4] = {};
    uint8_t m_blink = 0;
    uint8_t m_lastDial[40] = {};
    uint8_t m_latchC0 = 0, m_latchM = 0, m_latchI = 0, m_latchU = 0;
    bool m_busy = false;
    uint64_t m_busyLeft = 0;            // ticks
    uint64_t m_lineLeft = 0;            // ticks to the next line
    int m_line = 0;
    uint64_t m_blinkLeft = 0;
    uint32_t m_frames = 0;
};
