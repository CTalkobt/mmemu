#pragma once

// The SNES PPU (S-PPU1 and S-PPU2, #15): $2100-$213F.
//
// After Mesen2 (Core/SNES/SnesPpu.cpp), GPL-3.0, Sour and the Mesen
// contributors, and the fullsnes / Anomie documentation. Rendered a line at
// a time, at the start of horizontal blank (before HDMA changes registers for
// the next line): BG modes 0-7 with 8x8 and 16x16 tiles, 2/4/8 bpp, direct
// colour, offset-per-tile (modes 2, 4, 6), mode 7 (the matrix, flips,
// screen-over, EXTBG), sprites (the 8 size pairs, priority rotation, the
// 32-sprite / 34-sliver line limits), the two windows and their logic for
// every layer and the colour window, main/sub screens, colour math
// (add/subtract, half, fixed colour), mosaic, brightness and forced blank.
// Hi-res modes 5 and 6 and pseudo-hires show the main screen's pixels only.
// Left out: interlace, mid-line register changes, the overscan (239-line)
// picture.

#include "libdevices/main/io_handler.h"
#include "libdevices/main/ivideo_output.h"

#include <array>
#include <cstdint>
#include <string>

namespace snes {

class Ppu : public IOHandler, public IVideoOutput {
public:
    static constexpr int WIDTH = 256;
    static constexpr int HEIGHT = 224;

    Ppu();
    ~Ppu() override = default;

    /** A write to $2100-$213F (`reg` = address & $3F). */
    void write(uint8_t reg, uint8_t value);
    /** A read of $2134-$213F; others return PPU1 open bus. */
    uint8_t read(uint8_t reg);
    uint8_t peek(uint8_t reg) const;

    /** Draw screen line `line` (1-224). */
    void renderLine(int line);
    /** Vertical blank starts: the frame is complete; OAM address reload. */
    void startVblank();
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
    VideoDimensions getDimensions() const override { return {WIDTH, HEIGHT, WIDTH, HEIGHT}; }
    void renderFrame(uint32_t* buffer) override;

private:
    struct Pixel {
        uint16_t color = 0;     // BGR555
        uint8_t z = 0;          // 0 = transparent
        uint8_t layer = 5;      // 0-3 BG1-4, 4 OBJ, 5 backdrop
        bool objMath = true;    // OBJ palettes 0-3 never take part in colour math
    };
    struct Layer { std::array<uint16_t, 256> color; std::array<uint8_t, 256> prio; std::array<bool, 256> opaque; };

    uint16_t vramAddrRemap() const;
    void vramIncrement(bool high);
    void renderBg(int bg, int bpp, int line, Layer& out);
    void renderMode7(int bg, int line, Layer& out);
    void renderObj(int line, Layer& out, std::array<uint8_t, 256>& pal);
    bool window(int layerSel, int x) const;           // layerSel 0-3 BG, 4 OBJ, 5 colour
    uint16_t paletteColor(int bpp, int bg, int pal, int idx) const;

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

    // Frames (ARGB), double-buffered
    std::array<uint32_t, WIDTH * HEIGHT> m_frame[2];
    int m_back = 0;
    uint64_t m_frames = 0;
};

}  // namespace snes
