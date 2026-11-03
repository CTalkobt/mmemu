#pragma once

#include <cstdint>
#include <string>
#include <vector>

/**
 * Minimal GIF89a writer for debugger snapshots (#30 SID scope).
 *
 * Frames are 0xAARRGGBB (alpha ignored) and must share one size. All frames
 * together may use at most 256 distinct colours, which become one global
 * palette: no quantisation or dithering. With more than one frame the GIF
 * loops forever.
 */
namespace gif {

struct Frame {
    std::vector<uint32_t> argb;   ///< width * height pixels
    int delayCs = 10;             ///< display time in 1/100 s (ignored for a single frame)
};

/**
 * Encode frames as a GIF. On failure returns an empty vector and sets err
 * (no frames, size mismatch, or more than 256 colours).
 */
std::vector<uint8_t> encode(const std::vector<Frame>& frames, int width, int height, std::string* err = nullptr);

/** encode() and write to path. */
bool write(const std::string& path, const std::vector<Frame>& frames, int width, int height, std::string* err = nullptr);

} // namespace gif
