#pragma once

#include <cstdint>

/**
 * Optional interface for video chips whose graphics can be inspected by
 * debugger tools such as the GUI Graphics pane (#28). Obtain it with
 * dynamic_cast from the chip's IOHandler.
 *
 * Addresses are the ones the chip fetches from: 16-bit CPU-style addresses
 * for a VIC-II (bank base included), 28-bit physical addresses for a VIC-IV
 * in MEGA65 mode.
 */
class IVideoInspect {
public:
    virtual ~IVideoInspect();

    struct SpriteInfo {
        bool     enabled = false;
        uint32_t dataAddr = 0;
        int      height = 21;
        bool     multicolor = false;
        bool     wide64 = false;        ///< VIC-IV SPRX64EN (8 bytes per row)
        bool     sixteenColor = false;  ///< VIC-IV 16-colour sprite
        uint8_t  color = 0;             ///< sprite colour register
        uint8_t  mc0 = 0, mc1 = 0;      ///< sprite multicolour registers
        int      paletteBase = 0;       ///< 16-colour sprites: index added to each nibble
        bool     playerMissile = false; ///< ANTIC/GTIA player or missiles: 8 pixels, 1 byte per row
        int      width = 0;             ///< VERA: width in pixels (8-64); 0 for the others
        int      bitsPerPixel = 0;      ///< VERA: 4 or 8
    };

    struct CharsetInfo {
        uint32_t addr = 0;              ///< character set (or bitmap) base
        bool     multicolor = false;
        bool     bitmap = false;
        bool     fullColor = false;     ///< VIC-IV FCM for characters <= $FF
        uint8_t  bg[4] = {0, 0, 0, 0};  ///< background colour registers 0-3
        /** ANTIC text modes: 2 colours (modes 2/3, 6/7) or 4 (modes 4/5, multicolor);
         *  bg[0..2] then give pixel values 0-2 and fg value 3 (or 1 in 2 colours). */
        bool     antic = false;
        /** TED: bg[0..3] are $FF15-$FF18 (colour bytes, luminance in bits 4-6);
         *  multicolour characters use bg[0..2] and the attribute colour (fg). */
        bool     ted = false;
        int      fg = -1;               ///< foreground palette index; -1 = VIC colour RAM (shown as colour 1)
        int      bitsPerPixel = 0;      ///< VERA tile layer: 1, 2, 4 or 8 (0 for the others)
        int      tileW = 8, tileH = 8;  ///< VERA tile size
    };

    /** Number of palette entries (16 for a VIC-II, 256 for a VIC-III/IV, 128 for a TED). */
    virtual int paletteSize() const = 0;
    /** Palette entry as 0xAARRGGBB. */
    virtual uint32_t paletteColor(int index) const = 0;
    /** Read memory as the chip sees it (e.g. the character ROM shadow). */
    virtual uint8_t peekVideo(uint32_t addr) const = 0;

    virtual int  spriteCount() const { return 0; }
    virtual bool spriteInfo(int /*n*/, SpriteInfo& /*out*/) const { return false; }
    virtual bool charsetInfo(CharsetInfo& /*out*/) const { return false; }
    /** Tile layers with their own character/tile set (VERA: 2). */
    virtual int  layerCount() const { return 1; }
    /** A layer's character/tile set; layer 0 is charsetInfo() unless overridden. */
    virtual bool layerCharsetInfo(int layer, CharsetInfo& out) const { return layer == 0 && charsetInfo(out); }
};
