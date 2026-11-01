#pragma once

#include <cstdint>
#include <string>
#include <vector>

/**
 * Interface for devices that produce video output (e.g. VIC-I, VIC-II, VDC).
 */
class IVideoOutput {
public:
    virtual ~IVideoOutput() {}

    /**
     * Information about the current frame geometry.
     */
    struct VideoDimensions {
        int width;
        int height;
        int displayWidth;  // Visible part
        int displayHeight;
    };

    virtual VideoDimensions getDimensions() const = 0;

    /**
     * Render the current frame. Every pixel is a uint32_t 0xAARRGGBB (alpha in
     * the top byte, then red, green, blue), whatever the chip.
     * @param buffer Pointer to the destination buffer (width * height pixels).
     */
    virtual void renderFrame(uint32_t* buffer) = 0;

    /** Convert 0xAARRGGBB pixels to R,G,B,A bytes (as PNG writers expect). */
    static std::vector<uint8_t> toRgbaBytes(const uint32_t* argb, size_t count);

    /**
     * Export the current frame to a PNG file.
     * @param filename The path to save the PNG file to.
     * @return True if successful, false otherwise.
     */
    virtual bool exportPng(const std::string& filename);
};
