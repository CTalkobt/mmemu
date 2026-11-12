#pragma once

// The Game Boy's PPU (the DMG's and the CGB's), after Mesen2
// (Core/Gameboy/GbPpu.cpp; GPL-3.0, Sour and the Mesen contributors): a dot at
// a time, the modes and their STAT interrupt timing (the line-153 quirk, the
// DMG STAT write bug), OAM search, the background/window fetcher with its pixel
// FIFO and the sprite fetches that stall it, VRAM/OAM blocked to the CPU while
// the PPU uses them, the OAM corruption bug, BGP changes during a line, and
// the first frame after the LCD is turned on left blank.
//
// The CGB's (as Mesen): two VRAM banks, the background's attributes (palette,
// bank, flips, priority) from bank 1, objects' palette and bank, objects
// ordered by OAM index, LCDC bit 0 as the objects' master priority, the
// colour palettes (blocked to the CPU in mode 3), the tile fetch glitch, and
// one dot per CPU half-cycle in double speed. In DMG compatibility mode (KEY0)
// the DMG palettes' shades index colour palettes 0 (BG) and 0/1 (objects).
//
// Where c-sp's AGE tests (verified on a CGB E and a DMG C) differ from Mesen,
// they win, with SameBoy's timings (Core/display.c, MIT): the first line after
// the LCD is turned on ends mode 3 a dot sooner; window and X 0 object
// penalties; the DMG's WX 166. On the CGB (a CPU CGB E): the mode 0 interrupt
// and OAM reads a dot after mode 0 (in double speed mode 0 itself a dot
// later), VRAM reads blocked from mode 3 not 4 dots before it, OAM writes
// blocked from a line's start, LY 153 for 8 dots, and LY in double speed 2
// dots into a line.
//
// The output is a shade (0 white - 3 black) per pixel, the palettes already
// applied; the Super Game Boy gets every pixel as it is drawn (SgbLink). The
// CGB's is a 15-bit colour per pixel.

#include <cstdint>

namespace gb {

class Gameboy;

/** Which OAM corruption an access during OAM search causes (Mesen's GbOamCorruptionType). */
enum class OamCorruption { Read, Write, ReadIncDec };

enum class PpuMode : uint8_t { HBlank = 0, VBlank = 1, OamEvaluation = 2, Drawing = 3, NoIrq = 4 };

class Ppu {
public:
    static constexpr int WIDTH = 160, HEIGHT = 144;
    /** VRAM: 8 KB, and the CGB's second bank. */
    static constexpr int VRAM_SIZE = 0x4000;

    void attach(Gameboy* gb) { m_gb = gb; }
    /** As at power on (VRAM and OAM are filled by the owner). */
    void init();
    /** One dot (`single`) or two. */
    void exec(bool single);

    uint8_t read(uint16_t addr) const;
    void write(uint16_t addr, uint8_t v);
    uint8_t readVram(uint16_t addr) const { return m_vramReadBlocked ? 0xFF : m_vram[vramAddr(addr)]; }
    void writeVram(uint16_t addr, uint8_t v) { if (!m_vramWriteBlocked) m_vram[vramAddr(addr)] = v; }
    /** The CGB's VBK and palette registers ($FF4C KEY0, $FF4F, $FF68-$FF6B). */
    uint8_t readCgb(uint16_t addr) const;
    void writeCgb(uint16_t addr, uint8_t v);
    /** LCDC bit 4 cleared as the background fetcher reads a tile (the CGB): its index is read for the data. */
    void setTileFetchGlitch() { m_tileGlitch = true; }
    uint8_t readOam(uint8_t addr) const;
    void writeOam(uint8_t addr, uint8_t v, bool forDma);
    template <OamCorruption C> void oamCorruption(uint16_t addr);
    void setCpuStopState(bool stopped);

    uint8_t* vram() { return m_vram; }
    uint8_t* oam() { return m_oam; }
    const uint8_t* frame() const { return m_front; }
    const uint16_t* colorFrame() const { return m_cfront; }
    /** The CGB's colour palettes: 8 of 4 colours each, 15-bit BGR. */
    uint16_t* bgPalettes() { return m_bgPal; }
    uint16_t* objPalettes() { return m_objPal; }
    /** The CGB's features on (a CGB in CGB mode; KEY0 turns them off for a DMG cartridge). */
    bool cgbEnabled() const { return m_cgbEnabled; }
    void setCgbEnabled(bool on) { m_cgbEnabled = on; }
    uint8_t vramBank() const { return m_vramBank; }
    uint8_t control() const { return m_control; }
    uint32_t frameCount() const { return m_frameCount; }
    uint8_t scanline() const { return m_scanline; }
    uint16_t cycle() const { return m_cycle; }
    PpuMode mode() const { return m_mode; }
    bool lcdOn() const { return m_lcdEnabled; }

private:
    struct FifoEntry { uint8_t color = 0, attributes = 0, index = 0; };
    struct Fifo {
        uint8_t position = 0, size = 0;
        FifoEntry content[8];
        void reset() { size = 0; position = 0; for (auto& c : content) c = FifoEntry{}; }
        void pop() { content[position].color = 0; position = (position + 1) & 7; --size; }
    };
    struct Fetcher { uint16_t addr = 0; uint8_t attributes = 0, step = 0, low = 0, high = 0; };

