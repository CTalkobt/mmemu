#pragma once
#include "sdcard_spi.h"
#include <functional>

// VERA (Versatile Embedded Retro Adapter), the Commander X16's video and
// audio chip.
//
// Ported from the Commander X16 emulator (x16-emulator, src/video.c,
// src/vera_spi.c, src/audio.c), Copyright (c) 2019 Michael Steil,
// Copyright (c) 2020 Frank van den Hoef and contributors. All rights
// reserved. License: 2-clause BSD. The register behaviour, rendering and FX
// follow that emulator, which is what the X16 ROM is tested against; where
// it disagrees with the VERA Programmer's Reference, a comment says so.

#include "libdevices/main/io_handler.h"
#include "libdevices/main/ivideo_output.h"
#include "libdevices/main/iaudio_output.h"
#include "libdevices/main/isignal_line.h"
#include "vera_psg.h"
#include "vera_pcm.h"
#include <cstdint>
#include <string>
#include <vector>

/**
 * VERA at $9F20-$9F3F.
 *
 *  - 128 KB of VRAM through two address ports (ADDR0/ADDR1, DATA0/DATA1)
 *    with the increment/decrement table, ADDRSEL and DCSEL
 *  - Display composer (DCSEL 0/1): DC_VIDEO, HSCALE/VSCALE, border,
 *    HSTART/HSTOP/VSTART/VSTOP; VGA, NTSC composite (interlaced, with the
 *    field bit) and 240p/RGB timings as in the emulator
 *  - Two layers (text 1bpp with 16 or 256 colours, tiles 2/4/8bpp with flips
 *    and palette offsets, bitmaps), 128 sprites with Z depth, flips, palette
 *    offsets, collisions and the per-line sprite budget
 *  - Palette at $1FA00, sprite attributes at $1FC00, PSG at $1F9C0
 *  - IRQs: VSYNC, LINE, SPRCOL and AFLOW, driving the IRQ line as
 *    (ISR & IEN) != 0
 *  - VERA FX (DCSEL 2-6): address modes (line, polygon fill, affine), 4-bit
 *    mode, 16-bit hop, the 32-bit cache (fill, write, byte cycling, nibble
 *    masks, transparent writes), the multiplier/accumulator, 2-bit polygon
 *  - DCSEL 63: the version string ('V', 47, 0, 2)
 *  - PSG (16 voices) and PCM (FIFO) at 25 MHz / 512 = 48828.125 Hz
 *  - SPI ($9F3E/$9F3F) with no SD card attached (reads $FF)
 *
 * tick() takes CPU cycles at setCpuClockHz() (8 MHz by default).
 */
class Vera : public IOHandler, public IVideoOutput, public IAudioOutput {
public:
    static constexpr int SCREEN_WIDTH  = 640;
    static constexpr int SCREEN_HEIGHT = 480;
    static constexpr int SCAN_HEIGHT   = 525;    ///< both VGA and NTSC
    static constexpr int VGA_SCAN_WIDTH = 800;   ///< pixel clocks per VGA line
    static constexpr int NTSC_HALF_SCAN_WIDTH = 794;
    static constexpr uint32_t PIXEL_CLOCK_HZ = 25000000;
    static constexpr int AUDIO_RATE = 48828;     ///< 25 MHz / 512 (48828.125 Hz)
    static constexpr int NUM_SPRITES = 128;

    static constexpr uint32_t ADDR_PSG_START     = 0x1F9C0;
    static constexpr uint32_t ADDR_PSG_END       = 0x1FA00;
    static constexpr uint32_t ADDR_PALETTE_START = 0x1FA00;
    static constexpr uint32_t ADDR_PALETTE_END   = 0x1FC00;
    static constexpr uint32_t ADDR_SPRDATA_START = 0x1FC00;
    static constexpr uint32_t ADDR_SPRDATA_END   = 0x20000;

    /** Register offsets from $9F20. */
    enum Reg {
        ADDRx_L = 0x00, ADDRx_M = 0x01, ADDRx_H = 0x02,
        DATA0 = 0x03, DATA1 = 0x04, CTRL = 0x05,
        IEN = 0x06, ISR = 0x07, IRQLINE_L = 0x08,   // read: SCANLINE_L
        DC0 = 0x09, DC1 = 0x0A, DC2 = 0x0B, DC3 = 0x0C,
        L0_CONFIG = 0x0D, L0_MAPBASE, L0_TILEBASE, L0_HSCROLL_L, L0_HSCROLL_H, L0_VSCROLL_L, L0_VSCROLL_H,
        L1_CONFIG = 0x14, L1_MAPBASE, L1_TILEBASE, L1_HSCROLL_L, L1_HSCROLL_H, L1_VSCROLL_L, L1_VSCROLL_H,
        AUDIO_CTRL = 0x1B, AUDIO_RATE_REG = 0x1C, AUDIO_DATA = 0x1D,
        SPI_DATA = 0x1E, SPI_CTRL = 0x1F
    };

