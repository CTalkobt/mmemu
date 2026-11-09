#pragma once

// NES / Famicom PPU, the Ricoh 2C02 (NTSC).
//
// After Mesen2 (Core/NES/NesPpu.cpp, BaseNesPpu.cpp, DefaultNesPpu.h,
// NesDefaultVideoFilter.cpp), GPL-3.0, Sour and the Mesen contributors, and
// the nesdev wiki ("PPU rendering", "PPU scrolling", "PPU sprite
// evaluation"). Left out: OAM decay and row corruption, the $2000/$2005/$2006
// scroll glitches, open-bus decay, the 2C03/2C04/2C05 RGB PPUs, PAL/Dendy
// timing, HD packs and the debugger hooks.

#include "libdevices/main/io_handler.h"
#include "libdevices/main/ivideo_output.h"

#include <array>
#include <cstdint>
#include <string>

namespace nes {

class Cartridge;

/**
 * The PPU. The machine maps $2000-$3FFF (mirrored every 8 bytes) to read() /
 * write() and clocks it with clock(), three dots per CPU cycle. The pattern
 * tables and nametables come from the cartridge (its CHR memory, nametable
 * mirroring and the A12 line MMC3 counts); the palette RAM and OAM are here.
 *
 * Timing (a frame of 262 lines of 341 dots): line -1 is the pre-render line
 * (one dot shorter on odd frames when rendering), 0-239 are visible, 241
 * dot 1 sets the vertical blank flag (and /NMI with $2000 bit 7), cleared at
 * line -1 dot 1. The completed frame (256x240) is double-buffered.
 */
class Ppu : public IOHandler, public IVideoOutput {
public:
    static constexpr int WIDTH = 256;
    static constexpr int HEIGHT = 240;
    static constexpr int DOTS_PER_LINE = 341;
    static constexpr int LINES_PER_FRAME = 262;

    Ppu();
    ~Ppu() override = default;

    void setCartridge(Cartridge* c) { m_cart = c; }

    // ------------------------------------------------------------------
    // Machine interface
    // ------------------------------------------------------------------
    /** Run `dots` PPU cycles. */
    void clock(uint32_t dots);
    /** A CPU read of $2000-$2007 (`reg` 0-7). */
    uint8_t read(uint8_t reg);
    /** read() without side effects, for the debugger. */
    uint8_t peek(uint8_t reg) const;
    /** A CPU write of $2000-$2007 (`reg` 0-7). */
    void write(uint8_t reg, uint8_t value);
    /** OAM DMA ($4014) writes each byte through $2004. */
    void oamDmaWrite(uint8_t value) { write(4, value); }

    /** /NMI output: the vertical blank flag and $2000 bit 7. */
    bool nmi() const { return m_vblank && m_nmiEnable; }

    // Beam position and state, for the debugger and tests
    int scanline() const { return m_scanline; }
    int dot() const { return m_cycle; }
    uint64_t frameCount() const { return m_frameCount; }
    uint16_t vramAddr() const { return m_v; }
    uint16_t tempAddr() const { return m_t; }
    uint8_t fineX() const { return m_fineX; }
    bool renderingEnabled() const { return m_bgEnabled || m_spritesEnabled; }
    uint64_t dots() const { return m_dots; }
    uint8_t oam(uint8_t i) const { return m_oam[i]; }
    uint8_t paletteRam(uint8_t i) const { return readPalette(i); }
    /** The PPU address space ($0000-$3FFF) without side effects. */
    uint8_t peekVram(uint16_t addr) const;
    /** Write the PPU address space ($0000-$3FFF) directly (debugger, tests). */
    void pokeVram(uint16_t addr, uint8_t value);

    /** ARGB of NES colour `c` (0-$3F) with emphasis bits `emph` ($2001 bits 5-7 >> 5). */
    static uint32_t color(uint8_t c, uint8_t emph = 0);

