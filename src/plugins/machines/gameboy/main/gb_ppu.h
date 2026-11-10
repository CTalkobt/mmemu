#pragma once

// The Game Boy's PPU (the DMG's), after Mesen2 (Core/Gameboy/GbPpu.cpp;
// GPL-3.0, Sour and the Mesen contributors): a dot at a time, the modes and
// their STAT interrupt timing (the line-153 quirk, the DMG STAT write bug),
// OAM search, the background/window fetcher with its pixel FIFO and the
// sprite fetches that stall it, VRAM/OAM blocked to the CPU while the PPU
// uses them, the OAM corruption bug, BGP changes during a line, and the
// first frame after the LCD is turned on left blank.
//
// The output is a shade (0 white - 3 black) per pixel, the palettes already
// applied; the Super Game Boy gets every pixel as it is drawn (SgbLink).

#include <cstdint>

namespace gb {

class Gameboy;

/** Which OAM corruption an access during OAM search causes (Mesen's GbOamCorruptionType). */
enum class OamCorruption { Read, Write, ReadIncDec };

enum class PpuMode : uint8_t { HBlank = 0, VBlank = 1, OamEvaluation = 2, Drawing = 3, NoIrq = 4 };

class Ppu {
public:
    static constexpr int WIDTH = 160, HEIGHT = 144;

    void attach(Gameboy* gb) { m_gb = gb; }
    /** As at power on (VRAM and OAM are filled by the owner). */
    void init();
    /** One dot (`single`) or two. */
    void exec(bool single);

    uint8_t read(uint16_t addr) const;
    void write(uint16_t addr, uint8_t v);
    uint8_t readVram(uint16_t addr) const { return m_vramReadBlocked ? 0xFF : m_vram[addr & 0x1FFF]; }
    void writeVram(uint16_t addr, uint8_t v) { if (!m_vramWriteBlocked) m_vram[addr & 0x1FFF] = v; }
    uint8_t readOam(uint8_t addr) const;
    void writeOam(uint8_t addr, uint8_t v, bool forDma);
    template <OamCorruption C> void oamCorruption(uint16_t addr);
    void setCpuStopState(bool stopped);

    uint8_t* vram() { return m_vram; }
    uint8_t* oam() { return m_oam; }
    const uint8_t* frame() const { return m_front; }
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
    void spriteEvaluation();
    void resetRenderer();
    void clockSpriteFetcher();
    void findNextSprite();
    void clockTileFetcher();
    void pushSpriteToFifo();
    void pushTileToFifo();
    void updateStatIrq();
    void writePixel(uint8_t shade);
    void sendFrame();
    void setMode(PpuMode m) { m_mode = m; }
    uint8_t lcdReadOam(uint8_t a) const { return m_stopOamBlocked ? 0xFF : m_oam[a]; }
    uint8_t lcdReadVram(uint16_t a) const { return m_stopVramBlocked ? 0xFF : m_vram[a & 0x1FFF]; }
    void oamIncDecCorruption(int row);

    Gameboy* m_gb = nullptr;
    uint8_t m_vram[0x2000] = {};
    uint8_t m_oam[0xA0] = {};
    uint8_t m_buf[2][WIDTH * HEIGHT] = {};
    uint8_t* m_cur = m_buf[0];
    uint8_t* m_front = m_buf[1];
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
    uint8_t m_tileIndex = 0;
    bool m_lastPixelBg = true;
    uint8_t m_lastBgColor = 0;
};

}  // namespace gb