    // ISR / IEN bits
    static constexpr uint8_t INT_VSYNC  = 0x01;
    static constexpr uint8_t INT_LINE   = 0x02;
    static constexpr uint8_t INT_SPRCOL = 0x04;
    static constexpr uint8_t INT_AFLOW  = 0x08;

    Vera();
    ~Vera() override;

    // Configuration
    /** CPU clock that tick() counts (default 8 MHz). */
    void setCpuClockHz(uint32_t hz) { if (hz) m_cpuHz = hz; }
    void setClockHz(uint32_t hz) override { setCpuClockHz(hz); }
    uint32_t cpuClockHz() const { return m_cpuHz; }
    void setIrqLine(ISignalLine* line) override { m_irqLine = line; m_irqOut = false; updateIrq(); }
    /** Render the partial line before DATA0/DATA1 writes (the emulator's -midline-effects). */
    void setMidlineDataWrites(bool on) { m_midlineDataWrites = on; }

    /**
     * Another sound source mixed into VERA's output before the limiter, as
     * the X16 mixes the YM2151 (x16-emulator audio.c: (psg + pcm) * 2 + ym).
     * Called once per output sample (AUDIO_RATE); adds to left/right
     * (int16 scale).
     */
    void setExternalAudio(std::function<void(int32_t& left, int32_t& right)> fn) { m_externalAudio = std::move(fn); }

    // IOHandler
    const char* name()     const override { return m_name.c_str(); }
    void        setName(const std::string& name) override { m_name = name; }
    uint32_t    baseAddr() const override { return m_baseAddr; }
    void        setBaseAddr(uint32_t a) override { m_baseAddr = a; }
    uint32_t    addrMask() const override { return 0x001F; }
    bool ioRead (IBus* bus, uint32_t addr, uint8_t* val) override;
    bool ioPeek (IBus* bus, uint32_t addr, uint8_t* val) override;
    bool ioWrite(IBus* bus, uint32_t addr, uint8_t  val) override;
    /** Power-on: video_reset() plus the SPI interface. */
    void reset() override;
    void tick(uint64_t cycles) override;
    void getDeviceInfo(DeviceInfo& out) const override;
    std::vector<std::pair<std::string, uint32_t>> getDerivedValues() const override;
    std::vector<std::string> deviceAliases() const override { return {"VERA"}; }

    /** The SD card on VERA's SPI port, as disk unit 8: a raw image (e.g. FAT32). */
    bool mountDisk(int unit, const std::string& path) override;
    void ejectDisk(int unit) override;
    bool getDiskStatus(int unit, int& track, int& sector, bool& led) override;
    std::string getMountedDiskPath(int unit) override;
    SdCardSpi& sdCard() { return m_sd; }

    // IVideoOutput: the last completed 640x480 frame
    VideoDimensions getDimensions() const override {
        return {SCREEN_WIDTH, SCREEN_HEIGHT, SCREEN_WIDTH, SCREEN_HEIGHT};
    }
    void renderFrame(uint32_t* buffer) override;

    // IAudioOutput: PSG + PCM, mono
    int nativeSampleRate() const override { return AUDIO_RATE; }
    int pullSamples(float* buffer, int maxSamples) override;
    /** Interleaved left/right frames; consumes the same buffer as pullSamples. */
    int pullStereoSamples(float* lr, int maxFrames);
    int samplesAvailable() const { return m_audioCount; }

