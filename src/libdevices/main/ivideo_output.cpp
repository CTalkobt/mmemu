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

    // stbi_write_png expects RGBA, so 4 components
    return stbi_write_png(filename.c_str(), dims.width, dims.height, 4, buffer.data(), dims.width * 4);
}

#include "ivideo_inspect.h"
IVideoInspect::~IVideoInspect() = default;
