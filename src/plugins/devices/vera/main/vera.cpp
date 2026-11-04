// VERA video and audio for the Commander X16.
//
// Ported from the Commander X16 emulator (x16-emulator: src/video.c,
// src/vera_spi.c and the VERA part of src/audio.c).
//   Copyright (c) 2019 Michael Steil
//   Copyright (c) 2020 Frank van den Hoef
//   and contributors. All rights reserved. License: 2-clause BSD.
//
// The structure follows video.c closely (the function names in comments are
// the emulator's) so that changes there can be carried over. Differences:
//  - Time is integer: scan positions are pixel clocks scaled by the CPU
//    clock instead of floats; tick() may cover several lines at once.
//  - The frame shown by renderFrame() is the last completed one.
//  - VRAM is filled with a fixed pseudo-random pattern at reset (the
//    emulator uses rand()), so runs are repeatable.
//  - ADDR0/ADDR1 are kept to 17 bits after incrementing; the emulator lets
//    the 32-bit value run past $1FFFF / below 0, which only shows in ADDRx_H
//    read back (the hardware has a 17-bit address).
//  - ioPeek() returns what a read would return, without its side effects;
//    the emulator's debugger read (debugOn) instead shows the hidden FX and
//    composer values for write-only registers. Those are in getDeviceInfo().
//  - The reset through CTRL bit 7 also refreshes the layer and sprite
//    property caches (the emulator leaves them stale until the next write).
//  - No warp-mode frame skipping, activity LED, GIF/screenshot, or logging.
//  - The emulator's YM2151 and MIDI mixing (audio.c) belongs to the machine;
//    here only PSG + PCM are mixed, as (psg + pcm) * 2 with its limiter.

#include "vera.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <climits>
#include <cstdlib>

