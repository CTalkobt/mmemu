#include "pet_video.h"
#include <cstring>
#include <string>

void PetVideo::setLinkedDevice(const char* role, IOHandler* dev) {
    if (std::string(role) == "crtc")
        m_crtc = dynamic_cast<CRTC6545*>(dev);
}

PetVideo::PetVideo(Model model) : m_model(model) {
    PetVideo::reset();
}

void PetVideo::reset() {
    std::memset(m_vram, 0, sizeof(m_vram));
}

bool PetVideo::ioRead(IBus*, uint32_t addr, uint8_t* val) {
    if ((addr & ~0x07FFu) != baseAddr()) return false;
    *val = m_vram[addr & 0x07FF];
    return true;
}

bool PetVideo::ioWrite(IBus*, uint32_t addr, uint8_t val) {
    if ((addr & ~0x07FFu) != baseAddr()) return false;
    m_vram[addr & 0x07FF] = val;
    return true;
}

void PetVideo::tick(uint64_t) {
    // Timing is driven by the host or CRTC tick
}

IVideoOutput::VideoDimensions PetVideo::getDimensions() const {
    if (m_model == Model::PET_8000) {
        return { 80 * 8, 25 * 8, 80 * 8, 25 * 8 };
    } else {
        return { 40 * 8, 25 * 8, 40 * 8, 25 * 8 };
    }
}

void PetVideo::renderFrame(uint32_t* buffer) {
    auto dims = getDimensions();
    if (m_model == Model::PET_2001) {
        renderDiscrete(buffer, dims.width, dims.height);
    } else {
        renderCRTC(buffer, dims.width, dims.height);
    }
}

void PetVideo::renderDiscrete(uint32_t* buffer, int width, int height) {
    if (!m_charRom) return;

    for (int row = 0; row < 25; ++row) {
        for (int col = 0; col < 40; ++col) {
            uint8_t ch = m_vram[row * 40 + col];
            bool invert = (ch & 0x80);
            uint8_t charIdx = ch & 0x7F;

            for (int y = 0; y < 8; ++y) {
                uint8_t bits = m_charRom[charIdx * 8 + y];
                if (invert) bits = ~bits;

                uint32_t* line = buffer + ((row * 8 + y) * width) + (col * 8);
                for (int x = 0; x < 8; ++x) {
                    line[x] = (bits & (0x80 >> x)) ? m_pixelOn : m_pixelOff;
                }
            }
        }
    }
}

void PetVideo::renderCRTC(uint32_t* buffer, int width, int height) {
    if (!m_charRom || !m_crtc) return;

    int cols = (m_model == Model::PET_8000) ? 80 : 40;
    int rows = 25;

    for (int v = 0; v < rows; ++v) {
        for (int r = 0; r < 8; ++r) { // raster line in char
            for (int h = 0; h < cols; ++h) {
                // In a real CRTC this would be driven by MA/RA
                // For a simple frame renderer we calculate the VRAM offset
                uint8_t ch = m_vram[v * cols + h];
                bool invert = (ch & 0x80);
                uint8_t charIdx = ch & 0x7F;

                uint8_t bits = m_charRom[charIdx * 8 + r];
                if (invert) bits = ~bits;

                uint32_t* pixel = buffer + ((v * 8 + r) * width) + (h * 8);
                for (int x = 0; x < 8; ++x) {
                    pixel[x] = (bits & (0x80 >> x)) ? m_pixelOn : m_pixelOff;
                }
            }
        }
    }
}
