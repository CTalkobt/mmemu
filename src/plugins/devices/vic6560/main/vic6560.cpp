#include "vic6560.h"
#include "libmem/main/ibus.h"
#include <cstring>

VIC6560::VIC6560(const std::string& name, uint32_t baseAddr)
    : m_name(name), m_baseAddr(baseAddr)
{
    VIC6560::reset();
}

// Constexpr array definitions (ODR-used by tickAudio)
constexpr uint32_t VIC6560::PRESCALER[4];

void VIC6560::reset() {
    std::memset(m_regs, 0, sizeof(m_regs));
    m_rasterCounter = 0;
    if (m_irqLine) m_irqLine->set(false);

    std::memset(m_voiceAccum, 0, sizeof(m_voiceAccum));
    std::memset(m_voiceOut,   0, sizeof(m_voiceOut));
    m_lfsr         = 0xACE1;
    m_sampleFrac   = 0;
    m_audioBufRead  = 0;
    m_audioBufWrite = 0;
    m_audioBufCount = 0;
}

bool VIC6560::ioRead(IBus* bus, uint32_t addr, uint8_t* val) {
    m_bus = bus;
    if ((addr & ~addrMask()) != m_baseAddr) return false;
    
    uint8_t reg = addr & 0xF;
    if (reg == 0x04) {
        // Return bits 0-7 of raster counter
        *val = (uint8_t)((m_rasterCounter / 65) & 0xFF);
    } else if (reg == 0x03) {
        // Bit 7 is bit 8 of raster counter
        uint8_t raster8 = (uint8_t)(((m_rasterCounter / 65) >> 8) & 0x01);
        *val = (m_regs[0x03] & 0x7F) | (raster8 << 7);
    } else {
        *val = m_regs[reg];
    }
    return true;
}

bool VIC6560::ioWrite(IBus* bus, uint32_t addr, uint8_t val) {
    m_bus = bus;
    if ((addr & ~addrMask()) != m_baseAddr) return false;
    
    uint8_t reg = addr & 0xF;
    m_regs[reg] = val;
    return true;
}

void VIC6560::tick(uint64_t cycles) {
    uint64_t prevRaster = m_rasterCounter / 65;
    m_rasterCounter += cycles;
    uint64_t currRaster = m_rasterCounter / 65;

    // VIC-20 NTSC has 262 lines
    if (currRaster >= 262) {
        m_rasterCounter = 0;
        currRaster = 0;
    }

    // The 6560/6561 has no IRQ pin and no raster-compare interrupt (unlike the 6567 VIC-II).
    // The raster counter is read-only via registers $03/$04 and is polled by software.
    // Interrupt generation in the VIC-20 is handled entirely by the 6522 VIAs.
    (void)currRaster;
    (void)prevRaster;

    tickAudio(cycles);
}

// ---------------------------------------------------------------------------
// Sound synthesis
// ---------------------------------------------------------------------------

void VIC6560::pushSample(float s) {
    if (m_audioBufCount >= AUDIO_BUF) return; // buffer full: drop new sample
    m_audioBuf[m_audioBufWrite] = s;
    m_audioBufWrite = (m_audioBufWrite + 1) & (AUDIO_BUF - 1);
    ++m_audioBufCount;
}

int VIC6560::pullSamples(float* buffer, int maxSamples) {
    int n = (maxSamples < m_audioBufCount) ? maxSamples : m_audioBufCount;
    for (int i = 0; i < n; ++i) {
        buffer[i] = m_audioBuf[m_audioBufRead];
        m_audioBufRead = (m_audioBufRead + 1) & (AUDIO_BUF - 1);
    }
    m_audioBufCount -= n;
    return n;
}

