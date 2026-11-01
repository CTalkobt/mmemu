#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

/**
 * Decoding of sprite and character graphics from memory (#28).
 *
 * Pure functions over a byte-read callback, so the GUI Graphics pane, the CLI
 * `gfx` command and tests share one implementation.
 */
namespace gfx {

enum class Format {
    SpriteHires,   ///< VIC-II sprite, 24x21, 1 bpp, 3 bytes per row
    SpriteMulti,   ///< VIC-II multicolour sprite, 12x21 double-wide pixels, 2 bpp
    Sprite64,      ///< VIC-IV 64-wide sprite (SPRX64EN), 64x21, 1 bpp, 8 bytes per row
    Sprite16,      ///< VIC-IV 16-colour sprite with SPRX64EN, 16x21, 4 bpp, 8 bytes per row
    CharHires,     ///< 8x8 character, 1 bpp
    CharMulti,     ///< VIC-II multicolour character, 4x8 double-wide pixels, 2 bpp
    CharFCM,       ///< VIC-IV full-colour character, 8x8, 8 bpp (64 bytes)
    CharNCM,       ///< VIC-IV nybl-colour character, 16x8, 4 bpp (64 bytes)
};

struct FormatInfo {
    Format      format;
    const char* id;            ///< short name for CLI/MCP ("sprite", "char-mc", ...)
    const char* label;         ///< display name
    int width;                 ///< pixels per row (logical pixels)
    int height;                ///< rows
    int pixelWidth;            ///< output pixels per logical pixel (2 for multicolour)
    int bitsPerPixel;          ///< 1, 2, 4 or 8
    int bytesPerRow;
    uint32_t stride;           ///< default bytes from one item to the next
    bool isSprite;
    bool lowNibbleFirst;       ///< 4 bpp: low nibble is the left pixel
};

/** All formats, in menu order. */
const std::vector<FormatInfo>& formats();
const FormatInfo& info(Format f);
/** Look up by id (case-insensitive); false if unknown. */
bool formatById(const std::string& id, Format& out);

using ReadFn = std::function<uint8_t(uint32_t)>;

/**
 * Raw pixel values of one item, row-major, info(f).width * info(f).height
 * entries: the bit (1 bpp), bit pair (2 bpp), nibble (4 bpp) or byte (8 bpp).
 * swapNibbles reverses the format's default nibble order; rows > 0 overrides
 * the height (VIC-IV extended-height sprites), giving width * rows entries.
 */
std::vector<uint8_t> decode(Format f, const ReadFn& read, uint32_t addr, bool swapNibbles = false,
                            int rows = 0);

/** How raw values become colours (0xAARRGGBB). */
struct Colors {
    /**
     * 1 bpp: slot[0] = background, slot[1] = foreground.
     * 2 bpp: slot[value] (sprite: 0 bg, 1 MC0, 2 sprite colour, 3 MC1;
     *        char: 0 bg0, 1 bg1, 2 bg2, 3 colour RAM).
     */
    uint32_t slot[4] = {0xFF000000, 0xFFFFFFFF, 0xFF888888, 0xFFCCCCCC};
    /** 4/8 bpp: palette lookup; 4 bpp adds paletteBase. */
    std::function<uint32_t(int)> palette;
    int paletteBase = 0;
    /** Background shown for value 0 of 4 bpp sprites (transparent). */
    uint32_t transparent = 0xFF000000;
};

/** The default 16-colour VIC-II palette (0xAARRGGBB). */
uint32_t c64Palette(int index);

struct Sheet {
    int width = 0, height = 0;          ///< in output pixels
    int columns = 1, rows = 0;
    int cellW = 0, cellH = 0;           ///< item size in output pixels (without gap)
    int gap = 1;
    int count = 0;
    uint32_t start = 0, stride = 0;
    std::vector<uint32_t> argb;         ///< width * height
};

/**
 * Lay out `count` items starting at `start`, `stride` bytes apart, in a grid
 * of `columns`, separated by `gap` pixels of gridColor.
 */
Sheet renderSheet(Format f, const ReadFn& read, uint32_t start, int count, int columns,
                  uint32_t stride, const Colors& colors, bool swapNibbles = false,
                  int gap = 1, uint32_t gridColor = 0xFF303030, int rows = 0);

/** Index of the item under output pixel (x, y), or -1 for a gap or outside. */
int itemAt(const Sheet& s, int x, int y);

/** Address of item `index` in the sheet. */
inline uint32_t itemAddress(const Sheet& s, int index) { return s.start + (uint32_t)index * s.stride; }

} // namespace gfx
