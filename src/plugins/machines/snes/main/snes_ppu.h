#pragma once

// The SNES PPU (S-PPU1 and S-PPU2, #15): $2100-$213F.
//
// After Mesen2 (Core/SNES/SnesPpu.cpp), GPL-3.0, Sour and the Mesen
// contributors, and the fullsnes / Anomie documentation. Drawn as the beam
// goes: before every register write the pixels up to the current dot are
// drawn with the old values (pixel x leaves at dot x + 22), so mid-line
// changes split the line as on the hardware. The BG tilemap and character
// data are fetched ahead, a column of 8 pixels in dots 8c-8c+7 (Mesen's
// slots per mode), so a mid-line change to the scroll, the bases, the mode
// or the mosaic reaches the tiles at the next fetch; the fine scroll is
// taken as each pixel is drawn. Sprites for a line are found
// and fetched in the previous line's horizontal blank. BG modes 0-7 with
// 8x8 and 16x16 tiles, 2/4/8 bpp, direct
// colour, offset-per-tile (modes 2, 4, 6), mode 7 (the matrix, flips,
// screen-over, EXTBG), sprites (the 8 size pairs, priority rotation, the
// 32-sprite / 34-sliver line limits), the two windows and their logic for
// every layer and the colour window, main/sub screens, colour math
// (add/subtract, half, fixed colour), mosaic, brightness and forced blank.
// Hi-res (modes 5 and 6, pseudo-hires) makes 512-pixel lines, the sub screen
// in the even pixels; interlace makes 448 or 478 lines, a field a frame, with
// modes 5/6 drawing twice the rows and OBJ interlace halving sprites. The
// overscan setting gives 239 lines. A frame with neither hi-res nor
// interlace is given at 256 pixels and one row a line.

#include "libdevices/main/io_handler.h"
#include "libdevices/main/ivideo_output.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace snes {

class Ppu : public IOHandler, public IVideoOutput {
public:
    static constexpr int WIDTH = 256;
    static constexpr int HEIGHT = 224;
    /** The frame buffer: 512 pixels and two rows a line, 239 lines. */
    static constexpr int MAX_WIDTH = 512;
    static constexpr int MAX_HEIGHT = 478;

    Ppu();
    ~Ppu() override = default;

    /** A write to $2100-$213F (`reg` = address & $3F). */
    void write(uint8_t reg, uint8_t value);
    /** A read of $2134-$213F; others return PPU1 open bus. */
    uint8_t read(uint8_t reg);
    uint8_t peek(uint8_t reg) const;

    /** Where the beam is (line, H clock 0-1363), given before each register access: during the picture
     *  VRAM can't be reached, and CGRAM and OAM accesses go where the PPU is reading (Mesen). */
    void setBeam(int line, int hclock) {
        m_beamLine = line; m_beamHClock = hclock;
        if (line != m_evalLine) { m_evalLine = line; m_evalStart = m_oamPriority ? (uint8_t)((m_oamAddr & 0x1FC) >> 2) : 0; }
    }
    /** Draw line `line` up to dot `dot` (before a register write). */
    void sync(int line, int dot);
    /** Horizontal blank of line `line` (0 to the last visible): finish it, fetch the next line's sprites. */
    void endLine(int line);
    /** Line 0: the field flips, the frame's overscan/interlace settings are taken. */
    void startFrame();
    /** Vertical blank starts: the frame is complete; OAM address reload. */
    void startVblank();
    /** The first line of vertical blank: 225, or 240 with overscan (as at line 0). */
    int vblankLine() const { return m_overscanFrame ? 240 : 225; }
    /** $2133 bit 0, and the field ($213F bit 7). */
    bool interlace() const { return m_setini & 1; }
    bool oddField() const { return m_interlaceField; }
    /** A PAL console: $213F bit 4. */
    void setPal(bool pal) { m_pal = pal; }
    /** $4201 bit 7 (the I/O line to port 2): a $213F read only clears the latch flag while it's set (Mesen). */
    void setIoBit(bool on) { m_ioBit = on; }
    /** Latch the H/V counters ($2137 read or WRIO bit 7). */
    void latchCounters(int dot, int line) { m_hLatch = (uint16_t)dot; m_vLatch = (uint16_t)line; m_latched = true; }
    bool forcedBlank() const { return m_forcedBlank; }
    uint64_t frameCount() const { return m_frames; }

