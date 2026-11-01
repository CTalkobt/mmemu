#pragma once

#include "libtoolchain/main/gfx_decode.h"
#include <string>

class IBus;
class IVideoInspect;
struct MachineDescriptor;

/**
 * Builds a graphics sheet (#28) from a machine: graphics at an address, a VIC
 * sprite, or the VIC's current character set, coloured with the video chip's
 * palette and colour registers when it supports IVideoInspect. Shared by the
 * GUI Graphics pane, the CLI `gfx` command and the MCP `render_graphics` tool.
 */
namespace gfx {

enum class Source { Address, Sprite, Charset };

struct ViewRequest {
    Source   source = Source::Address;
    int      sprite = 0;            ///< Source::Sprite
    uint32_t addr = 0;              ///< Source::Address
    Format   format = Format::SpriteHires;   ///< Source::Address (others pick it from the chip)
    int      count = 1;
    int      columns = 8;
    uint32_t stride = 0;            ///< 0 = the format's default
    bool     videoView = false;     ///< read memory as the video chip sees it
    bool     swapNibbles = false;
    int      paletteBase = -1;      ///< -1 = automatic (sprite number x 16 for 16-colour sprites)
};

struct View {
    Sheet       sheet;
    Format      format = Format::SpriteHires;
    uint32_t    start = 0;
    int         paletteBase = 0;
    std::string error;              ///< non-empty on failure
};

/** First IOHandler of the machine that implements IVideoInspect, or nullptr. */
IVideoInspect* findVideoInspect(MachineDescriptor* desc);

View buildView(const ViewRequest& req, IBus* cpuBus, IVideoInspect* video);

/** ASCII rendering of one item for terminals: '.' for 0, '#' for 1 bpp set pixels, digits/hex otherwise. */
std::string asciiArt(Format f, const ReadFn& read, uint32_t addr, bool swapNibbles = false, int rows = 0);

} // namespace gfx