void VIC6560::tickAudio(uint64_t cycles) {
    // --- Advance the three square-wave tone oscillators ($900A–$900C) ---
    for (int v = 0; v < 3; ++v) {
        uint8_t reg = m_regs[0xA + v];
        if (!(reg & 0x80)) {
            // Voice disabled: silence and reset accumulator so it starts
            // cleanly when re-enabled.
            m_voiceOut[v]   = 0;
            m_voiceAccum[v] = 0;
            continue;
        }
        uint32_t F          = reg & 0x7Fu;
        uint32_t halfPeriod = PRESCALER[v] * (128u - F);
        m_voiceAccum[v] += cycles;
        uint32_t toggles    = (uint32_t)(m_voiceAccum[v] / halfPeriod);
        m_voiceAccum[v]    %= halfPeriod;
        m_voiceOut[v]      ^= (uint8_t)(toggles & 1u);
    }

    // --- Advance the noise channel ($900D) via a Galois 16-bit LFSR ---
    {
        uint8_t reg = m_regs[0xD];
        if (!(reg & 0x80)) {
            m_voiceOut[3]   = 0;
            m_voiceAccum[3] = 0;
        } else {
            uint32_t F          = reg & 0x7Fu;
            uint32_t halfPeriod = PRESCALER[3] * (128u - F);
            m_voiceAccum[3] += cycles;
            uint32_t steps      = (uint32_t)(m_voiceAccum[3] / halfPeriod);
            m_voiceAccum[3]    %= halfPeriod;
            // Advance LFSR: Galois form, polynomial 0xB400
            // (maximal-length 16-bit sequence, taps at 16,15,13,4)
            for (uint32_t i = 0; i < steps; ++i) {
                uint16_t out = m_lfsr & 1u;
                m_lfsr >>= 1;
                if (out) m_lfsr ^= 0xB400u;
            }
            m_voiceOut[3] = m_lfsr & 1u;
        }
    }

    // --- Mix and emit samples ---
    // Volume: $900E bits 3-0 (0 = mute, 15 = full).
    // Each active channel contributes ±0.25 so four voices sum to ±1.0.
    uint8_t vol = m_regs[0xE] & 0x0Fu;
    float amplitude = vol / 15.0f;

    float mix = 0.0f;
    for (int v = 0; v < 4; ++v) {
        uint8_t reg = (v < 3) ? m_regs[0xA + v] : m_regs[0xD];
        if (reg & 0x80) {
            mix += m_voiceOut[v] ? 0.25f : -0.25f;
        }
    }
    float sample = mix * amplitude;

    // Determine how many output samples this tick spans and push each one.
    m_sampleFrac += (uint64_t)cycles * (uint32_t)m_sampleRate;
    uint32_t newSamples = (uint32_t)(m_sampleFrac / PHI2_NTSC);
    m_sampleFrac       %= PHI2_NTSC;
    for (uint32_t i = 0; i < newSamples; ++i) {
        pushSample(sample);
    }
}

IVideoOutput::VideoDimensions VIC6560::getDimensions() const {
    // Standard VIC-20 NTSC: 201x232 visible area approximately.
    // Total internal buffer: 256x280
    return {256, 280, 22 * 8, 23 * 8};
}

// VIC-I colour palette in AARRGGBB order.
static uint32_t s_palette[] = {
    0xFF000000, //  0: Black
    0xFFFFFFFF, //  1: White
    0xFF882000, //  2: Red
    0xFF88FFFF, //  3: Cyan
    0xFFAA22CC, //  4: Purple
    0xFF00CC55, //  5: Green
    0xFF0000AA, //  6: Blue
    0xFFEEEE77, //  7: Yellow
    0xFFDD8855, //  8: Orange
    0xFFFFAA77, //  9: Light Orange
    0xFFFFBBBB, // 10: Pink
    0xFF888888, // 11: Dark Gray
    0xFFAAAAAA, // 12: Medium Gray
    0xFF88FF88, // 13: Light Green
    0xFF88AAFF, // 14: Light Blue
    0xFFDDDDDD, // 15: Light Gray
};

uint32_t VIC6560::getVicColor(uint8_t index) {
    return s_palette[index & 0x0F];
}