    // State access for tests and the debugger
    uint8_t  vramPeek(uint32_t addr) const { return m_vram[addr & 0x1FFFF]; }
    /** A VRAM write as the chip does it (updates the PSG, palette and sprites). */
    void     vramPoke(uint32_t addr, uint8_t val) { videoSpaceWrite(addr & 0x1FFFF, val); }
    uint16_t scanline() const;          ///< as SCANLINE reads (0-511)
    uint16_t rawScanline() const { return m_vgaScanPosY; }   ///< VGA line 0-524
    uint8_t  isr() const;               ///< ISR incl. AFLOW
    uint8_t  ien() const { return m_ien; }
    uint16_t irqLineCompare() const { return m_irqLine9; }
    bool     irqAsserted() const;
    uint32_t address(int sel) const { return m_ioAddr[sel & 1] & 0x1FFFF; }
    uint8_t  dcsel() const { return m_ioDcsel; }
    uint8_t  addrsel() const { return m_ioAddrsel; }
    uint8_t  dcVideo() const { return m_regComposer[0]; }
    uint8_t  composerReg(int i) const { return m_regComposer[i & 0xFF]; }
    uint8_t  layerReg(int layer, int i) const { return m_regLayer[layer & 1][i % 7]; }
    int      currentField() const { return (m_regComposer[0] >> 7) & 1; }
    uint64_t frameCount() const { return m_frameCount; }
    /** Palette entry as 0x00RRGGBB, after DC_VIDEO's output mode/chroma. */
    uint32_t paletteRgb(int i);
    uint8_t  fxCache(int i) const { return m_fxCache[i & 3]; }
    int32_t  fxAccumulator() const { return m_fxMultAccumulator; }
    const VeraPsg& psg() const { return m_psg; }
    const VeraPcm& pcm() const { return m_pcm; }

private:
    struct LayerProps {
        uint8_t  color_depth = 0;
        uint32_t map_base = 0;
        uint32_t tile_base = 0;
        bool text_mode = false, text_mode_256c = false, tile_mode = false, bitmap_mode = false;
        uint16_t hscroll = 0, vscroll = 0;
        uint8_t  mapw_log2 = 0, maph_log2 = 0;
        uint16_t tilew = 0, tileh = 0;
        uint8_t  tilew_log2 = 0, tileh_log2 = 0;
        uint16_t mapw_max = 0, maph_max = 0, tilew_max = 0, tileh_max = 0;
        uint16_t layerw_max = 0, layerh_max = 0;
        uint8_t  tile_size_log2 = 0;
        int min_eff_x = 0, max_eff_x = 0;
        uint8_t bits_per_pixel = 0, first_color_pos = 0, color_mask = 0, color_fields_max = 0;
    };
    struct SpriteProps {
        int8_t   zdepth = 0;
        uint8_t  collision_mask = 0;
        int16_t  x = 0, y = 0;
        uint8_t  width_log2 = 3, height_log2 = 3;
        uint8_t  width = 8, height = 8;
        bool     hflip = false, vflip = false;
        uint8_t  color_mode = 0;
        uint32_t address = 0;
        uint16_t palette_offset = 0;
    };

    // Registers
    uint8_t  readReg(uint8_t reg, bool sideEffects);
    void     writeReg(uint8_t reg, uint8_t value);
    uint8_t  dcValue(uint8_t i) const;
    void     videoReset();

    // VRAM
    uint8_t  videoSpaceRead(uint32_t address) const { return m_vram[address & 0x1FFFF]; }
    void     videoSpaceReadRange(uint8_t* dest, uint32_t address, uint32_t size) const;
    void     videoSpaceWrite(uint32_t address, uint8_t value);
    void     fxVideoSpaceWrite(uint32_t address, bool nibble, uint8_t value);
    void     fxVramCacheWrite(uint32_t address, uint8_t value, uint8_t mask);
    void     specialWrite(uint32_t address, uint8_t value);
    uint32_t getAndIncAddress(uint8_t sel, bool write);
    void     fxAffinePrefetch();
    int32_t  fxMultiply() const;

    // Rendering
    void refreshLayerProperties(uint8_t layer);
    void refreshSpriteProperties(uint16_t sprite);
    void refreshPalette();
    void renderSpriteLine(uint16_t y);
    void renderLayerLineText(uint8_t layer, uint16_t y);
    void renderLayerLineTile(uint8_t layer, uint16_t y);
    void renderLayerLineBitmap(uint8_t layer, uint16_t y);
    void renderLine(uint16_t y, double scanPosX);
    void updateIsrAndColl(uint16_t y, uint16_t compare);
    void videoStep(uint64_t cycles);
    void midline();
    void completeFrame();

    // Audio / SPI / IRQ
    void audioStep(uint64_t cycles);
    void renderAudioSample();
    void spiStep(uint64_t cycles);
    uint8_t spiRead(uint8_t reg, bool sideEffects);
    void spiWrite(uint8_t reg, uint8_t value);
    void updateIrq();

    std::string  m_name = "VERA";
    uint32_t     m_baseAddr = 0x9F20;
    ISignalLine* m_irqLine = nullptr;
    bool         m_irqOut = false;
    uint32_t     m_cpuHz = 8000000;
    bool         m_midlineDataWrites = false;

    uint8_t  m_vram[0x20000];
    uint8_t  m_palette[256 * 2];
    uint8_t  m_spriteData[NUM_SPRITES][8];