    // ------------------------------------------------------------------
    // IOHandler
    // ------------------------------------------------------------------
    const char* name() const override { return m_name.c_str(); }
    void setName(const std::string& n) override { m_name = n; }
    uint32_t baseAddr() const override { return 0x2000; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    /** Power-on state. */
    void reset() override;
    /** The RESET button: $2000/$2001, the write toggle and the read buffer clear; OAM, palette and VRAM stay. */
    void softReset();
    /** Does nothing: the machine clocks the PPU with clock(). */
    void tick(uint64_t) override {}

    // ------------------------------------------------------------------
    // IVideoOutput
    // ------------------------------------------------------------------
    VideoDimensions getDimensions() const override { return {WIDTH, HEIGHT, WIDTH, HEIGHT}; }
    void renderFrame(uint32_t* buffer) override;

private:
    struct Sprite {
        uint8_t low = 0, high = 0, x = 0, palette = 0;
        bool behind = false, hflip = false;
    };

    void exec();
    void firstDot();
    void renderDot();
    void loadTile();
    void evaluateSprites();
    /** The dot of this line where the sprite overflow flag gets set, or -1. */
    int overflowDot() const;
    void fetchSprite(int i);
    uint8_t pixel();
    void incX();
    void incY();
    void vramIncrement();
    uint8_t readVram(uint16_t addr);
    void writeVram(uint16_t addr, uint8_t value);
    void setBusAddress(uint16_t addr);
    /** Mesen SetOpenBus: drive the `mask` bits to `value`, decay the others. */
    void setOpenBus(uint8_t mask, uint8_t value);
    uint8_t readPalette(uint8_t addr) const;
    void writePalette(uint8_t addr, uint8_t value);

    std::string m_name = "ppu";
    Cartridge* m_cart = nullptr;

    // Registers
    uint16_t m_v = 0, m_t = 0;           // loopy v and t
    uint8_t m_fineX = 0;
    bool m_w = false;                    // the $2005/$2006 write toggle
    uint8_t m_openBus = 0;
    uint64_t m_openBusStamp[8] = {};     // frame each bit was last driven (it decays after 3)
    uint8_t m_readBuffer = 0;
    uint8_t m_oamAddr = 0;
    // $2000
    bool m_vramInc32 = false, m_sprite16 = false, m_nmiEnable = false;
    uint16_t m_spritePatterns = 0, m_bgPatterns = 0;
    // $2001
    uint8_t m_mask = 0;
    bool m_grayscale = false, m_bgLeft = false, m_spritesLeft = false, m_bgEnabled = false, m_spritesEnabled = false;
    // $2002
    bool m_vblank = false, m_sprite0Hit = false, m_overflow = false;
    bool m_preventVblank = false;

    // Rendering enable takes effect a dot late, and another dot later for the
    // scroll increments (Mesen _renderingEnabled / _prevRenderingEnabled)
    bool m_renderingNow = false;
    bool m_renderingPrev = false;
    // The v increment after a $2007 access happens on the next dot
    bool m_pendingIncrement = false;
    int m_ignoreVramRead = 0;
    int m_overflowAt = -1;               // the dot the overflow flag is set on this line            // dots during which a $2007 read is ignored
    // $2006's second write reaches v 3 dots later
    int m_vDelay = 0;
    uint16_t m_vPending = 0;

    // Timing
    int m_scanline = -1;
    int m_cycle = 0;
    uint64_t m_frameCount = 0;

    // Background pipeline
    struct {
        uint16_t addr = 0;
        uint8_t palette = 0, low = 0, high = 0;
    } m_tile;
    uint16_t m_lowShift = 0, m_highShift = 0;
    uint8_t m_curPalette = 0, m_prevPalette = 0;
    uint8_t m_ntLatch = 0;

    // Sprites
    std::array<uint8_t, 256> m_oam{};
    std::array<uint8_t, 32> m_secondary{};
    std::array<Sprite, 8> m_sprites{};
    int m_spriteCount = 0;             // in secondary OAM for the next line
    int m_spriteIndex = 0;             // the next sprite fetch (dots 257-320)
    int m_lineSprites = 0;             // loaded for the current line
    bool m_sprite0Next = false;        // sprite 0 in secondary OAM
    bool m_sprite0Line = false;        // ... for the current line
    std::array<bool, 258> m_hasSprite{};

    std::array<uint8_t, 32> m_palette{};

    // Frame buffers of (emphasis << 6 | colour)
    std::array<uint16_t, WIDTH * HEIGHT> m_buffers[2];
    int m_back = 0, m_front = 1;
    uint64_t m_dots = 0;               // dots since reset (MMC3's A12 filter)
};

}  // namespace nes