    void execCycle();
    void vblankScanline();
    void firstScanlineAfterPowerOn();
    void visibleScanline();
    void drawCycle();
    /** Mode 0: its interrupt, VRAM and OAM free, nothing more to do this line. */
    void startHblank();
    void spriteEvaluation();
    void resetRenderer();
    void clockSpriteFetcher();
    void findNextSprite();
    void clockTileFetcher();
    void pushSpriteToFifo();
    void pushTileToFifo();
    void updateStatIrq();
    /** A pixel: `index` is the shade (DMG), or palette * 4 + colour (CGB); objects' in the object palettes. */
    void writeBgPixel(uint8_t index);
    void writeObjPixel(uint8_t index);
    void writePixel(uint8_t index, uint16_t color);
    void sendFrame();
    void setMode(PpuMode m);
    /** The CGB in double speed shows a new line's LY 2 dots in (AGE's ly). */
    bool lyLate() const;
    void oamWriteBlockAtLineStart();
    uint16_t vramAddr(uint16_t addr) const { return (uint16_t)((m_vramBank << 13) | (addr & 0x1FFF)); }
    uint8_t lcdReadOam(uint8_t a) const { return m_stopOamBlocked ? 0xFF : m_oam[a]; }
    uint8_t lcdReadVram(uint16_t a) const { return m_stopVramBlocked ? 0xFF : m_vram[a & 0x3FFF]; }
    uint8_t readCgbPalette(uint8_t pos, const uint16_t* pal) const;
    void writeCgbPalette(uint8_t& pos, uint16_t* pal, bool autoInc, uint8_t v);
    void oamIncDecCorruption(int row);

    Gameboy* m_gb = nullptr;
    bool m_cgb = false;                // the model (the CGB's timings and quirks)
    uint8_t m_vram[VRAM_SIZE] = {};
    uint8_t m_oam[0xA0] = {};
    uint8_t m_buf[2][WIDTH * HEIGHT] = {};
    uint8_t* m_cur = m_buf[0];
    uint8_t* m_front = m_buf[1];
    uint16_t m_cbuf[2][WIDTH * HEIGHT] = {};
    uint16_t* m_ccur = m_cbuf[0];
    uint16_t* m_cfront = m_cbuf[1];
    uint32_t m_frameCount = 0;
    uint64_t m_lastFrameTime = 0;

    // Mesen's GbPpuState
    uint8_t m_scanline = 0;
    uint16_t m_cycle = 0, m_idleCycles = 0;
    PpuMode m_mode = PpuMode::HBlank, m_irqMode = PpuMode::HBlank;
    bool m_statIrqFlag = false;
    uint8_t m_ly = 0;
    int16_t m_lyForCompare = 0;
    uint8_t m_lyCompare = 0;
    bool m_lyCoincidence = false;
    uint8_t m_bgp = 0, m_obp0 = 0xFF, m_obp1 = 0xFF, m_scx = 0, m_scy = 0, m_wx = 0, m_wy = 0;
    uint8_t m_control = 0;
    bool m_lcdEnabled = false, m_windowTilemap = false, m_windowEnabled = false, m_bgTileSelect = false;
    bool m_bgTilemap = false, m_largeSprites = false, m_spritesEnabled = false, m_bgEnabled = false;
    uint8_t m_status = 0;

    Fifo m_bgFifo, m_oamFifo;
    Fetcher m_bgFetcher, m_oamFetcher;
    int16_t m_drawnPixels = 0;
    uint8_t m_fetchColumn = 0;
    bool m_fetchWindow = false;
    int16_t m_windowCounter = -1;
    bool m_wyEnable = false, m_wxEnable = false, m_insertGlitchBgPixel = false;
    int16_t m_fetchSprite = -1;
    uint8_t m_spriteCount = 0;
    uint8_t m_spriteX[10] = {}, m_spriteY[10] = {}, m_spriteIndex[10] = {};
    uint8_t m_oamReadBuffer[2] = {};
    bool m_lcdDisabled = true;
    bool m_stopOamBlocked = false, m_stopVramBlocked = false;
    bool m_oamReadBlocked = false, m_oamWriteBlocked = false, m_vramReadBlocked = false, m_vramWriteBlocked = false;
    bool m_isFirstFrame = true, m_forceBlankFrame = true, m_rendererIdle = false;
    bool m_hblankPending = false;      // the last pixel out: the rest of mode 0's start next dot
    uint8_t m_tileIndex = 0;
    bool m_lastPixelBg = true;
    uint8_t m_lastBgColor = 0;

    // The CGB's
    bool m_cgbEnabled = false, m_tileGlitch = false, m_stopPaletteBlocked = false;
    uint8_t m_vramBank = 0;
    uint16_t m_bgPal[32] = {}, m_objPal[32] = {};
    uint8_t m_bgPalPos = 0, m_objPalPos = 0;
    bool m_bgPalAutoInc = false, m_objPalAutoInc = false;
};

}  // namespace gb