    // Direct access for tests and the debugger
    uint16_t vram(uint16_t word) const { return m_vram[word & 0x7FFF]; }
    uint16_t cgram(uint8_t i) const { return m_cgram[i]; }
    uint8_t oam(uint16_t i) const { return m_oam[i % 544]; }
    int bgMode() const { return m_bgMode; }
    /** ARGB of a BGR555 colour at full brightness. */
    static uint32_t argb(uint16_t c);

    // ------------------------------------------------------------------
    // IOHandler
    // ------------------------------------------------------------------
    const char* name() const override { return m_name.c_str(); }
    void setName(const std::string& n) override { m_name = n; }
    uint32_t baseAddr() const override { return 0x2100; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void reset() override;
    void tick(uint64_t) override {}

    // ------------------------------------------------------------------
    // IVideoOutput
    // ------------------------------------------------------------------
    VideoDimensions getDimensions() const override;
    void renderFrame(uint32_t* buffer) override;

private:
    struct Pixel {
        uint16_t color = 0;     // BGR555
        uint8_t z = 0;          // 0 = transparent
        uint8_t layer = 5;      // 0-3 BG1-4, 4 OBJ, 5 backdrop
        int16_t cg = -1;        // its CGRAM address (-1: direct colour)
        bool objMath = true;    // OBJ palettes 0-3 never take part in colour math
    };
    /** `cg`: the CGRAM address the colour came from (-1: direct colour). */
    struct Layer { std::array<uint16_t, 256> color; std::array<uint8_t, 256> prio; std::array<bool, 256> opaque; std::array<int16_t, 256> cg; };
    // Access during the picture (Mesen's CanAccessVram / CanAccessCgram / GetOamAddress)
    bool rendering() const { return !m_forcedBlank && m_beamLine < vblankLine(); }
    bool canAccessCgram() const {
        return m_beamLine >= vblankLine() || m_beamLine == 0 || m_forcedBlank || m_beamHClock < 88 || m_beamHClock >= 1096;
    }
    uint16_t oamAccessAddr() const;
    uint16_t vramFetch() const { return rendering() ? 0 : m_vram[vramAddrRemap() & 0x7FFF]; }

    uint16_t vramAddrRemap() const;
    void vramIncrement(bool high);
    bool doubleWidth() const { return (m_setini & 0x08) || m_bgMode == 5 || m_bgMode == 6; }
    /** Draw pixels [x0, x1] of line `line` with the registers as they are. */
    void drawPixels(int line, int x0, int x1);
    /** One BG column's fetched data (Mesen's TileData): the map entry, the vertical offset it was fetched
     *  with, its 8-pixel column in the BG, and the character words (two halves in hi-res). */
    struct BgTile { uint16_t entry = 0, vofs = 0, tileX = 0; uint16_t chr[4] = {}; };
    /** The BG fetches of dots [x0, x1] (0-263) of line `line`, as Mesen's FetchTileData. */
    void fetchBg(int line, int x0, int x1);
    /** Fetch up to dot `dot` of the line being drawn. */
    void fetchTo(int line, int dot);
    void fetchMap(int bg, int line, int col);
    void fetchChr(int bg, int bpp, int line, int col, int plane, bool second);
    uint16_t mapEntry(int bg, int tileX, int tileY) const;
    /** The BG row of line `line` before the vertical offset (mosaic, interlaced hi-res), as Mesen's realY:
     *  `chr` for the character fetch, which in interlaced hi-res with mosaic drops the odd field's row. */
    int bgRow(int bg, int line, bool chr) const;
    /** The rows a mosaic block has gone down so far (Mesen's MosaicSize - _mosaicScanlineCounter). */
    int mosaicRows() const { return m_mosaicSize - m_mosaicCounter; }
    /** The mosaic's line counter at the end of a line (Mesen's ProcessEndOfScanline), done when the next line starts. */
    void mosaicLineEnd();
    /** BG pixels x0..x1 from the fetched columns; `even`: hi-res modes' even (sub screen) half-pixels. */
    void renderBg(int bg, int bpp, Layer& out, int x0, int x1, bool even);
    void renderMode7(int bg, int line, Layer& out, int x0, int x1);
    void renderObj(int line, Layer& out, std::array<uint8_t, 256>& pal);
    void mathPixel(uint16_t& a, uint16_t b, int x, bool inWindow);
    bool window(int layerSel, int x) const;           // layerSel 0-3 BG, 4 OBJ, 5 colour
    uint16_t paletteColor(int bpp, int bg, int pal, int idx) const;
    /** The CGRAM address paletteColor() reads (-1: direct colour). */
    int16_t paletteIndex(int bpp, int bg, int pal, int idx) const;

    std::string m_name = "ppu";

    std::array<uint16_t, 0x8000> m_vram{};
    std::array<uint16_t, 256> m_cgram{};
    std::array<uint8_t, 544> m_oam{};

    // $2100
    bool m_forcedBlank = true;
    uint8_t m_brightness = 0;
    // $2101
    uint16_t m_objBase = 0, m_objGap = 0x1000;
    uint8_t m_objSize = 0;
    // $2102-$2104
    uint16_t m_oamAddr = 0, m_oamReload = 0;
    bool m_oamPriority = false;
    uint8_t m_oamLatch = 0;
    // $2105-$210C
    int m_bgMode = 0;
    bool m_bg3Priority = false;
    uint8_t m_tile16 = 0;
    uint8_t m_mosaicSize = 1, m_mosaicMask = 0;
    // Mesen's _mosaicScanlineCounter: size + 1 from line 0 (or when the mosaic is turned on), down a line at a
    // time and back to the size at 0; the end of a line's step waits (m_mosaicDue: 1 step, 2 line 0's reload too)
    uint8_t m_mosaicCounter = 0, m_mosaicDue = 0;
    uint16_t m_mapBase[4] = {}, m_charBase[4] = {};
    uint8_t m_mapSize[4] = {};
    // $210D-$2114
    uint16_t m_hofs[4] = {}, m_vofs[4] = {};
    uint8_t m_ofsLatch = 0, m_hofsLatch = 0;
    int16_t m_m7hofs = 0, m_m7vofs = 0;
    // $2115-$2119, $2139/$213A
    uint8_t m_vmain = 0;
    uint16_t m_vramAddr = 0, m_vramPrefetch = 0;
    // Mode 7
    uint8_t m_m7sel = 0, m_m7latch = 0;
    int16_t m_m7[4] = {};          // A B C D
    int16_t m_m7x = 0, m_m7y = 0;
    // $2121/$2122/$213B
    uint8_t m_cgAddr = 0, m_cgLatch = 0;
    bool m_cgHigh = false;
    // Windows
    uint8_t m_winSel[3] = {};      // W12SEL W34SEL WOBJSEL
    uint8_t m_winPos[4] = {};      // WH0-WH3
    uint8_t m_winLog[2] = {};      // WBGLOG WOBJLOG
    uint8_t m_tm = 0, m_ts = 0, m_tmw = 0, m_tsw = 0;
    // Colour math
    uint8_t m_cgwsel = 0, m_cgadsub = 0;
    uint16_t m_fixed = 0;
    uint8_t m_setini = 0;
    // Counters and status
    uint16_t m_hLatch = 0, m_vLatch = 0;
    bool m_latched = false, m_hFlip = false, m_vFlip = false;
    bool m_rangeOver = false, m_timeOver = false;
    uint8_t m_ppu1Bus = 0, m_ppu2Bus = 0;
    int16_t m_mpy = 0;
    uint32_t m_mpyResult = 0;
    bool m_interlaceField = false;
    bool m_pal = false, m_ioBit = true;
    // The beam at the access, and where the PPU reads OAM and CGRAM: the sprite being evaluated (one per two
    // dots from the line's first, by the priority rotation), then the one being fetched; the CGRAM address
    // of the last colour looked up (0 when the backdrop shows on either screen, as Mesen)
    int m_beamLine = 0, m_beamHClock = 0, m_evalLine = -1;
    uint8_t m_evalStart = 0, m_timeIndex = 0, m_cgInternal = 0;

    // The line being drawn
    int m_drawLine = -1, m_drawX = 0;
    int m_fetchX = 0;                               // the next dot to fetch (0-263)
    BgTile m_bgTiles[4][33];                        // the line's columns, by BG
    uint16_t m_optH = 0, m_optV = 0;                // offset-per-tile: BG3's entries for the last column
    Layer m_obj;                                    // this line's sprites
    std::array<uint8_t, 256> m_objPal{};
    std::array<uint16_t, 256> m_main{}, m_sub{};    // BGR555, before brightness
    std::array<uint8_t, 256> m_mainFlags{};         // bit 0: colour math allowed
    std::array<bool, 256> m_subHas{};               // a sub screen layer (not the backdrop)

    // Frames (ARGB, MAX_WIDTH x MAX_HEIGHT), double-buffered
    std::vector<uint32_t> m_frame[2];
    bool m_frameWide[2] = {}, m_frameTall[2] = {};
    int m_back = 0;
    uint64_t m_frames = 0;
    bool m_overscanFrame = false, m_interlaceFrame = false, m_wideFrame = false;
};

}  // namespace snes
