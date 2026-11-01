#include "ivideo_output.h"
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "util/stb_image_write.h"

bool IVideoOutput::exportPng(const std::string& filename) {
    VideoDimensions dims = getDimensions();
    if (dims.width <= 0 || dims.height <= 0) {
        return false;
    }
    std::vector<uint32_t> buffer(dims.width * dims.height);
    renderFrame(buffer.data());

    // stbi_write_png expects R,G,B,A bytes; frames are 0xAARRGGBB words
    std::vector<uint8_t> rgba = toRgbaBytes(buffer.data(), buffer.size());
    return stbi_write_png(filename.c_str(), dims.width, dims.height, 4, rgba.data(), dims.width * 4);
}

std::vector<uint8_t> IVideoOutput::toRgbaBytes(const uint32_t* argb, size_t count) {
    std::vector<uint8_t> out(count * 4);
    for (size_t i = 0; i < count; ++i) {
        uint32_t px = argb[i];
        out[i * 4]     = (px >> 16) & 0xFF;
        out[i * 4 + 1] = (px >> 8) & 0xFF;
        out[i * 4 + 2] = px & 0xFF;
        out[i * 4 + 3] = (px >> 24) & 0xFF;
    }
    return out;
}

#include "ivideo_inspect.h"
IVideoInspect::~IVideoInspect() = default;