    // I/O registers
    uint32_t m_ioAddr[2] = {0, 0};
    uint8_t  m_ioRddata[2] = {0, 0};
    uint8_t  m_ioInc[2] = {0, 0};
    uint8_t  m_ioAddrsel = 0;
    uint8_t  m_ioDcsel = 0;
    uint8_t  m_ien = 0;
    uint8_t  m_isr = 0;
    uint16_t m_irqLine9 = 0;

    uint8_t  m_regLayer[2][7];
    static constexpr int COMPOSER_SLOTS = 4 * 64;
    uint8_t  m_regComposer[COMPOSER_SLOTS];
    uint8_t  m_prevRegComposer[2][COMPOSER_SLOTS];

    uint8_t  m_layerLine[2][SCREEN_WIDTH];
    uint8_t  m_spriteLineCol[SCREEN_WIDTH];
    uint8_t  m_spriteLineZ[SCREEN_WIDTH];
    uint8_t  m_spriteLineMask[SCREEN_WIDTH];
    uint8_t  m_spriteLineCollisions = 0;
    bool     m_layerLineEnable[2] = {false, false};
    bool     m_oldLayerLineEnable[2] = {false, false};
    bool     m_spriteLineEnable = false;
    bool     m_oldSpriteLineEnable = false;

    LayerProps  m_layerProps[2];
    LayerProps  m_prevLayerProps[2][2];
    SpriteProps m_spriteProps[NUM_SPRITES];

    uint32_t m_paletteEntries[256];
    bool     m_paletteDirty = true;

    // render_line's statics
    uint16_t m_yPrev = 0;
    uint16_t m_sPosXPrev = 0;
    uint32_t m_effYFp = 0;   // 16.16
    uint32_t m_effXFp = 0;   // 16.16
    uint8_t  m_colLine[SCREEN_WIDTH];

    // FX (16.16 fixed point positions/increments, as in the emulator)
    uint8_t  m_fxAddr1Mode = 0;
    uint32_t m_fxXPixelIncrement = 0, m_fxYPixelIncrement = 0;
    uint32_t m_fxXPixelPosition = 0, m_fxYPixelPosition = 0;
    uint16_t m_fxPolyFillLength = 0;
    uint32_t m_fxAffineTileBase = 0, m_fxAffineMapBase = 0;
    uint8_t  m_fxAffineMapSize = 2;
    bool m_fx4bitMode = false, m_fx16bitHop = false, m_fxCacheByteCycling = false;
    bool m_fxCacheFill = false, m_fxCacheWrite = false, m_fxTransWrites = false;
    bool m_fx2bitPoly = false, m_fx2bitPoking = false;
    bool m_fxCacheIncrementMode = false, m_fxCacheNibbleIndex = false;
    uint8_t m_fxCacheByteIndex = 0;
    bool m_fxMultiplier = false, m_fxSubtract = false, m_fxAffineClip = false;
    uint8_t m_fx16bitHopAlign = 0;
    bool m_fxNibbleBit[2] = {false, false};
    bool m_fxNibbleIncr[2] = {false, false};
    uint8_t m_fxCache[4] = {0, 0, 0, 0};
    int32_t m_fxMultAccumulator = 0;

    // Scan position: pixel clocks * m_cpuHz (integer form of the emulator's floats)
    uint64_t m_vgaScanPosX = 0;
    uint16_t m_vgaScanPosY = 0;
    uint64_t m_ntscHalfCnt = 0;
    uint16_t m_ntscScanPosY = 0;
    uint64_t m_frameCount = 0;

    std::vector<uint32_t> m_framebuffer;   // being drawn (0x00RRGGBB)
    std::vector<uint32_t> m_completed;     // last full frame (0xFFRRGGBB)

    // Audio
    VeraPsg  m_psg;
    VeraPcm  m_pcm;
    uint64_t m_audioAcc = 0;               // 25 MHz pixel clocks * m_cpuHz
    uint32_t m_limiterAmp = 1 << 16;
    static constexpr int AUDIO_BUF = 8192; // stereo frames
    float    m_audioBuf[AUDIO_BUF * 2];
    int      m_audioRead = 0, m_audioWrite = 0, m_audioCount = 0;

    // SPI (no SD card)
    bool    m_spiSs = false, m_spiBusy = false, m_spiAutotx = false;
    uint8_t m_spiSendingByte = 0, m_spiReceivedByte = 0xFF;
    double  m_spiOutcounter = 0.0;
    std::function<void(int32_t&, int32_t&)> m_externalAudio;
    SdCardSpi m_sd;
};