void VIC6560::renderFrame(uint32_t* buffer) {
    if (!m_bus) return;

    // VIC-I address space is inverted relative to the CPU: VA13 is complemented.
    //   VIC $0000-$1FFF (VA13=0) → CPU $8000-$9FFF  (char ROM / I/O)
    //   VIC $2000-$3FFF (VA13=1) → CPU $0000-$1FFF  (RAM)
    auto vicToCpu = [](uint16_t v) -> uint16_t {
        return (v < 0x2000) ? (v + 0x8000) : (v - 0x2000);
    };

    uint16_t vicScreen = ((m_regs[0x05] >> 4) & 0x0F) * 0x400;
    if (m_regs[0x02] & 0x80) vicScreen |= 0x200;
    uint16_t screenBase = vicToCpu(vicScreen);

    uint16_t vicChar = (m_regs[0x05] & 0x0F) * 0x400;
    uint16_t charBase = vicToCpu(vicChar);
    
    int cols = m_regs[0x02] & 0x7F;
    int rows = (m_regs[0x03] & 0x7E) >> 1;
    bool char16 = m_regs[0x03] & 0x01;
    int charHeight = char16 ? 16 : 8;

    uint8_t bgColorIdx = (m_regs[0x0F] & 0xF0) >> 4;
    uint8_t borderColorIdx = m_regs[0x0F] & 0x07;
    uint8_t auxColorIdx = (m_regs[0x0E] & 0xF0) >> 4;
    
    uint32_t bgColor = getVicColor(bgColorIdx);
    uint32_t borderColor = getVicColor(borderColorIdx);
    uint32_t auxColor = getVicColor(auxColorIdx);

    // Logging for debug
    static int frameCounter = 0;
    if (frameCounter++ % 60 == 0) {
        char msg[256];
        std::snprintf(msg, sizeof(msg), 
            "VIC6560 render: SCRBASE=$%04X CBBASE=$%04X COLS=%d ROWS=%d BG=%d BORDER=%d BUS=%p",
            screenBase, charBase, cols, rows, (int)bgColorIdx, (int)borderColorIdx, (void*)m_bus);
        // We don't have a log() method here, use printf or add it
    }

    VideoDimensions dim = getDimensions();
    
    // Fill with border color
    for (int i = 0; i < dim.width * dim.height; ++i) buffer[i] = borderColor;

    // Render characters
    int startX = (dim.width - cols * 8) / 2;
    int startY = (dim.height - rows * charHeight) / 2;

    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            uint16_t cellOffset = r * cols + c;
            uint8_t charCode = m_bus->peek8(screenBase + cellOffset);
            
            // Color RAM is usually at $9400 or $9600.
            // In VIC-20 it's 4-bit wide.
            uint8_t colorAttr = 0;
            if (m_colorRam) {
                colorAttr = m_colorRam[cellOffset] & 0x0F;
            }

            bool multiColor = colorAttr & 0x08;
            uint32_t fgColor = getVicColor(colorAttr & 0x07);

            for (int y = 0; y < charHeight; ++y) {
                uint8_t bits = m_bus->peek8(charBase + charCode * charHeight + y);
                for (int x = 0; x < 8; ++x) {
                    uint32_t pixelColor = bgColor;
                    if (multiColor) {
                        // Multi-color mode: 2 bits per pixel
                        int shift = (7 - x) & 0x06;
                        uint8_t pair = (bits >> shift) & 0x03;
                        switch (pair) {
                            case 0: pixelColor = bgColor; break;
                            case 1: pixelColor = borderColor; break;
                            case 2: pixelColor = fgColor; break;
                            case 3: pixelColor = auxColor; break;
                        }
                        // Draw 2 pixels wide
                        int px = startX + c * 8 + x;
                        int py = startY + r * charHeight + y;
                        if (px >= 0 && px < dim.width && py >= 0 && py < dim.height) {
                            buffer[py * dim.width + px] = pixelColor;
                        }
                    } else {
                        if (bits & (0x80 >> x)) pixelColor = fgColor;
                        else pixelColor = bgColor;
                        
                        int px = startX + c * 8 + x;
                        int py = startY + r * charHeight + y;
                        if (px >= 0 && px < dim.width && py >= 0 && py < dim.height) {
                            buffer[py * dim.width + px] = pixelColor;
                        }
                    }
                }
            }
        }
    }
}