namespace {

constexpr uint8_t VERA_VERSION_MAJOR = 47;
constexpr uint8_t VERA_VERSION_MINOR = 0;
constexpr uint8_t VERA_VERSION_PATCH = 2;
const uint8_t VERA_VERSION_STRING[4] = {'V', VERA_VERSION_MAJOR, VERA_VERSION_MINOR, VERA_VERSION_PATCH};

// NTSC: 262.5 lines per field, lower field first
constexpr int NTSC_Y_OFFSET_LOW  = 42;
constexpr int NTSC_Y_OFFSET_HIGH = 568;
constexpr double TITLE_SAFE_X = 0.067;
constexpr double TITLE_SAFE_Y = 0.05;

constexpr double SPI_CLOCK_RATE_MHZ = 12.5;

const uint16_t DEFAULT_PALETTE[256] = {
0x000,0xfff,0x800,0xafe,0xc4c,0x0c5,0x00a,0xee7,0xd85,0x640,0xf77,0x333,0x777,0xaf6,0x08f,0xbbb,0x000,0x111,0x222,0x333,0x444,0x555,0x666,0x777,0x888,0x999,0xaaa,0xbbb,0xccc,0xddd,0xeee,0xfff,0x211,0x433,0x644,0x866,0xa88,0xc99,0xfbb,0x211,0x422,0x633,0x844,0xa55,0xc66,0xf77,0x200,0x411,0x611,0x822,0xa22,0xc33,0xf33,0x200,0x400,0x600,0x800,0xa00,0xc00,0xf00,0x221,0x443,0x664,0x886,0xaa8,0xcc9,0xfeb,0x211,0x432,0x653,0x874,0xa95,0xcb6,0xfd7,0x210,0x431,0x651,0x862,0xa82,0xca3,0xfc3,0x210,0x430,0x640,0x860,0xa80,0xc90,0xfb0,0x121,0x343,0x564,0x786,0x9a8,0xbc9,0xdfb,0x121,0x342,0x463,0x684,0x8a5,0x9c6,0xbf7,0x120,0x241,0x461,0x582,0x6a2,0x8c3,0x9f3,0x120,0x240,0x360,0x480,0x5a0,0x6c0,0x7f0,0x121,0x343,0x465,0x686,0x8a8,0x9ca,0xbfc,0x121,0x242,0x364,0x485,0x5a6,0x6c8,0x7f9,0x020,0x141,0x162,0x283,0x2a4,0x3c5,0x3f6,0x020,0x041,0x061,0x082,0x0a2,0x0c3,0x0f3,0x122,0x344,0x466,0x688,0x8aa,0x9cc,0xbff,0x122,0x244,0x366,0x488,0x5aa,0x6cc,0x7ff,0x022,0x144,0x166,0x288,0x2aa,0x3cc,0x3ff,0x022,0x044,0x066,0x088,0x0aa,0x0cc,0x0ff,0x112,0x334,0x456,0x668,0x88a,0x9ac,0xbcf,0x112,0x224,0x346,0x458,0x56a,0x68c,0x79f,0x002,0x114,0x126,0x238,0x24a,0x35c,0x36f,0x002,0x014,0x016,0x028,0x02a,0x03c,0x03f,0x112,0x334,0x546,0x768,0x98a,0xb9c,0xdbf,0x112,0x324,0x436,0x648,0x85a,0x96c,0xb7f,0x102,0x214,0x416,0x528,0x62a,0x83c,0x93f,0x102,0x204,0x306,0x408,0x50a,0x60c,0x70f,0x212,0x434,0x646,0x868,0xa8a,0xc9c,0xfbe,0x211,0x423,0x635,0x847,0xa59,0xc6b,0xf7d,0x201,0x413,0x615,0x826,0xa28,0xc3a,0xf3c,0x201,0x403,0x604,0x806,0xa08,0xc09,0xf0b
};

const int INCREMENTS[32] = {
    0,   0,
    1,   -1,
    2,   -2,
    4,   -4,
    8,   -8,
    16,  -16,
    32,  -32,
    64,  -64,
    128, -128,
    256, -256,
    512, -512,
    40,  -40,
    80,  -80,
    160, -160,
    320, -320,
    640, -640,
};

inline int calcLayerEffX(int layerwMax, int hscroll, int x) { return (x + hscroll) & layerwMax; }

uint8_t calculateLineColIndex(uint8_t sprZ, uint8_t sprCol, uint8_t l1Col, uint8_t l2Col) {
    switch (sprZ) {
        case 3: return sprCol ? sprCol : (l2Col ? l2Col : l1Col);
        case 2: return l2Col ? l2Col : (sprCol ? sprCol : l1Col);
        case 1: return l2Col ? l2Col : (l1Col ? l1Col : sprCol);
        default: return l2Col ? l2Col : l1Col;
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Construction / reset
// ---------------------------------------------------------------------------

Vera::Vera()
    : m_framebuffer(SCREEN_WIDTH * SCREEN_HEIGHT, 0),
      m_completed(SCREEN_WIDTH * SCREEN_HEIGHT, 0xFF000000u) {
    Vera::reset();
}

Vera::~Vera() = default;

void Vera::videoReset() {
    // video_reset()
    m_ioAddr[0] = m_ioAddr[1] = 0;
    m_ioInc[0] = m_ioInc[1] = 0;
    m_ioAddrsel = 0;
    m_ioDcsel = 0;
    m_ioRddata[0] = m_ioRddata[1] = 0;

    m_ien = 0;
    m_isr = 0;
    m_irqLine9 = 0;

    std::memset(m_regLayer, 0, sizeof(m_regLayer));

    std::memset(m_regComposer, 0, sizeof(m_regComposer));
    m_regComposer[1] = 128;        // hscale = 1.0
    m_regComposer[2] = 128;        // vscale = 1.0
    m_regComposer[5] = 640 >> 2;
    m_regComposer[7] = 480 >> 1;

    // FX
    m_fxAddr1Mode = 0;
    m_fxXPixelPosition = 0x8000;
    m_fxYPixelPosition = 0x8000;
    m_fxXPixelIncrement = 0;
    m_fxYPixelIncrement = 0;
    m_fxCacheWrite = false;
    m_fxCacheFill = false;
    m_fx4bitMode = false;
    m_fx16bitHop = false;
    m_fxSubtract = false;
    m_fxCacheByteCycling = false;
    m_fxTransWrites = false;
    m_fxMultiplier = false;
    m_fxMultAccumulator = 0;
    m_fx2bitPoly = false;
    m_fx2bitPoking = false;
    m_fxCacheNibbleIndex = false;
    m_fxCacheByteIndex = 0;
    m_fxCacheIncrementMode = false;
    std::memset(m_fxCache, 0, sizeof(m_fxCache));
    m_fx16bitHopAlign = 0;
    m_fxNibbleBit[0] = m_fxNibbleBit[1] = false;
    m_fxNibbleIncr[0] = m_fxNibbleIncr[1] = false;
    m_fxPolyFillLength = 0;
    m_fxAffineTileBase = 0;
    m_fxAffineMapBase = 0;
    m_fxAffineMapSize = 2;
    m_fxAffineClip = false;

    std::memset(m_spriteData, 0, sizeof(m_spriteData));

    for (int i = 0; i < 256; i++) {
        m_palette[i * 2 + 0] = DEFAULT_PALETTE[i] & 0xff;
        m_palette[i * 2 + 1] = DEFAULT_PALETTE[i] >> 8;
    }
    refreshPalette();

    // The emulator fills VRAM with rand(); a fixed xorshift keeps runs repeatable
    uint32_t seed = 0x2F6E2B1u;
    for (uint32_t i = 0; i < sizeof(m_vram); i++) {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        m_vram[i] = (uint8_t)(seed >> 8);
    }

    m_spriteLineCollisions = 0;

    m_vgaScanPosX = 0;
    m_vgaScanPosY = 0;
    m_ntscHalfCnt = 0;
    m_ntscScanPosY = 0;

    m_psg.reset();
    m_pcm.reset();

    // Not in video_reset(): keep the cached properties in step with the registers
    for (uint8_t l = 0; l < 2; l++) refreshLayerProperties(l);
    for (uint16_t s = 0; s < NUM_SPRITES; s++) refreshSpriteProperties(s);
}

void Vera::reset() {
    // Power-on state of the statics video_reset() does not touch
    for (auto& p : m_layerProps) p = LayerProps();
    for (auto& a : m_prevLayerProps) for (auto& p : a) p = LayerProps();
    std::memset(m_prevRegComposer, 0, sizeof(m_prevRegComposer));
    std::memset(m_layerLine, 0, sizeof(m_layerLine));
    std::memset(m_spriteLineCol, 0, sizeof(m_spriteLineCol));
    std::memset(m_spriteLineZ, 0, sizeof(m_spriteLineZ));
    std::memset(m_spriteLineMask, 0, sizeof(m_spriteLineMask));
    std::memset(m_colLine, 0, sizeof(m_colLine));
    m_layerLineEnable[0] = m_layerLineEnable[1] = false;
    m_oldLayerLineEnable[0] = m_oldLayerLineEnable[1] = false;
    m_spriteLineEnable = m_oldSpriteLineEnable = false;
    m_yPrev = 0;
    m_sPosXPrev = 0;
    m_effYFp = m_effXFp = 0;
    m_frameCount = 0;
    std::fill(m_framebuffer.begin(), m_framebuffer.end(), 0u);
    std::fill(m_completed.begin(), m_completed.end(), 0xFF000000u);

    videoReset();

    // vera_spi_init()
    m_spiSs = false;
    m_spiBusy = false;
    m_spiAutotx = false;
    m_spiReceivedByte = 0xff;
    m_spiSendingByte = 0;
    m_spiOutcounter = 0.0;

    m_audioAcc = 0;
    m_limiterAmp = 1 << 16;
    m_audioRead = m_audioWrite = m_audioCount = 0;

    updateIrq();
}

// ---------------------------------------------------------------------------
// Layer / sprite / palette properties
// ---------------------------------------------------------------------------

void Vera::refreshLayerProperties(uint8_t layer) {
    // refresh_layer_properties()
    LayerProps* props = &m_layerProps[layer];
    const uint8_t* reg = m_regLayer[layer];

    uint16_t prevLayerwMax = props->layerw_max;
    uint16_t prevHscroll = props->hscroll;

    props->color_depth    = reg[0] & 0x3;
    props->map_base       = (uint32_t)reg[1] << 9;
    props->tile_base      = (uint32_t)(reg[2] & 0xFC) << 9;
    props->bitmap_mode    = (reg[0] & 0x4) != 0;
    props->text_mode      = (props->color_depth == 0) && !props->bitmap_mode;
    props->text_mode_256c = (reg[0] & 8) != 0;
    props->tile_mode      = !props->bitmap_mode && !props->text_mode;

    if (!props->bitmap_mode) {
        props->hscroll = (uint16_t)(reg[3] | (reg[4] & 0xf) << 8);
        props->vscroll = (uint16_t)(reg[5] | (reg[6] & 0xf) << 8);
    } else {
        props->hscroll = 0;
        props->vscroll = 0;
    }

    uint16_t mapw = 0, maph = 0;
    props->tilew = 0;
    props->tileh = 0;

    if (props->tile_mode || props->text_mode) {
        props->mapw_log2 = 5 + ((reg[0] >> 4) & 3);
        props->maph_log2 = 5 + ((reg[0] >> 6) & 3);
        mapw = (uint16_t)(1 << props->mapw_log2);
        maph = (uint16_t)(1 << props->maph_log2);

        props->tilew_log2 = 3 + (reg[2] & 1);
        props->tileh_log2 = 3 + ((reg[2] >> 1) & 1);
        props->tilew = (uint16_t)(1 << props->tilew_log2);
        props->tileh = (uint16_t)(1 << props->tileh_log2);
    } else if (props->bitmap_mode) {
        // bitmap mode is basically tiled mode with a single huge tile
        props->tilew = (reg[2] & 1) ? 640 : 320;
        props->tileh = SCREEN_HEIGHT;
    }

    props->mapw_max   = (uint16_t)(mapw - 1);
    props->maph_max   = (uint16_t)(maph - 1);
    props->tilew_max  = (uint16_t)(props->tilew - 1);
    props->tileh_max  = (uint16_t)(props->tileh - 1);
    props->layerw_max = (uint16_t)((mapw * props->tilew) - 1);
    props->layerh_max = (uint16_t)((maph * props->tileh) - 1);

    // min/max eff_x for bulk reading the map during draw
    if (prevLayerwMax != props->layerw_max || prevHscroll != props->hscroll) {
        int minEffX = INT_MAX, maxEffX = INT_MIN;
        for (int x = 0; x < SCREEN_WIDTH; ++x) {
            int effX = calcLayerEffX(props->layerw_max, props->hscroll, x);
            minEffX = std::min(minEffX, effX);
            maxEffX = std::max(maxEffX, effX);
        }
        props->min_eff_x = minEffX;
        props->max_eff_x = maxEffX;
    }

    props->bits_per_pixel = (uint8_t)(1 << props->color_depth);
    props->tile_size_log2 = (uint8_t)(props->tilew_log2 + props->tileh_log2 + props->color_depth - 3);

    props->first_color_pos  = (uint8_t)(8 - props->bits_per_pixel);
    props->color_mask       = (uint8_t)((1 << props->bits_per_pixel) - 1);
    props->color_fields_max = (uint8_t)((8 >> props->color_depth) - 1);
}

void Vera::refreshSpriteProperties(uint16_t sprite) {
    // refresh_sprite_properties()
    SpriteProps* props = &m_spriteProps[sprite];
    const uint8_t* d = m_spriteData[sprite];

    props->zdepth = (int8_t)((d[6] >> 2) & 3);
    props->collision_mask = d[6] & 0xf0;

    props->x = (int16_t)(d[2] | (d[3] & 3) << 8);
    props->y = (int16_t)(d[4] | (d[5] & 3) << 8);
    props->width_log2  = (uint8_t)(((d[7] >> 4) & 3) + 3);
    props->height_log2 = (uint8_t)((d[7] >> 6) + 3);
    props->width       = (uint8_t)(1 << props->width_log2);
    props->height      = (uint8_t)(1 << props->height_log2);

    // fix up negative coordinates
    if (props->x >= 0x400 - props->width)  props->x = (int16_t)(props->x - 0x400);
    if (props->y >= 0x400 - props->height) props->y = (int16_t)(props->y - 0x400);

    props->hflip = d[6] & 1;
    props->vflip = (d[6] >> 1) & 1;

    props->color_mode = (d[1] >> 7) & 1;
    props->address    = (uint32_t)d[0] << 5 | (uint32_t)(d[1] & 0xf) << 13;

    props->palette_offset = (uint16_t)((d[7] & 0x0f) << 4);
}

void Vera::refreshPalette() {
    // refresh_palette()
    const uint8_t outMode = m_regComposer[0] & 3;
    const bool chromaDisable = ((m_regComposer[0] & 0x07) == 6);
    for (int i = 0; i < 256; ++i) {
        uint8_t r, g, b;
        if (outMode == 0) {
            // video generation off -> blue screen
            r = 0; g = 0; b = 255;
        } else {
            uint16_t entry = (uint16_t)(m_palette[i * 2] | m_palette[i * 2 + 1] << 8);
            r = (uint8_t)(((entry >> 8) & 0xf) << 4 | ((entry >> 8) & 0xf));
            g = (uint8_t)(((entry >> 4) & 0xf) << 4 | ((entry >> 4) & 0xf));
            b = (uint8_t)((entry & 0xf) << 4 | (entry & 0xf));
            if (chromaDisable) r = g = b = (uint8_t)((r + b + g) / 3);
        }
        m_paletteEntries[i] = (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
    }
    m_paletteDirty = false;
}

uint32_t Vera::paletteRgb(int i) {
    if (m_paletteDirty) refreshPalette();
    return m_paletteEntries[i & 0xFF];
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void Vera::renderSpriteLine(uint16_t y) {
    // render_sprite_line()
    std::memset(m_spriteLineCol, 0, SCREEN_WIDTH);
    std::memset(m_spriteLineZ, 0, SCREEN_WIDTH);
    std::memset(m_spriteLineMask, 0, SCREEN_WIDTH);

    uint16_t budget = 800 + 1;
    for (int i = 0; i < NUM_SPRITES; i++) {
        // one clock per lookup
        budget--; if (budget == 0) break;
        const SpriteProps* props = &m_spriteProps[i];

        if (props->zdepth == 0) continue;

        // does this line fall within the sprite?
        if ((int)y < props->y || (int)y >= props->y + props->height) continue;

        const uint16_t effSy = (uint16_t)(props->vflip ? ((props->height - 1) - (y - props->y)) : (y - props->y));

        int16_t       effSx     = (int16_t)(props->hflip ? (props->width - 1) : 0);
        const int16_t effSxIncr = props->hflip ? -1 : 1;

        // The emulator reads straight from video_ram here (and can run past
        // its end for sprites near $1FFFF); this wraps at 128 KB.
        const uint32_t bitmapAddr = props->address + (uint32_t)(effSy << (props->width_log2 - (1 - props->color_mode)));

        uint8_t unpacked[64];
        const uint16_t width = props->width < 64 ? props->width : 64;
        const uint8_t vramFetchMask = (uint8_t)(((2 - props->color_mode) << 2) - 1);
        if (props->color_mode == 0) {
            // 4bpp
            for (int k = 0; k < width / 2; k++) {
                uint8_t s = videoSpaceRead(bitmapAddr + k);
                unpacked[k * 2]     = s >> 4;
                unpacked[k * 2 + 1] = s & 0xf;
            }
        } else {
            // 8bpp
            for (int k = 0; k < width; k++) unpacked[k] = videoSpaceRead(bitmapAddr + k);
        }

        for (uint16_t sx = 0; sx < props->width; ++sx) {
            const uint16_t lineX = (uint16_t)(props->x + sx);
            if (lineX >= SCREEN_WIDTH) {
                effSx = (int16_t)(effSx + effSxIncr);
                continue;
            }

            // one clock per fetched 32 bits
            if (!(sx & vramFetchMask)) {
                budget--; if (budget == 0) break;
            }
            // one clock per rendered pixel
            budget--; if (budget == 0) break;

            uint8_t colIndex = unpacked[effSx];
            effSx = (int16_t)(effSx + effSxIncr);

            if (colIndex > 0) {
                m_spriteLineCollisions |= m_spriteLineMask[lineX] & props->collision_mask;
                m_spriteLineMask[lineX] |= props->collision_mask;

                if (props->zdepth > m_spriteLineZ[lineX]) {
                    if (colIndex < 16) colIndex = (uint8_t)(colIndex + props->palette_offset);
                    m_spriteLineCol[lineX] = colIndex;
                    m_spriteLineZ[lineX] = (uint8_t)props->zdepth;
                }
            }
        }
    }
}

static inline uint32_t mapAddrBase2(uint32_t mapBase, uint8_t tilewLog2, uint8_t tilehLog2, uint8_t mapwLog2,
                                    int effX, int effY) {
    return mapBase + ((uint32_t)((((effY >> tilehLog2) << mapwLog2) + (effX >> tilewLog2))) << 1);
}

void Vera::renderLayerLineText(uint8_t layer, uint16_t y) {
    // render_layer_line_text()
    const LayerProps* props  = &m_prevLayerProps[1][layer];
    const LayerProps* props0 = &m_prevLayerProps[0][layer];

    const uint8_t maxPixelsPerByte = (uint8_t)((8 >> props->color_depth) - 1);
    const int     effY = (y + props0->vscroll) & props0->layerh_max;
    const int     yy   = effY & props->tileh_max;

    // additional bytes to reach the correct line of the tile
    const uint32_t yAdd = (uint32_t)((yy << props->tilew_log2) >> 3);

    auto mapAddr = [&](int effX) {
        return mapAddrBase2(props->map_base, props->tilew_log2, props->tileh_log2, props->mapw_log2, effX, effY);
    };
    const uint32_t mapAddrBegin = mapAddr(props->min_eff_x);
    const uint32_t mapAddrEnd   = mapAddr(props->max_eff_x);
    const int      size         = (int)(mapAddrEnd - mapAddrBegin) + 2;

    uint8_t tileBytes[512];   // max 256 tiles, 2 bytes each
    videoSpaceReadRange(tileBytes, mapAddrBegin, (uint32_t)std::min(std::max(size, 0), 512));

    uint32_t tileStart;
    uint8_t  fgColor, bgColor, s;
    int      colorShift;

    {
        const int effX = calcLayerEffX(props->layerw_max, props->hscroll, 0);
        const int xx   = effX & props->tilew_max;
        const uint32_t ma = (mapAddr(effX) - mapAddrBegin) & 0x1FF;
        const uint8_t tileIndex = tileBytes[ma];
        const uint8_t byte1     = tileBytes[(ma + 1) & 0x1FF];
        if (!props->text_mode_256c) {
            fgColor = byte1 & 15;
            bgColor = byte1 >> 4;
        } else {
            fgColor = byte1;
            bgColor = 0;
        }
        tileStart = (uint32_t)tileIndex << props->tile_size_log2;
        const uint16_t xAdd = (uint16_t)(xx >> 3);
        s = videoSpaceRead(props->tile_base + tileStart + yAdd + xAdd);
        colorShift = maxPixelsPerByte - (xx & 0x7);
    }

    for (int x = 0; x < SCREEN_WIDTH; x++) {
        const int effX = calcLayerEffX(props->layerw_max, props->hscroll, x);
        const int xx   = effX & props->tilew_max;

        if ((effX & 0x7) == 0) {
            if ((effX & props->tilew_max) == 0) {
                const uint32_t ma = (mapAddr(effX) - mapAddrBegin) & 0x1FF;
                const uint8_t tileIndex = tileBytes[ma];
                const uint8_t byte1     = tileBytes[(ma + 1) & 0x1FF];
                if (!props->text_mode_256c) {
                    fgColor = byte1 & 15;
                    bgColor = byte1 >> 4;
                } else {
                    fgColor = byte1;
                    bgColor = 0;
                }
                tileStart = (uint32_t)tileIndex << props->tile_size_log2;
            }
            const uint16_t xAdd = (uint16_t)(xx >> 3);
            s = videoSpaceRead(props->tile_base + tileStart + yAdd + xAdd);
            colorShift = maxPixelsPerByte;
        }

        const uint8_t colIndex = (uint8_t)((s >> (colorShift & 7)) & 1);
        --colorShift;
        m_layerLine[layer][x] = colIndex ? fgColor : bgColor;
    }
}

void Vera::renderLayerLineTile(uint8_t layer, uint16_t y) {
    // render_layer_line_tile()
    const LayerProps* props  = &m_prevLayerProps[1][layer];
    const LayerProps* props0 = &m_prevLayerProps[0][layer];

    const uint8_t  maxPixelsPerByte = (uint8_t)((8 >> props->color_depth) - 1);
    const int      effY     = (y + props0->vscroll) & props0->layerh_max;
    const uint8_t  yy       = (uint8_t)(effY & props->tileh_max);
    const uint8_t  yyFlip   = (uint8_t)(yy ^ props->tileh_max);
    const uint32_t yAdd     = (uint32_t)yy << ((props->tilew_log2 + props->color_depth - 3) & 31);
    const uint32_t yAddFlip = (uint32_t)yyFlip << ((props->tilew_log2 + props->color_depth - 3) & 31);

    auto mapAddr = [&](int effX) {
        return mapAddrBase2(props->map_base, props->tilew_log2, props->tileh_log2, props->mapw_log2, effX, effY);
    };
    const uint32_t mapAddrBegin = mapAddr(props->min_eff_x);
    const uint32_t mapAddrEnd   = mapAddr(props->max_eff_x);
    const int      size         = (int)(mapAddrEnd - mapAddrBegin) + 2;

    uint8_t tileBytes[512];
    videoSpaceReadRange(tileBytes, mapAddrBegin, (uint32_t)std::min(std::max(size, 0), 512));

    uint8_t  paletteOffset;
    bool     vflip, hflip;
    uint32_t tileStart;
    uint8_t  s;
    int      colorShift;
    int      colorShiftIncr;

    auto fetchMap = [&](int effX) {
        const uint32_t ma = (mapAddr(effX) - mapAddrBegin) & 0x1FF;
        const uint8_t byte0 = tileBytes[ma];
        const uint8_t byte1 = tileBytes[(ma + 1) & 0x1FF];
        vflip = (byte1 >> 3) & 1;
        hflip = (byte1 >> 2) & 1;
        paletteOffset = byte1 & 0xf0;
        const uint16_t tileIndex = (uint16_t)(byte0 | ((byte1 & 3) << 8));
        tileStart = (uint32_t)tileIndex << props->tile_size_log2;
        colorShiftIncr = hflip ? props->bits_per_pixel : -props->bits_per_pixel;
    };
    auto fetchTile = [&](int effX) {
        int xx = effX & props->tilew_max;
        if (hflip) {
            xx = xx ^ props->tilew_max;
            colorShift = 0;
        } else {
            colorShift = props->first_color_pos;
        }
        const uint16_t xAdd = (uint16_t)((xx << props->color_depth) >> 3);
        s = videoSpaceRead(props->tile_base + tileStart + (vflip ? yAddFlip : yAdd) + xAdd);
    };

    {
        const int effX = calcLayerEffX(props->layerw_max, props->hscroll, 0);
        fetchMap(effX);
        fetchTile(effX);
    }

    for (int x = 0; x < SCREEN_WIDTH; x++) {
        const int effX = calcLayerEffX(props->layerw_max, props->hscroll, x);
        if ((effX & maxPixelsPerByte) == 0) {
            if ((effX & props->tilew_max) == 0) fetchMap(effX);
            fetchTile(effX);
        }

        uint8_t colIndex = (uint8_t)((s >> (colorShift & 7)) & props->color_mask);
        colorShift += colorShiftIncr;

        // palette offset
        if (colIndex > 0 && colIndex < 16) {
            colIndex = (uint8_t)(colIndex + paletteOffset);
            if (props->text_mode_256c) colIndex |= 0x80;
        }
        m_layerLine[layer][x] = colIndex;
    }
}

void Vera::renderLayerLineBitmap(uint8_t layer, uint16_t y) {
    // render_layer_line_bitmap()
    const LayerProps* props = &m_prevLayerProps[1][layer];
    if (props->tilew == 0 || props->tileh == 0) return;

    int yy = y % props->tileh;
    uint32_t yAdd = (uint32_t)((yy * props->tilew * props->bits_per_pixel) >> 3);

    for (int x = 0; x < SCREEN_WIDTH; x++) {
        int xx = x % props->tilew;
        uint8_t paletteOffset = m_regLayer[layer][4] & 0xf;
        uint16_t xAdd = (uint16_t)((xx * props->bits_per_pixel) >> 3);
        uint8_t s = videoSpaceRead(props->tile_base + yAdd + xAdd);

        uint8_t colIndex = (uint8_t)((s >> ((props->first_color_pos - ((xx & props->color_fields_max) << props->color_depth)) & 7))
                                     & props->color_mask);
        if (colIndex > 0 && colIndex < 16) {
            colIndex = (uint8_t)(colIndex + (paletteOffset << 4));
            if (props->text_mode_256c) colIndex |= 0x80;
        }
        m_layerLine[layer][x] = colIndex;
    }
}

void Vera::renderLine(uint16_t y, double scanPosX) {
    // render_line()
    uint8_t  dcVideo = m_regComposer[0];
    uint16_t vstart = (uint16_t)(m_regComposer[6] << 1);
    uint16_t vstop  = (uint16_t)(m_regComposer[7] << 1);

    if (y != m_yPrev) {
        m_yPrev = y;
        m_sPosXPrev = 0;

        // 2-line history of the composer and layer registers, so that raster
        // effects take hold at render, a line later, or mid-line at scan-out
        std::memcpy(m_prevRegComposer[1], m_prevRegComposer[0], COMPOSER_SLOTS);
        std::memcpy(m_prevRegComposer[0], m_regComposer, COMPOSER_SLOTS);
        m_prevLayerProps[1][0] = m_prevLayerProps[0][0];
        m_prevLayerProps[1][1] = m_prevLayerProps[0][1];
        m_prevLayerProps[0][0] = m_layerProps[0];
        m_prevLayerProps[0][1] = m_layerProps[1];

        if ((dcVideo & 3) > 1) {   // 480i or 240p
            if ((y >> 1) == 0) {
                m_effYFp = (uint32_t)y * ((uint32_t)m_prevRegComposer[1][2] << 9);
            } else if (((y & 0xfffe) >= vstart) && ((y & 0xfffe) < vstop)) {
                m_effYFp += ((uint32_t)m_prevRegComposer[1][2] << 10);
            }
        } else {
            if (y == 0) {
                m_effYFp = 0;
            } else if ((y >= vstart) && (y < vstop)) {
                m_effYFp += ((uint32_t)m_prevRegComposer[1][2] << 9);
            }
        }
    }

    if ((dcVideo & 8) && (dcVideo & 3) > 1) y &= 0xfffe;   // progressive NTSC/RGB

    if (m_paletteDirty) refreshPalette();

    if (y >= SCREEN_HEIGHT) return;

    long sx = std::lround(scanPosX);
    uint16_t sPosX = (uint16_t)std::min<long>(std::max<long>(sx, 0), SCREEN_WIDTH);

    if (m_sPosXPrev == 0) m_effXFp = 0;

    uint8_t  outMode = m_regComposer[0] & 3;
    uint8_t  borderColor = m_regComposer[3];
    uint16_t hstart = (uint16_t)(m_regComposer[4] << 2);
    uint16_t hstop  = (uint16_t)(m_regComposer[5] << 2);

    uint16_t effY = (uint16_t)(m_effYFp >> 16);
    if (effY >= 480) effY = (uint16_t)(480 - (y & 1));

    m_layerLineEnable[0] = dcVideo & 0x10;
    m_layerLineEnable[1] = dcVideo & 0x20;
    m_spriteLineEnable   = dcVideo & 0x40;

    // clear layer_line if the layer gets disabled
    for (uint8_t layer = 0; layer < 2; layer++) {
        if (!m_layerLineEnable[layer] && m_oldLayerLineEnable[layer]) {
            for (uint16_t i = m_sPosXPrev; i < SCREEN_WIDTH; i++) m_layerLine[layer][i] = 0;
        }
        if (m_sPosXPrev == 0) m_oldLayerLineEnable[layer] = m_layerLineEnable[layer];
    }

    // clear sprite_line if sprites get disabled
    if (!m_spriteLineEnable && m_oldSpriteLineEnable) {
        for (uint16_t i = m_sPosXPrev; i < SCREEN_WIDTH; i++) {
            m_spriteLineCol[i] = 0;
            m_spriteLineZ[i] = 0;
            m_spriteLineMask[i] = 0;
        }
    }
    if (m_sPosXPrev == 0) m_oldSpriteLineEnable = m_spriteLineEnable;

    if (m_spriteLineEnable) renderSpriteLine(effY);

    for (uint8_t layer = 0; layer < 2; layer++) {
        if (!m_layerLineEnable[layer]) continue;
        if (m_prevLayerProps[1][layer].text_mode)        renderLayerLineText(layer, effY);
        else if (m_prevLayerProps[1][layer].bitmap_mode) renderLayerLineBitmap(layer, effY);
        else                                             renderLayerLineTile(layer, effY);
    }

    // with video output on, the colour indices of the line
    if (outMode != 0) {
        if (y < vstart || y >= vstop) {
            std::memset(m_colLine, borderColor, SCREEN_WIDTH);
        } else {
            hstart = hstart < 640 ? hstart : 640;
            hstop  = hstop  < 640 ? hstop  : 640;

            for (uint16_t x = m_sPosXPrev; x < hstart && x < sPosX; ++x) m_colLine[x] = borderColor;

            const uint32_t scale = m_regComposer[1];
            for (uint16_t x = std::max(hstart, m_sPosXPrev); x < hstop && x < sPosX; ++x) {
                uint16_t effX = (uint16_t)(m_effXFp >> 16);
                m_colLine[x] = (effX < SCREEN_WIDTH)
                    ? calculateLineColIndex(m_spriteLineZ[effX], m_spriteLineCol[effX],
                                            m_layerLine[0][effX], m_layerLine[1][effX])
                    : 0;
                m_effXFp += (scale << 9);
            }
            for (uint16_t x = hstop; x < sPosX; ++x) m_colLine[x] = borderColor;
        }
    }

    uint32_t* fb = m_framebuffer.data() + (size_t)y * SCREEN_WIDTH;
    for (uint16_t x = m_sPosXPrev; x < sPosX; x++) fb[x] = m_paletteEntries[m_colLine[x]];

    // NTSC overscan: outside the title-safe area at a quarter brightness
    if (outMode == 2) {
        for (uint16_t x = m_sPosXPrev; x < sPosX; x++) {
            if (x < SCREEN_WIDTH * TITLE_SAFE_X || x > SCREEN_WIDTH * (1 - TITLE_SAFE_X) ||
                y < SCREEN_HEIGHT * TITLE_SAFE_Y || y > SCREEN_HEIGHT * (1 - TITLE_SAFE_Y)) {
                fb[x] = (fb[x] & 0x00fcfcfc) >> 2;
            }
        }
    }

    m_sPosXPrev = sPosX;
}

void Vera::updateIsrAndColl(uint16_t y, uint16_t compare) {
    // update_isr_and_coll()
    if (y == SCREEN_HEIGHT) {
        if (m_spriteLineCollisions != 0) m_isr |= INT_SPRCOL;
        m_isr = (uint8_t)((m_isr & 0xf) | m_spriteLineCollisions);
        m_spriteLineCollisions = 0;
        m_isr |= INT_VSYNC;
    }
    if (y == compare) m_isr |= INT_LINE;
}

void Vera::completeFrame() {
    for (size_t i = 0; i < m_framebuffer.size(); i++) m_completed[i] = 0xFF000000u | m_framebuffer[i];
    m_frameCount++;
}

void Vera::videoStep(uint64_t cycles) {
    // video_step(), for any number of lines at once
    const bool ntscMode = m_regComposer[0] & 2;
    const uint64_t add = cycles * PIXEL_CLOCK_HZ;

    const uint64_t vgaWidth = (uint64_t)VGA_SCAN_WIDTH * m_cpuHz;
    m_vgaScanPosX += add;
    while (m_vgaScanPosX > vgaWidth) {
        m_vgaScanPosX -= vgaWidth;
        if (!ntscMode) renderLine(m_vgaScanPosY, VGA_SCAN_WIDTH);
        m_vgaScanPosY++;
        if (m_vgaScanPosY == SCAN_HEIGHT) {
            m_vgaScanPosY = 0;
            if (!ntscMode) completeFrame();
        }
        if (!ntscMode) updateIsrAndColl(m_vgaScanPosY, m_irqLine9);
    }

    const uint64_t ntscWidth = (uint64_t)NTSC_HALF_SCAN_WIDTH * m_cpuHz;
    m_ntscHalfCnt += add;
    while (m_ntscHalfCnt > ntscWidth) {
        m_ntscHalfCnt -= ntscWidth;
        if (ntscMode) {
            if (m_ntscScanPosY < SCAN_HEIGHT) {
                uint16_t y = (uint16_t)(m_ntscScanPosY - NTSC_Y_OFFSET_LOW);
                if ((y & 1) == 0) renderLine(y, NTSC_HALF_SCAN_WIDTH);
            } else {
                uint16_t y = (uint16_t)(m_ntscScanPosY - NTSC_Y_OFFSET_HIGH);
                if ((y & 1) == 0) renderLine((uint16_t)(y | 1), NTSC_HALF_SCAN_WIDTH);
            }
        }
        m_ntscScanPosY++;
        if (m_ntscScanPosY == SCAN_HEIGHT) {
            m_regComposer[0] |= 0x80;
            if (ntscMode) completeFrame();
        }
        if (m_ntscScanPosY == SCAN_HEIGHT * 2) {
            m_regComposer[0] &= ~0x80;
            m_ntscScanPosY = 0;
            if (ntscMode) completeFrame();
        }
        if (ntscMode) {
            // correct enough for even screen heights
            if (m_ntscScanPosY < SCAN_HEIGHT)
                updateIsrAndColl((uint16_t)(m_ntscScanPosY - NTSC_Y_OFFSET_LOW), m_irqLine9 & ~1);
            else
                updateIsrAndColl((uint16_t)(m_ntscScanPosY - NTSC_Y_OFFSET_HIGH), m_irqLine9 & ~1);
        }
    }
}

void Vera::midline() {
    // video_step(MHZ, 0, true): draw the line so far before a register change
    const bool ntscMode = m_regComposer[0] & 2;
    if (!ntscMode) {
        renderLine(m_vgaScanPosY, (double)m_vgaScanPosX / m_cpuHz);
    } else {
        double pos = (double)m_ntscHalfCnt / m_cpuHz;
        if (m_ntscScanPosY < SCAN_HEIGHT) {
            uint16_t y = (uint16_t)(m_ntscScanPosY - NTSC_Y_OFFSET_LOW);
            if ((y & 1) == 0) renderLine(y, pos);
        } else {
            uint16_t y = (uint16_t)(m_ntscScanPosY - NTSC_Y_OFFSET_HIGH);
            if ((y & 1) == 0) renderLine((uint16_t)(y | 1), pos);
        }
    }
}

void Vera::renderFrame(uint32_t* buffer) {
    std::memcpy(buffer, m_completed.data(), m_completed.size() * sizeof(uint32_t));
}

// ---------------------------------------------------------------------------
// Address ports and VRAM
// ---------------------------------------------------------------------------

uint32_t Vera::getAndIncAddress(uint8_t sel, bool write) {
    // get_and_inc_address()
    uint32_t address = m_ioAddr[sel];
    int incr = INCREMENTS[m_ioInc[sel]];

    if (m_fx4bitMode && m_fxNibbleIncr[sel] && !incr) {
        if (m_fxNibbleBit[sel]) {
            if ((m_ioInc[sel] & 1) == 0) m_ioAddr[sel] += 1;
            m_fxNibbleBit[sel] = false;
        } else {
            if (m_ioInc[sel] & 1) m_ioAddr[sel] -= 1;
            m_fxNibbleBit[sel] = true;
        }
    }

    if (sel == 1 && m_fx16bitHop) {
        if (incr == 4) {
            incr = (m_fx16bitHopAlign == (address & 0x3)) ? 1 : 3;
        } else if (incr == 320) {
            incr = (m_fx16bitHopAlign == (address & 0x3)) ? 1 : 319;
        }
    }

    m_ioAddr[sel] += (uint32_t)incr;

    if (sel == 1 && m_fxAddr1Mode == 1) {   // FX line draw
        m_fxXPixelPosition += m_fxXPixelIncrement;
        if (m_fxXPixelPosition & 0x10000) {
            m_fxXPixelPosition &= ~0x10000u;
            if (m_fx4bitMode && m_fxNibbleIncr[0]) {
                if (m_fxNibbleBit[1]) {
                    if ((m_ioInc[0] & 1) == 0) m_ioAddr[1] += 1;
                    m_fxNibbleBit[1] = false;
                } else {
                    if (m_ioInc[0] & 1) m_ioAddr[1] -= 1;
                    m_fxNibbleBit[1] = true;
                }
            }
            m_ioAddr[1] += (uint32_t)INCREMENTS[m_ioInc[0]];
        }
    } else if (m_fxAddr1Mode == 2 && !write) {   // FX polygon fill
        m_fxXPixelPosition += m_fxXPixelIncrement;
        m_fxYPixelPosition += m_fxYPixelIncrement;
        m_fxPolyFillLength = (uint16_t)(((int32_t)m_fxYPixelPosition >> 16) - ((int32_t)m_fxXPixelPosition >> 16));
        if (sel == 0 && m_fxCacheByteCycling && !m_fxCacheFill) {
            m_fxCacheByteIndex = (m_fxCacheByteIndex + 1) & 3;
        }
        if (sel == 1) {
            if (m_fx4bitMode) {
                m_ioAddr[1] = m_ioAddr[0] + (m_fxXPixelPosition >> 17);
                m_fxNibbleBit[1] = (m_fxXPixelPosition >> 16) & 1;
            } else {
                m_ioAddr[1] = m_ioAddr[0] + (m_fxXPixelPosition >> 16);
            }
        }
    } else if (sel == 1 && m_fxAddr1Mode == 3 && !write) {   // FX affine
        m_fxXPixelPosition += m_fxXPixelIncrement;
        m_fxYPixelPosition += m_fxYPixelIncrement;
    }

    // 17-bit address (the emulator keeps the full 32-bit value)
    m_ioAddr[0] &= 0x1FFFF;
    m_ioAddr[1] &= 0x1FFFF;
    return address;
}

void Vera::fxAffinePrefetch() {
    // fx_affine_prefetch()
    if (m_fxAddr1Mode != 3) return;

    uint32_t address;
    uint8_t xTile = (m_fxXPixelPosition >> 19) & 0xff;
    uint8_t yTile = (m_fxYPixelPosition >> 19) & 0xff;
    uint8_t xSub  = (m_fxXPixelPosition >> 16) & 0x07;
    uint8_t ySub  = (m_fxYPixelPosition >> 16) & 0x07;
    const uint8_t fx4 = m_fx4bitMode ? 1 : 0;

    if (!m_fxAffineClip) {   // wrap
        xTile &= (uint8_t)(m_fxAffineMapSize - 1);
        yTile &= (uint8_t)(m_fxAffineMapSize - 1);
    }

    if (xTile >= m_fxAffineMapSize || yTile >= m_fxAffineMapSize) {
        // clipped: the value of tile 0
        address = m_fxAffineTileBase + (uint32_t)(ySub << (3 - fx4)) + (uint32_t)(xSub >> fx4);
    } else {
        address = m_fxAffineMapBase + (uint32_t)(yTile * m_fxAffineMapSize) + xTile;
        uint8_t tileIdx = videoSpaceRead(address);
        address = m_fxAffineTileBase + ((uint32_t)tileIdx << (6 - fx4));
        address += (uint32_t)(ySub << (3 - fx4)) + (uint32_t)(xSub >> fx4);
    }
    m_fxNibbleBit[1] = ((xSub & 1) >> (1 - fx4)) != 0;
    m_ioAddr[1] = address & 0x1FFFF;
    m_ioRddata[1] = videoSpaceRead(address);
}

void Vera::videoSpaceReadRange(uint8_t* dest, uint32_t address, uint32_t size) const {
    for (uint32_t i = 0; i < size; ++i) dest[i] = videoSpaceRead(address + i);
}

void Vera::specialWrite(uint32_t address, uint8_t value) {
    if (address >= ADDR_PSG_START && address < ADDR_PSG_END) {
        m_psg.writeReg(address & 0x3f, value);
    } else if (address >= ADDR_PALETTE_START && address < ADDR_PALETTE_END) {
        m_palette[address & 0x1ff] = value;
        m_paletteDirty = true;
    } else if (address >= ADDR_SPRDATA_START && address < ADDR_SPRDATA_END) {
        m_spriteData[(address >> 3) & 0x7f][address & 0x7] = value;
        refreshSpriteProperties((address >> 3) & 0x7f);
    }
}

void Vera::videoSpaceWrite(uint32_t address, uint8_t value) {
    // video_space_write()
    m_vram[address & 0x1FFFF] = value;
    specialWrite(address, value);
}

void Vera::fxVideoSpaceWrite(uint32_t address, bool nibble, uint8_t value) {
    // fx_video_space_write()
    uint8_t& v = m_vram[address & 0x1FFFF];
    if (m_fx4bitMode) {
        if (nibble) {
            if (!m_fxTransWrites || (value & 0x0f) > 0) v = (uint8_t)((v & 0xf0) | (value & 0x0f));
        } else {
            if (!m_fxTransWrites || (value & 0xf0) > 0) v = (uint8_t)((v & 0x0f) | (value & 0xf0));
        }
    } else {
        if (!m_fxTransWrites || value > 0) v = value;
    }
    specialWrite(address, value);
}

void Vera::fxVramCacheWrite(uint32_t address, uint8_t value, uint8_t mask) {
    // fx_vram_cache_write(): VRAM only, as in the emulator (no palette/sprite/PSG update)
    if (!m_fxTransWrites || value > 0) {
        uint8_t& v = m_vram[address & 0x1FFFF];
        switch (mask) {
            case 0: v = value; break;
            case 1: v = (uint8_t)((v & 0x0f) | (value & 0xf0)); break;
            case 2: v = (uint8_t)((v & 0xf0) | (value & 0x0f)); break;
            case 3: break;
        }
    }
}

int32_t Vera::fxMultiply() const {
    return (int32_t)(int16_t)((m_fxCache[1] << 8) | m_fxCache[0]) *
           (int32_t)(int16_t)((m_fxCache[3] << 8) | m_fxCache[2]);
}

// ---------------------------------------------------------------------------
// Registers
// ---------------------------------------------------------------------------

uint8_t Vera::dcValue(uint8_t reg) const {
    // video_get_dc_value()
    switch (reg & 0x1F) {
        case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07:
        case 0x08: case 0x09: case 0x0a: case 0x0c: case 0x0d: case 0x0e: case 0x0f:
            return m_regComposer[reg];
        case 0x0b:
            return m_regComposer[reg] & 0x3f;
        case 0x10: return (m_fxXPixelPosition >> 16) & 0xff;
        case 0x11: return (uint8_t)(((m_fxXPixelPosition >> 24) & 0x07) | (m_fxXPixelPosition & 0x80));
        case 0x12: return (m_fxYPixelPosition >> 16) & 0xff;
        case 0x13: return (uint8_t)(((m_fxYPixelPosition >> 24) & 0x07) | (m_fxYPixelPosition & 0x80));
        case 0x14: return (m_fxXPixelPosition >> 8) & 0xff;
        case 0x15: return (m_fxYPixelPosition >> 8) & 0xff;
        case 0x16:   // DCSEL=5, $9F2B
            if (m_fxPolyFillLength >= 768) {
                return (m_fx2bitPoly && m_fxAddr1Mode == 2) ? 0x00 : 0x80;
            }
            if (m_fx4bitMode) {
                if (m_fx2bitPoly && m_fxAddr1Mode == 2) {
                    return (uint8_t)(((m_fxYPixelPosition & 0x00008000) >> 8) |
                        ((m_fxXPixelPosition >> 11) & 0x60) |
                        ((m_fxXPixelPosition >> 14) & 0x10) |
                        ((m_fxPolyFillLength & 0x0007) << 1) |
                        ((m_fxXPixelPosition & 0x00008000) >> 15));
                }
                return (uint8_t)(((!!(m_fxPolyFillLength & 0xfff8)) << 7) |
                    ((m_fxXPixelPosition >> 11) & 0x60) |
                    ((m_fxXPixelPosition >> 14) & 0x10) |
                    ((m_fxPolyFillLength & 0x0007) << 1));
            }
            return (uint8_t)(((!!(m_fxPolyFillLength & 0xfff0)) << 7) |
                ((m_fxXPixelPosition >> 11) & 0x60) |
                ((m_fxPolyFillLength & 0x000f) << 1));
        case 0x17: return (uint8_t)((m_fxPolyFillLength & 0x03f8) >> 2);
        case 0x18: return m_fxCache[0];
        case 0x19: return m_fxCache[1];
        case 0x1a: return m_fxCache[2];
        case 0x1b: return m_fxCache[3];
        default: break;
    }
    return VERA_VERSION_STRING[reg % 4];
}

uint16_t Vera::scanline() const {
    bool ntscMode = m_regComposer[0] & 2;
    uint16_t s = ntscMode ? (uint16_t)(m_ntscScanPosY % SCAN_HEIGHT) : m_vgaScanPosY;
    return s >= 512 ? 511 : s;
}

uint8_t Vera::isr() const {
    return (uint8_t)(m_isr | (m_pcm.fifoAlmostEmpty() ? INT_AFLOW : 0));
}

bool Vera::irqAsserted() const {
    // video_get_irq_out()
    return (isr() & m_ien) != 0;
}

void Vera::updateIrq() {
    bool level = irqAsserted();
    if (level != m_irqOut) {
        m_irqOut = level;
        if (m_irqLine) m_irqLine->set(level);
    }
}

uint8_t Vera::readReg(uint8_t reg, bool sideEffects) {
    // video_read(); sideEffects == false is a peek
    reg &= 0x1F;
    uint16_t sl = scanline();

    switch (reg) {
        case 0x00: return m_ioAddr[m_ioAddrsel] & 0xff;
        case 0x01: return (m_ioAddr[m_ioAddrsel] >> 8) & 0xff;
        case 0x02:
            return (uint8_t)(((m_ioAddr[m_ioAddrsel] >> 16) & 1) | (m_fxNibbleBit[m_ioAddrsel] << 1) |
                             (m_fxNibbleIncr[m_ioAddrsel] << 2) | (m_ioInc[m_ioAddrsel] << 3));
        case 0x03:
        case 0x04: {
            const uint8_t sel = reg - 3;
            if (!sideEffects) return m_ioRddata[sel];

            bool addrNibble = m_fxNibbleBit[sel];
            getAndIncAddress(sel, false);

            uint8_t value = m_ioRddata[sel];

            if (reg == 4 && m_fxAddr1Mode == 3) fxAffinePrefetch();
            else m_ioRddata[sel] = videoSpaceRead(m_ioAddr[sel]);

            if (m_fxCacheFill) {
                if (m_fx4bitMode) {
                    uint8_t nibbleRead = addrNibble ? (uint8_t)((value & 0x0f) << 4) : (uint8_t)(value & 0xf0);
                    if (m_fxCacheNibbleIndex) {
                        m_fxCache[m_fxCacheByteIndex] = (uint8_t)((m_fxCache[m_fxCacheByteIndex] & 0xf0) | (nibbleRead >> 4));
                        m_fxCacheNibbleIndex = false;
                        m_fxCacheByteIndex = (m_fxCacheByteIndex + 1) & 0x3;
                    } else {
                        m_fxCache[m_fxCacheByteIndex] = (uint8_t)((m_fxCache[m_fxCacheByteIndex] & 0x0f) | nibbleRead);
                        m_fxCacheNibbleIndex = true;
                    }
                } else {
                    m_fxCache[m_fxCacheByteIndex] = value;
                    if (m_fxCacheIncrementMode)
                        m_fxCacheByteIndex = (uint8_t)((m_fxCacheByteIndex & 0x2) | ((m_fxCacheByteIndex + 1) & 0x1));
                    else
                        m_fxCacheByteIndex = (m_fxCacheByteIndex + 1) & 0x3;
                }
            }
            return value;
        }
        case 0x05: return (uint8_t)((m_ioDcsel << 1) | m_ioAddrsel);
        case 0x06: return (uint8_t)(((m_irqLine9 & 0x100) >> 1) | ((sl & 0x100) >> 2) | (m_ien & 0xF));
        case 0x07: return isr();
        case 0x08: return sl & 0xFF;

        case 0x09: case 0x0A: case 0x0B: case 0x0C: {
            int i = reg - 0x09 + (m_ioDcsel << 2);
            switch (i) {
                case 0x00: case 0x01: case 0x02: case 0x03: case 0x04:
                case 0x05: case 0x06: case 0x07: case 0x08:
                case 0x16:   // DCSEL=5, $9F2B
                case 0x17:   // DCSEL=5, $9F2C
                    return dcValue((uint8_t)i);
                case 0x18:   // DCSEL=6, $9F29: reading resets the accumulator
                    if (sideEffects) m_fxMultAccumulator = 0;
                    break;
                case 0x19:   // DCSEL=6, $9F2A: reading accumulates
                    if (sideEffects) {
                        int32_t m = fxMultiply();
                        if (m_fxSubtract) m_fxMultAccumulator -= m;
                        else m_fxMultAccumulator += m;
                    }
                    break;
                default:
                    break;
            }
            // the rest is write-only and reads as the version string
            return VERA_VERSION_STRING[i % 4];
        }
        case 0x0D: case 0x0E: case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13:
            return m_regLayer[0][reg - 0x0D];
        case 0x14: case 0x15: case 0x16: case 0x17: case 0x18: case 0x19: case 0x1A:
            return m_regLayer[1][reg - 0x14];

        case 0x1B: return m_pcm.readCtrl();
        case 0x1C: return m_pcm.readRate();
        case 0x1D: return 0;

        case 0x1E:
        case 0x1F: return spiRead(reg & 1, sideEffects);
    }
    return 0;
}

void Vera::writeReg(uint8_t reg, uint8_t value) {
    // video_write()
    reg &= 0x1F;
    switch (reg) {
        case 0x00:
            if (m_fx2bitPoly && m_fx4bitMode && m_fxAddr1Mode == 2 && m_ioAddrsel == 1) {
                m_fx2bitPoking = true;
                m_ioAddr[1] = (m_ioAddr[1] & 0x1fffc) | (value & 0x3);
            } else {
                m_ioAddr[m_ioAddrsel] = (m_ioAddr[m_ioAddrsel] & 0x1ff00) | value;
                if (m_fx16bitHop && m_ioAddrsel == 1) m_fx16bitHopAlign = value & 3;
            }
            m_ioRddata[m_ioAddrsel] = videoSpaceRead(m_ioAddr[m_ioAddrsel]);
            break;
        case 0x01:
            m_ioAddr[m_ioAddrsel] = (m_ioAddr[m_ioAddrsel] & 0x100ff) | ((uint32_t)value << 8);
            m_ioRddata[m_ioAddrsel] = videoSpaceRead(m_ioAddr[m_ioAddrsel]);
            break;
        case 0x02:
            m_ioAddr[m_ioAddrsel] = (m_ioAddr[m_ioAddrsel] & 0x0ffff) | ((uint32_t)(value & 0x1) << 16);
            m_fxNibbleBit[m_ioAddrsel]  = (value >> 1) & 0x1;
            m_fxNibbleIncr[m_ioAddrsel] = (value >> 2) & 0x1;
            m_ioInc[m_ioAddrsel] = value >> 3;
            m_ioRddata[m_ioAddrsel] = videoSpaceRead(m_ioAddr[m_ioAddrsel]);
            break;
        case 0x03:
        case 0x04: {
            const uint8_t sel = reg - 3;
            if (m_fx2bitPoking && m_fxAddr1Mode) {
                m_fx2bitPoking = false;
                uint8_t& v = m_vram[m_ioAddr[1] & 0x1FFFF];
                const uint8_t c = m_fxCache[m_fxCacheByteIndex];
                switch (value >> 6) {
                    case 0: v = (uint8_t)((c & 0xc0) | (m_ioRddata[1] & 0x3f)); break;
                    case 1: v = (uint8_t)((c & 0x30) | (m_ioRddata[1] & 0xcf)); break;
                    case 2: v = (uint8_t)((c & 0x0c) | (m_ioRddata[1] & 0xf3)); break;
                    case 3: v = (uint8_t)((c & 0x03) | (m_ioRddata[1] & 0xfc)); break;
                }
                break;
            }

            if (m_midlineDataWrites) midline();
            bool nibble = m_fxNibbleBit[sel];
            uint32_t address = getAndIncAddress(sel, true);

            uint8_t wrdata;
            uint8_t ramWrdata[4];
            uint8_t nibbleMask[4];
            uint8_t cacheToUse[4];

            if (m_fxMultiplier) {
                int32_t m = fxMultiply();
                m = m_fxSubtract ? m_fxMultAccumulator - m : m_fxMultAccumulator + m;
                cacheToUse[0] = (uint8_t)(m & 0xff);
                cacheToUse[1] = (uint8_t)((m >> 8) & 0xff);
                cacheToUse[2] = (uint8_t)((m >> 16) & 0xff);
                cacheToUse[3] = (uint8_t)((m >> 24) & 0xff);
            } else {
                std::memcpy(cacheToUse, m_fxCache, 4);
            }

            wrdata = m_fxCacheByteCycling ? m_fxCache[m_fxCacheByteIndex] : value;

            for (int k = 0; k < 4; k++)
                ramWrdata[k] = (m_fxCacheWrite && !m_fxCacheByteCycling) ? cacheToUse[k] : wrdata;

            if (m_fxCacheWrite) {
                address &= 0x1fffc;
                for (int k = 0; k < 4; k++) {
                    if (m_fxTransWrites) {
                        if (m_fx4bitMode)
                            nibbleMask[k] = (uint8_t)((((ramWrdata[k] & 0xf0) == 0) << 1) | ((ramWrdata[k] & 0x0f) == 0));
                        else
                            nibbleMask[k] = (ramWrdata[k] != 0) ? 0 : 3;
                    } else {
                        nibbleMask[k] = (value >> (2 * k)) & 0x3;
                    }
                }
                for (int k = 0; k < 4; k++) fxVramCacheWrite(address + k, ramWrdata[k], nibbleMask[k]);
            } else {
                fxVideoSpaceWrite(address, nibble, wrdata);   // normal write
            }

            m_ioRddata[sel] = videoSpaceRead(m_ioAddr[sel]);
            break;
        }
        case 0x05:
            if (value & 0x80) videoReset();
            m_ioDcsel = (value >> 1) & 0x3f;
            m_ioAddrsel = value & 1;
            break;
        case 0x06:
            m_irqLine9 = (uint16_t)((m_irqLine9 & 0xFF) | ((value >> 7) << 8));
            m_ien = value & 0xF;
            break;
        case 0x07:
            // The emulator clears any written bit, the collision bits included;
            // the docs call bits 7-4 read-only.
            m_isr &= (uint8_t)(value ^ 0xff);
            break;
        case 0x08:
            m_irqLine9 = (uint16_t)((m_irqLine9 & 0x100) | value);
            break;

        case 0x09: case 0x0A: case 0x0B: case 0x0C: {
            midline();   // potential mid-line raster effect
            int i = reg - 0x09 + (m_ioDcsel << 2);
            if (i == 0) {
                // progressive field 0 -> 1, or VGA -> another mode with
                // progressive on, clears the framebuffer
                if (((m_regComposer[0] & 0x8) == 0 && (value & 0x8)) ||
                    ((m_regComposer[0] & 0x3) == 1 && (value & 0x3) > 1 && (value & 0x8))) {
                    std::fill(m_framebuffer.begin(), m_framebuffer.end(), 0u);
                }
                // the interlace field bit is read-only
                m_regComposer[0] = (uint8_t)((m_regComposer[0] & ~0x7f) | (value & 0x7f));
                m_paletteDirty = true;
            } else {
                m_regComposer[i] = value;
            }

            auto incrFrom = [&](int lo, int hi) -> uint32_t {
                uint32_t base = ((uint32_t)(m_regComposer[hi] & 0x7f) << 15) + ((uint32_t)m_regComposer[lo] << 7);
                if (m_regComposer[hi] & 0x40) base |= 0xffc00000u;   // sign extend
                return base << (5 * (!!(m_regComposer[hi] & 0x80)));  // x32
            };

            switch (i) {
                case 0x08:   // DCSEL=2 $9F29 FX_CTRL
                    m_fxAddr1Mode = value & 0x03;
                    m_fx4bitMode = (value & 0x04) >> 2;
                    m_fx16bitHop = (value & 0x08) >> 3;
                    m_fxCacheByteCycling = (value & 0x10) >> 4;
                    m_fxCacheFill = (value & 0x20) >> 5;
                    m_fxCacheWrite = (value & 0x40) >> 6;
                    m_fxTransWrites = (value & 0x80) >> 7;
                    break;
                case 0x09:   // DCSEL=2 $9F2A FX_TILEBASE
                    m_fxAffineTileBase = (uint32_t)(value & 0xfc) << 9;
                    m_fxAffineClip = (value & 0x02) >> 1;
                    m_fx2bitPoly = (value & 0x01);
                    break;
                case 0x0a:   // DCSEL=2 $9F2B FX_MAPBASE
                    m_fxAffineMapBase = (uint32_t)(value & 0xfc) << 9;
                    m_fxAffineMapSize = (uint8_t)(2 << ((value & 0x03) << 1));
                    break;
                case 0x0b:   // DCSEL=2 $9F2C FX_MULT
                    m_fxCacheIncrementMode = value & 0x01;
                    m_fxCacheNibbleIndex = (value & 0x02) >> 1;
                    m_fxCacheByteIndex = (value & 0x0c) >> 2;
                    m_fxMultiplier = (value & 0x10) >> 4;
                    m_fxSubtract = (value & 0x20) >> 5;
                    if (value & 0x40) {   // accumulate
                        int32_t m = fxMultiply();
                        if (m_fxSubtract) m_fxMultAccumulator -= m;
                        else m_fxMultAccumulator += m;
                    }
                    if (value & 0x80) m_fxMultAccumulator = 0;   // reset accumulator
                    break;
                case 0x0c:   // DCSEL=3 $9F29 FX_X_INCR_L
                    m_fxXPixelIncrement = incrFrom(0x0c, 0x0d);
                    break;
                case 0x0d:   // DCSEL=3 $9F2A FX_X_INCR_H
                    m_fxXPixelIncrement = incrFrom(0x0c, 0x0d);
                    if (m_fxAddr1Mode == 1 || m_fxAddr1Mode == 2)   // subpixel to 0.5
                        m_fxXPixelPosition = (m_fxXPixelPosition & 0x07ff0000) | 0x00008000;
                    break;
                case 0x0e:   // DCSEL=3 $9F2B FX_Y_INCR_L
                    m_fxYPixelIncrement = incrFrom(0x0e, 0x0f);
                    break;
                case 0x0f:   // DCSEL=3 $9F2C FX_Y_INCR_H
                    m_fxYPixelIncrement = incrFrom(0x0e, 0x0f);
                    if (m_fxAddr1Mode == 1 || m_fxAddr1Mode == 2)
                        m_fxYPixelPosition = (m_fxYPixelPosition & 0x07ff0000) | 0x00008000;
                    break;
                case 0x10:   // DCSEL=4 $9F29 FX_X_POS_L
                    m_fxXPixelPosition = (m_fxXPixelPosition & 0x0700ff80) | ((uint32_t)value << 16);
                    fxAffinePrefetch();
                    break;
                case 0x11:   // DCSEL=4 $9F2A FX_X_POS_H
                    m_fxXPixelPosition = (m_fxXPixelPosition & 0x00ffff00) | ((uint32_t)(value & 0x7) << 24) | (value & 0x80);
                    fxAffinePrefetch();
                    break;
                case 0x12:   // DCSEL=4 $9F2B FX_Y_POS_L
                    m_fxYPixelPosition = (m_fxYPixelPosition & 0x0700ff80) | ((uint32_t)value << 16);
                    fxAffinePrefetch();
                    break;
                case 0x13:   // DCSEL=4 $9F2C FX_Y_POS_H
                    m_fxYPixelPosition = (m_fxYPixelPosition & 0x00ffff00) | ((uint32_t)(value & 0x7) << 24) | (value & 0x80);
                    fxAffinePrefetch();
                    break;
                case 0x14:   // DCSEL=5 $9F29 FX_X_POS_S
                    m_fxXPixelPosition = (m_fxXPixelPosition & 0x07ff0080) | ((uint32_t)value << 8);
                    break;
                case 0x15:   // DCSEL=5 $9F2A FX_Y_POS_S
                    m_fxYPixelPosition = (m_fxYPixelPosition & 0x07ff0080) | ((uint32_t)value << 8);
                    break;
                case 0x18: m_fxCache[0] = value; break;   // DCSEL=6 FX_CACHE_L
                case 0x19: m_fxCache[1] = value; break;   // FX_CACHE_M
                case 0x1a: m_fxCache[2] = value; break;   // FX_CACHE_H
                case 0x1b: m_fxCache[3] = value; break;   // FX_CACHE_U
            }
            break;
        }

        case 0x0D: case 0x0E: case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13:
            midline();
            m_regLayer[0][reg - 0x0D] = value;
            refreshLayerProperties(0);
            break;
        case 0x14: case 0x15: case 0x16: case 0x17: case 0x18: case 0x19: case 0x1A:
            midline();
            m_regLayer[1][reg - 0x14] = value;
            refreshLayerProperties(1);
            break;

        // audio_render() before these in the emulator: tick() has already
        // produced every sample up to now
        case 0x1B: m_pcm.writeCtrl(value); break;
        case 0x1C: m_pcm.writeRate(value); break;
        case 0x1D: m_pcm.writeFifo(value); break;

        case 0x1E:
        case 0x1F:
            spiWrite(reg & 1, value);
            break;
    }
}

bool Vera::ioRead(IBus*, uint32_t addr, uint8_t* val) {
    *val = readReg((uint8_t)(addr & 0x1F), true);
    updateIrq();
    return true;
}

bool Vera::ioPeek(IBus*, uint32_t addr, uint8_t* val) {
    *val = readReg((uint8_t)(addr & 0x1F), false);
    return true;
}

bool Vera::ioWrite(IBus*, uint32_t addr, uint8_t val) {
    writeReg((uint8_t)(addr & 0x1F), val);
    updateIrq();
    return true;
}

// ---------------------------------------------------------------------------
// SPI (vera_spi.c), no SD card attached
// ---------------------------------------------------------------------------

void Vera::spiStep(uint64_t cycles) {
    if (!m_spiBusy) return;
    m_spiOutcounter += (double)cycles * SPI_CLOCK_RATE_MHZ / ((double)m_cpuHz / 1e6);
    if (m_spiOutcounter >= 10) {
        // 9 is closer to reality; 10 is safe against clock alignment on hardware
        m_spiBusy = false;
        m_spiReceivedByte = 0xff;   // no card
    }
}

uint8_t Vera::spiRead(uint8_t reg, bool sideEffects) {
    switch (reg) {
        case 0:
            if (sideEffects && m_spiAutotx && m_spiSs && !m_spiBusy) {
                // autotx sends $FF after each read
                m_spiSendingByte = 0xff;
                m_spiBusy = true;
                m_spiOutcounter = 0;
            }
            return m_spiReceivedByte;
        case 1:
            return (uint8_t)(m_spiBusy << 7 | m_spiAutotx << 2 | m_spiSs);
    }
    return 0;
}

void Vera::spiWrite(uint8_t reg, uint8_t value) {
    switch (reg) {
        case 0:
            if (m_spiSs && !m_spiBusy) {
                m_spiSendingByte = value;
                m_spiBusy = true;
                m_spiOutcounter = 0;
            }
            break;
        case 1:
            m_spiSs = value & 1;   // sdcard_select() with no card does nothing
            m_spiAutotx = !!(value & 4);
            break;
    }
}

// ---------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------

void Vera::renderAudioSample() {
    int16_t psgL, psgR, pcmL, pcmR;
    m_psg.render(&psgL, &psgR);
    m_pcm.render(&pcmL, &pcmR);

    // audio.c: mix = (psg + pcm) * 2 (the loudest single PSG voice is 1/8 of
    // full scale), through its limiter
    int32_t mixL = ((int32_t)psgL + pcmL) * 2;
    int32_t mixR = ((int32_t)psgR + pcmR) * 2;
    if (m_externalAudio) m_externalAudio(mixL, mixR);   // + ym
    uint32_t amp = (uint32_t)std::max(std::abs(mixL), std::abs(mixR));
    if (amp > 32767) {
        uint32_t newAmp = (32767u << 16) / amp;
        m_limiterAmp = std::min(newAmp, m_limiterAmp);
    }
    int16_t outL = (int16_t)(((int64_t)mixL * m_limiterAmp) >> 16);
    int16_t outR = (int16_t)(((int64_t)mixR * m_limiterAmp) >> 16);
    if (m_limiterAmp < (1u << 16)) m_limiterAmp++;

    if (m_audioCount < AUDIO_BUF) {
        m_audioBuf[m_audioWrite * 2]     = outL / 32768.0f;
        m_audioBuf[m_audioWrite * 2 + 1] = outR / 32768.0f;
        m_audioWrite = (m_audioWrite + 1) % AUDIO_BUF;
        ++m_audioCount;
    }
}

void Vera::audioStep(uint64_t cycles) {
    // one sample every 512 pixel clocks
    m_audioAcc += cycles * PIXEL_CLOCK_HZ;
    const uint64_t perSample = 512ull * m_cpuHz;
    while (m_audioAcc >= perSample) {
        m_audioAcc -= perSample;
        renderAudioSample();
    }
}

int Vera::pullSamples(float* buffer, int maxSamples) {
    int n = std::min(maxSamples, m_audioCount);
    for (int i = 0; i < n; ++i) {
        buffer[i] = 0.5f * (m_audioBuf[m_audioRead * 2] + m_audioBuf[m_audioRead * 2 + 1]);
        m_audioRead = (m_audioRead + 1) % AUDIO_BUF;
    }
    m_audioCount -= n;
    return n;
}

int Vera::pullStereoSamples(float* lr, int maxFrames) {
    int n = std::min(maxFrames, m_audioCount);
    for (int i = 0; i < n; ++i) {
        lr[i * 2]     = m_audioBuf[m_audioRead * 2];
        lr[i * 2 + 1] = m_audioBuf[m_audioRead * 2 + 1];
        m_audioRead = (m_audioRead + 1) % AUDIO_BUF;
    }
    m_audioCount -= n;
    return n;
}

// ---------------------------------------------------------------------------
// Tick / debugger
// ---------------------------------------------------------------------------

void Vera::tick(uint64_t cycles) {
    if (cycles == 0) return;
    videoStep(cycles);
    audioStep(cycles);
    spiStep(cycles);
    updateIrq();
}

void Vera::getDeviceInfo(DeviceInfo& out) const {
    out.name = m_name;
    out.baseAddr = m_baseAddr;
    out.addrMask = addrMask();

    static const char* const NAMES[32] = {
        "ADDRx_L", "ADDRx_M", "ADDRx_H", "DATA0", "DATA1", "CTRL", "IEN", "ISR",
        "IRQLINE_L/SCANLINE_L", "DC_VIDEO/HSTART/FX_CTRL", "DC_HSCALE/HSTOP", "DC_VSCALE/VSTART", "DC_BORDER/VSTOP",
        "L0_CONFIG", "L0_MAPBASE", "L0_TILEBASE", "L0_HSCROLL_L", "L0_HSCROLL_H", "L0_VSCROLL_L", "L0_VSCROLL_H",
        "L1_CONFIG", "L1_MAPBASE", "L1_TILEBASE", "L1_HSCROLL_L", "L1_HSCROLL_H", "L1_VSCROLL_L", "L1_VSCROLL_H",
        "AUDIO_CTRL", "AUDIO_RATE", "AUDIO_DATA", "SPI_DATA", "SPI_CTRL"
    };
    Vera* self = const_cast<Vera*>(this);   // readReg(…, false) changes nothing
    for (uint8_t r = 0; r < 32; r++) {
        out.registers.push_back({NAMES[r], r, self->readReg(r, false), "", {}});
    }

    char buf[64];
    auto add = [&](const char* k, const char* fmt, auto v) {
        std::snprintf(buf, sizeof(buf), fmt, v);
        out.state.push_back({k, buf});
    };
    add("ADDR0", "$%05X", (unsigned)(m_ioAddr[0] & 0x1FFFF));
    add("ADDR1", "$%05X", (unsigned)(m_ioAddr[1] & 0x1FFFF));
    add("INC0", "%d", INCREMENTS[m_ioInc[0]]);
    add("INC1", "%d", INCREMENTS[m_ioInc[1]]);
    add("ADDRSEL", "%u", (unsigned)m_ioAddrsel);
    add("DCSEL", "%u", (unsigned)m_ioDcsel);
    add("Scanline", "%u", (unsigned)(m_regComposer[0] & 2 ? m_ntscScanPosY : m_vgaScanPosY));
    add("IRQ line", "%u", (unsigned)m_irqLine9);
    add("IEN", "$%02X", (unsigned)m_ien);
    add("ISR", "$%02X", (unsigned)isr());
    add("DC_VIDEO", "$%02X", (unsigned)m_regComposer[0]);
    add("HSCALE", "%u", (unsigned)m_regComposer[1]);
    add("VSCALE", "%u", (unsigned)m_regComposer[2]);
    add("BORDER", "%u", (unsigned)m_regComposer[3]);
    add("HSTART", "%u", (unsigned)(m_regComposer[4] << 2));
    add("HSTOP", "%u", (unsigned)(m_regComposer[5] << 2));
    add("VSTART", "%u", (unsigned)(m_regComposer[6] << 1));
    add("VSTOP", "%u", (unsigned)(m_regComposer[7] << 1));
    add("FX_CTRL", "$%02X", (unsigned)m_regComposer[8]);
    add("FX_MULT", "$%02X", (unsigned)dcValue(0x0b));
    add("FX_X_POS", "$%08X", (unsigned)m_fxXPixelPosition);
    add("FX_Y_POS", "$%08X", (unsigned)m_fxYPixelPosition);
    add("FX_X_INCR", "$%08X", (unsigned)m_fxXPixelIncrement);
    add("FX_Y_INCR", "$%08X", (unsigned)m_fxYPixelIncrement);
    std::snprintf(buf, sizeof(buf), "%02X %02X %02X %02X", m_fxCache[0], m_fxCache[1], m_fxCache[2], m_fxCache[3]);
    out.state.push_back({"FX cache", buf});
    add("FX accumulator", "%d", (int)m_fxMultAccumulator);
    add("PCM FIFO", "%u", m_pcm.fifoCount());
    add("Frames", "%llu", (unsigned long long)m_frameCount);
}

std::vector<std::pair<std::string, uint32_t>> Vera::getDerivedValues() const {
    return {
        {"ADDR0", m_ioAddr[0] & 0x1FFFF},
        {"ADDR1", m_ioAddr[1] & 0x1FFFF},
        {"SCANLINE", scanline()},
        {"ISR", isr()},
        {"IEN", m_ien},
        {"DCSEL", m_ioDcsel},
    };
}
