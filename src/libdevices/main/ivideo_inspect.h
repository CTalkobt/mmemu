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
    };

    struct CharsetInfo {
        uint32_t addr = 0;              ///< character set (or bitmap) base
        bool     multicolor = false;
        bool     bitmap = false;
        bool     fullColor = false;     ///< VIC-IV FCM for characters <= $FF
        uint8_t  bg[4] = {0, 0, 0, 0};  ///< background colour registers 0-3
    };

    /** Number of palette entries (16 for a VIC-II, 256 for a VIC-III/IV). */
    virtual int paletteSize() const = 0;
    /** Palette entry as 0xAARRGGBB. */
    virtual uint32_t paletteColor(int index) const = 0;
    /** Read memory as the chip sees it (e.g. the character ROM shadow). */
    virtual uint8_t peekVideo(uint32_t addr) const = 0;

    virtual int  spriteCount() const { return 0; }
    virtual bool spriteInfo(int /*n*/, SpriteInfo& /*out*/) const { return false; }
    virtual bool charsetInfo(CharsetInfo& /*out*/) const { return false; }
};
