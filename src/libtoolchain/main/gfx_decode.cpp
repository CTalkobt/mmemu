#include "gfx_decode.h"
#include <algorithm>
#include <cctype>

namespace gfx {

const std::vector<FormatInfo>& formats() {
    static const std::vector<FormatInfo> s = {
        // fmt                 id            label                          w   h  pw bpp bpr stride sprite lowNib
        {Format::SpriteHires, "sprite",     "Sprite (hires)",             24, 21, 1, 1, 3,   64,  true,  false},
        {Format::SpriteMulti, "sprite-mc",  "Sprite (multicolour)",       12, 21, 2, 2, 3,   64,  true,  false},
        {Format::Sprite64,    "sprite64",   "Sprite 64-wide (VIC-IV)",    64, 21, 1, 1, 8,  192,  true,  false},
        {Format::Sprite16,    "sprite16",   "Sprite 16-colour (VIC-IV)",  16, 21, 1, 4, 8,  192,  true,  false},
        {Format::Sprite16Narrow, "sprite16-6", "Sprite 16-colour 6-wide (VIC-IV)", 6, 21, 1, 4, 3, 64, true, false},
        {Format::CharHires,   "char",       "Character (hires)",           8,  8, 1, 1, 1,    8,  false, false},
        {Format::CharMulti,   "char-mc",    "Character (multicolour)",     4,  8, 2, 2, 1,    8,  false, false},
        {Format::CharFCM,     "char-fcm",   "Character FCM (VIC-IV)",      8,  8, 1, 8, 8,   64,  false, false},
        {Format::CharNCM,     "char-ncm",   "Character NCM (VIC-IV)",     16,  8, 1, 4, 8,   64,  false, true},
    };
    return s;
}

const FormatInfo& info(Format f) {
    for (const auto& i : formats())
        if (i.format == f) return i;
    return formats().front();
}

bool formatById(const std::string& id, Format& out) {
    std::string lower = id;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    for (const auto& i : formats()) {
        if (lower == i.id) { out = i.format; return true; }
    }
    return false;
}

std::vector<uint8_t> decode(Format f, const ReadFn& read, uint32_t addr, bool swapNibbles, int rows) {
    const FormatInfo& fi = info(f);
    const int height = rows > 0 ? rows : fi.height;
    std::vector<uint8_t> out((size_t)fi.width * height, 0);
    const int pixelsPerByte = 8 / fi.bitsPerPixel;
    const uint8_t mask = (uint8_t)((1u << fi.bitsPerPixel) - 1);
    bool lowFirst = fi.lowNibbleFirst != swapNibbles;

    for (int r = 0; r < height; ++r) {
        for (int b = 0; b < fi.bytesPerRow; ++b) {
            uint8_t byte = read(addr + (uint32_t)(r * fi.bytesPerRow + b));
            for (int p = 0; p < pixelsPerByte; ++p) {
                int x = b * pixelsPerByte + p;
                if (x >= fi.width) break;
                int shift;
                if (fi.bitsPerPixel == 4 && lowFirst) shift = p * 4;
                else shift = 8 - fi.bitsPerPixel * (p + 1);   // MSB-first
                out[(size_t)r * fi.width + x] = (uint8_t)((byte >> shift) & mask);
            }
        }
    }
    return out;
}

uint32_t c64Palette(int index) {
    static const uint32_t p[16] = {
        0xFF000000, 0xFFFFFFFF, 0xFF883932, 0xFF67B6BD, 0xFF8B3F96, 0xFF55A049, 0xFF40318D, 0xFFBFCE72,
        0xFF8B5429, 0xFF574200, 0xFFB86962, 0xFF505050, 0xFF787878, 0xFF94E089, 0xFF7869C4, 0xFF9F9F9F,
    };
    return p[index & 0x0F];
}

static uint32_t colorFor(const FormatInfo& fi, uint8_t v, const Colors& c) {
    switch (fi.bitsPerPixel) {
        case 1: return c.slot[v & 1];
        case 2: return c.slot[v & 3];
        case 4:
            if (v == 0 && fi.isSprite) return c.transparent;
            return c.palette ? c.palette((c.paletteBase + v) & 0xFF) : c64Palette(v);
        default:
            return c.palette ? c.palette(v) : c64Palette(v);
    }
}

Sheet renderSheet(Format f, const ReadFn& read, uint32_t start, int count, int columns,
                  uint32_t stride, const Colors& colors, bool swapNibbles, int gap, uint32_t gridColor,
                  int rows) {
    const FormatInfo& fi = info(f);
    const int height = rows > 0 ? rows : fi.height;
    Sheet s;
    s.count = std::max(0, count);
    s.columns = std::max(1, std::min(columns, std::max(1, s.count)));
    s.rows = (s.count + s.columns - 1) / s.columns;
    s.cellW = fi.width * fi.pixelWidth;
    s.cellH = height;
    s.gap = std::max(0, gap);
    s.start = start;
    s.stride = stride ? stride : fi.stride;
    s.width = s.columns * s.cellW + (s.columns + 1) * s.gap;
    s.height = s.rows * s.cellH + (s.rows + 1) * s.gap;
    if (s.count == 0) { s.width = s.height = 0; return s; }
    s.argb.assign((size_t)s.width * s.height, gridColor);

    for (int i = 0; i < s.count; ++i) {
        int ox = s.gap + (i % s.columns) * (s.cellW + s.gap);
        int oy = s.gap + (i / s.columns) * (s.cellH + s.gap);
        auto px = decode(f, read, itemAddress(s, i), swapNibbles, height);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < fi.width; ++x) {
                uint32_t col = colorFor(fi, px[(size_t)y * fi.width + x], colors);
                for (int k = 0; k < fi.pixelWidth; ++k)
                    s.argb[(size_t)(oy + y) * s.width + ox + x * fi.pixelWidth + k] = col;
            }
        }
    }
    return s;
}

int itemAt(const Sheet& s, int x, int y) {
    if (s.count == 0 || x < 0 || y < 0 || x >= s.width || y >= s.height) return -1;
    int cx = x - s.gap, cy = y - s.gap;
    if (cx < 0 || cy < 0) return -1;
    int col = cx / (s.cellW + s.gap), row = cy / (s.cellH + s.gap);
    if (cx % (s.cellW + s.gap) >= s.cellW || cy % (s.cellH + s.gap) >= s.cellH) return -1;
    if (col >= s.columns) return -1;
    int idx = row * s.columns + col;
    return idx < s.count ? idx : -1;
}

} // namespace gfx
