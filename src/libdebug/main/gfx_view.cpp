#include "gfx_view.h"
#include "libcore/main/machine_desc.h"
#include "libdevices/main/io_handler.h"
#include "libdevices/main/io_registry.h"
#include "libdevices/main/ivideo_inspect.h"
#include "libmem/main/ibus.h"
#include <vector>

namespace gfx {

IVideoInspect* findVideoInspect(MachineDescriptor* desc) {
    if (!desc || !desc->ioRegistry) return nullptr;
    std::vector<IOHandler*> handlers;
    desc->ioRegistry->enumerate(handlers);
    for (auto* h : handlers)
        if (auto* v = dynamic_cast<IVideoInspect*>(h)) return v;
    return nullptr;
}

View buildView(const ViewRequest& req, IBus* cpuBus, IVideoInspect* video) {
    View v;
    if (!cpuBus) { v.error = "No machine."; return v; }
    if (req.source != Source::Address && !video) {
        v.error = "This machine's video chip can't be inspected.";
        return v;
    }

    auto color = [video](int i) {
        return (video && i >= 0 && i < video->paletteSize()) ? video->paletteColor(i) : c64Palette(i);
    };
    Colors colors;
    colors.palette = color;

    IVideoInspect::CharsetInfo ci;
    bool haveCs = video && video->charsetInfo(ci);
    IVideoInspect::SpriteInfo si0;
    bool haveSp = video && video->spriteInfo(0, si0);
    int bg0 = haveCs ? ci.bg[0] : 6;

    Format fmt = req.format;
    uint32_t start = req.addr;
    int rows = 0;
    int palBase = req.paletteBase >= 0 ? req.paletteBase : 0;

    if (req.source == Source::Sprite) {
        IVideoInspect::SpriteInfo si;
        if (!video->spriteInfo(req.sprite, si)) { v.error = "No such sprite."; return v; }
        start = si.dataAddr;
        fmt = si.sixteenColor ? Format::Sprite16
            : si.wide64       ? Format::Sprite64
            : si.multicolor   ? Format::SpriteMulti
                              : Format::SpriteHires;
        if (si.height != 21) rows = si.height;
        if (req.paletteBase < 0) palBase = si.paletteBase;
        colors.slot[1] = color(fmt == Format::SpriteMulti ? si.mc0 : si.color);
        colors.slot[2] = color(si.color);
        colors.slot[3] = color(si.mc1);
        colors.slot[0] = colors.transparent = color(bg0);
    } else if (req.source == Source::Charset) {
        if (!haveCs) { v.error = "The video chip has no character set."; return v; }
        start = ci.addr;
        fmt = ci.fullColor ? Format::CharFCM : ci.multicolor ? Format::CharMulti : Format::CharHires;
        colors.slot[0] = color(ci.bg[0]);
        colors.slot[1] = ci.multicolor ? color(ci.bg[1]) : color(1);
        colors.slot[2] = color(ci.bg[2]);
        colors.slot[3] = color(1);
    } else {
        const FormatInfo& fi = info(fmt);
        colors.slot[0] = colors.transparent = color(bg0);
        if (fi.isSprite) {
            colors.slot[1] = color(haveSp ? (fi.bitsPerPixel == 2 ? si0.mc0 : si0.color) : 1);
            colors.slot[2] = color(haveSp ? si0.color : 2);
            colors.slot[3] = color(haveSp ? si0.mc1 : 7);
        } else {
            colors.slot[1] = color(fi.bitsPerPixel == 2 ? (haveCs ? ci.bg[1] : 2) : 1);
            colors.slot[2] = color(haveCs ? ci.bg[2] : 7);
            colors.slot[3] = color(1);
        }
    }
    colors.paletteBase = palBase;

    ReadFn read;
    if (req.videoView && video) read = [video](uint32_t a) { return video->peekVideo(a); };
    else                        read = [cpuBus](uint32_t a) { return cpuBus->peek8(a); };

    v.sheet = renderSheet(fmt, read, start, req.count, req.columns, req.stride, colors,
                          req.swapNibbles, 1, 0xFF303030, rows);
    v.format = fmt;
    v.start = start;
    v.paletteBase = palBase;
    return v;
}

std::string asciiArt(Format f, const ReadFn& read, uint32_t addr, bool swapNibbles, int rows) {
    const FormatInfo& fi = info(f);
    int height = rows > 0 ? rows : fi.height;
    auto px = decode(f, read, addr, swapNibbles, height);
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < fi.width; ++x) {
            uint8_t val = px[(size_t)y * fi.width + x];
            char c;
            if (val == 0) c = '.';
            else if (fi.bitsPerPixel == 1) c = '#';
            else if (fi.bitsPerPixel <= 4) c = hex[val];
            else c = hex[val >> 4];   // 8 bpp: high nibble only, to keep one char per pixel
            for (int k = 0; k < fi.pixelWidth; ++k) out += c;
        }
        out += '\n';
    }
    return out;
}

} // namespace gfx
