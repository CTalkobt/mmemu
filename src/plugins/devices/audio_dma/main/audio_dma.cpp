#include "audio_dma.h"
#include "libdevices/main/io_registry.h"
#include "libmem/main/ibus.h"
#include <cmath>
#include <algorithm>

AudioDmaDevice::AudioDmaDevice(uint32_t baseAddr)
    : m_base(baseAddr), m_bus(nullptr), m_ioRegistry(nullptr),
      m_audioControlReg(0),
      m_sampleRate(44100), m_clockHz(40000000),
      m_sampleWriteIndex(0), m_sampleReadIndex(0) {
    m_sampleBuffer.resize(m_sampleRate * 2, 0.0f);  // 2 seconds of buffer at 44.1 kHz
    reset();
}

void AudioDmaDevice::reset() {
    for (int i = 0; i < 4; ++i) {
        m_channels[i].enabled = false;
        m_channels[i].loopMode = false;
        m_channels[i].sineMode = false;
        m_channels[i].stopped = false;
        m_channels[i].loopStartAddr = 0;
        m_channels[i].freqStep = 0;
        m_channels[i].sampleEndAddr = 0;
        m_channels[i].volume = 0;
        m_channels[i].currentAddr = 0;
        m_channels[i].freqCounter = 0;
        m_channels[i].sampleBufferIndex = 0;
    }
    m_audioControlReg = 0;
    m_sampleWriteIndex = 0;
    m_sampleReadIndex = 0;
}

bool AudioDmaDevice::ioRead(IBus* /*bus*/, uint32_t addr, uint8_t* val) {
    uint32_t offset = addr - m_base;

    // Handle control register at $D711 (offset $01 from base $D710)
    if (offset == 0x01) {
        *val = m_audioControlReg;
        return true;
    }

    // Audio channels start at offset $10 (address $D720)
    if (offset < 0x10 || offset >= 0x50) return false;  // Only $D720-$D75F

    int ch = (offset - 0x10) / 16;
    int regIdx = (offset - 0x10) % 16;

    AudioChannel& channel = m_channels[ch];

    switch (regIdx) {
        case 0x00:  // Control: EN (7), LOOP (6), SINE (5)
            *val = (channel.enabled ? 0x80 : 0x00) |
                   (channel.loopMode ? 0x40 : 0x00) |
                   (channel.sineMode ? 0x20 : 0x00);
            return true;
        case 0x01:  // Loop start address (LSB)
            *val = channel.loopStartAddr & 0xFF;
            return true;
        case 0x02:  // Loop start address (MID)
            *val = (channel.loopStartAddr >> 8) & 0xFF;
            return true;
        case 0x03:  // Loop start address (MSB, 24-bit)
            *val = (channel.loopStartAddr >> 16) & 0xFF;
            return true;
        case 0x04:  // Frequency step (LSB)
            *val = channel.freqStep & 0xFF;
            return true;
        case 0x05:  // Frequency step (MID)
            *val = (channel.freqStep >> 8) & 0xFF;
            return true;
        case 0x06:  // Frequency step (MSB, 24-bit)
            *val = (channel.freqStep >> 16) & 0xFF;
            return true;
        case 0x07:  // Sample end address (LSB, 16-bit)
            *val = channel.sampleEndAddr & 0xFF;
            return true;
        case 0x08:  // Sample end address (MSB, 16-bit)
            *val = (channel.sampleEndAddr >> 8) & 0xFF;
            return true;
        case 0x09:  // Volume
            *val = channel.volume;
            return true;
        case 0x0A:  // Current address (LSB)
            *val = channel.currentAddr & 0xFF;
            return true;
        case 0x0B:  // Current address (MID)
            *val = (channel.currentAddr >> 8) & 0xFF;
            return true;
        case 0x0C:  // Current address (MSB, 24-bit)
            *val = (channel.currentAddr >> 16) & 0xFF;
            return true;
        case 0x0D:  // Status: CH0STP (bit 0, read-only)
            *val = channel.stopped ? 0x01 : 0x00;
            return true;
        case 0x0E:
        case 0x0F:
            *val = 0x00;  // Reserved
            return true;
        default:
            return false;
    }
}

bool AudioDmaDevice::ioWrite(IBus* bus, uint32_t addr, uint8_t val) {
    uint32_t offset = addr - m_base;

    // Handle control register at $D711 (offset $01 from base $D710)
    if (offset == 0x01) {
        m_audioControlReg = val;
        return true;
    }

    // Audio channels start at offset $10 (address $D720)
    if (offset < 0x10 || offset >= 0x50) return false;  // Only $D720-$D75F

    if (!m_bus) m_bus = bus;

    int ch = (offset - 0x10) / 16;
    int regIdx = (offset - 0x10) % 16;

    AudioChannel& channel = m_channels[ch];

    switch (regIdx) {
        case 0x00:  // Control: EN (7), LOOP (6), SINE (5)
            channel.enabled = (val & 0x80) != 0;
            channel.loopMode = (val & 0x40) != 0;
            channel.sineMode = (val & 0x20) != 0;
            if (channel.enabled && channel.stopped) {
                channel.stopped = false;
                channel.freqCounter = 0;
            }
            return true;
        case 0x01:  // Loop start address (LSB)
            channel.loopStartAddr = (channel.loopStartAddr & 0xFFFF00) | val;
            return true;
        case 0x02:  // Loop start address (MID)
            channel.loopStartAddr = (channel.loopStartAddr & 0xFF00FF) | (val << 8);
            return true;
        case 0x03:  // Loop start address (MSB, 24-bit)
            channel.loopStartAddr = (channel.loopStartAddr & 0x00FFFF) | ((val & 0xFF) << 16);
            return true;
        case 0x04:  // Frequency step (LSB)
            channel.freqStep = (channel.freqStep & 0xFFFF00) | val;
            return true;
        case 0x05:  // Frequency step (MID)
            channel.freqStep = (channel.freqStep & 0xFF00FF) | (val << 8);
            return true;
        case 0x06:  // Frequency step (MSB, 24-bit)
            channel.freqStep = (channel.freqStep & 0x00FFFF) | ((val & 0xFF) << 16);
            return true;
        case 0x07:  // Sample end address (LSB, 16-bit)
            channel.sampleEndAddr = (channel.sampleEndAddr & 0xFF00) | val;
            return true;
        case 0x08:  // Sample end address (MSB, 16-bit)
            channel.sampleEndAddr = (channel.sampleEndAddr & 0x00FF) | (val << 8);
            return true;
        case 0x09:  // Volume
            channel.volume = val;
            return true;
        case 0x0A:  // Current address (LSB)
            channel.currentAddr = (channel.currentAddr & 0xFFFF00) | val;
            return true;
        case 0x0B:  // Current address (MID)
            channel.currentAddr = (channel.currentAddr & 0xFF00FF) | (val << 8);
            return true;
        case 0x0C:  // Current address (MSB, 24-bit)
            channel.currentAddr = (channel.currentAddr & 0x00FFFF) | ((val & 0xFF) << 16);
            return true;
        case 0x0D:  // Status register (read-only, ignore writes)
        case 0x0E:  // Reserved
        case 0x0F:  // Reserved
            return true;
        default:
            return false;
    }
}

void AudioDmaDevice::tick(uint64_t cycles) {
    if (!m_bus) return;

    // Check if audio DMA is enabled via $D711 bit 7
    if (!(m_audioControlReg & 0x80)) {
        return;  // Audio DMA disabled
    }

    // Process each channel for the given number of cycles
    for (int ch = 0; ch < 4; ++ch) {
        tickChannel(ch, cycles);
    }
}

void AudioDmaDevice::tickChannel(int ch, uint64_t cycles) {
    AudioChannel& channel = m_channels[ch];

    if (!channel.enabled || channel.stopped) {
        return;
    }

    // Sine mode: generate pure sine wave
    if (channel.sineMode) {
        // Pure sine wave at specified frequency — no DMA fetches needed
        // Frequency counter still increments normally
        for (uint64_t i = 0; i < cycles; ++i) {
            channel.freqCounter += channel.freqStep;
            if (channel.freqCounter >= 0x1000000) {
                channel.freqCounter -= 0x1000000;
                // Generate sine sample (simplified: just use frequency for phase)
                uint32_t phase = (channel.currentAddr & 0x1F);  // 32-byte sine table
                float sineVal = std::sin(2.0f * 3.14159f * phase / 32.0f);
                float sample = sineVal * (channel.volume / 255.0f);
                pushSample(sample, sample);
            }
        }
        return;
    }

    // Standard sample playback
    for (uint64_t i = 0; i < cycles; ++i) {
        channel.freqCounter += channel.freqStep;
        if (channel.freqCounter >= 0x1000000) {
            channel.freqCounter -= 0x1000000;

            // Fetch next sample
            uint8_t sample8bit = 0;
            int16_t sample16bit = 0;
            bool is16bit = false;

            fetchSample(ch, sample8bit, sample16bit, is16bit);

            // Normalize and push to output buffer
            float normalized = is16bit ? normalizeS16(sample16bit) : normalizeU8(sample8bit);
            float sample = normalized * (channel.volume / 255.0f);
            pushSample(sample, sample);

            // Advance current address
            channel.currentAddr = (channel.currentAddr + 1) & 0xFFFFFF;

            // Check if we've reached end address (compare lower 16 bits only)
            uint16_t currentAddrLow = channel.currentAddr & 0xFFFF;
            if (currentAddrLow == channel.sampleEndAddr) {
                if (channel.loopMode) {
                    // Loop back to loop start address
                    channel.currentAddr = channel.loopStartAddr;
                } else {
                    // Stop playback
                    channel.stopped = true;
                    channel.enabled = false;
                    break;
                }
            }
        }
    }
}

void AudioDmaDevice::fetchSample(int ch, uint8_t& sample8bit, int16_t& sample16bit, bool& is16bit) {
    if (!m_bus) {
        sample8bit = 0;
        sample16bit = 0;
        is16bit = false;
        return;
    }

    AudioChannel& channel = m_channels[ch];

    // For now, assume 8-bit unsigned samples (most common)
    // TODO: Support 16-bit samples when format flag is implemented
    sample8bit = m_bus->read8(channel.currentAddr);
    sample16bit = 0;
    is16bit = false;
}

void AudioDmaDevice::pushSample(float left, float right) {
    // Stereo interleaved: left, right, left, right, ...
    uint32_t next_idx = m_sampleWriteIndex + 2;
    if (next_idx >= m_sampleBuffer.size()) {
        next_idx -= m_sampleBuffer.size();
    }

    // Prevent overwriting samples that haven't been read yet
    if (next_idx == m_sampleReadIndex) {
        return;  // Buffer full, skip this sample
    }

    m_sampleBuffer[m_sampleWriteIndex] = left;
    m_sampleBuffer[(m_sampleWriteIndex + 1) % m_sampleBuffer.size()] = right;
    m_sampleWriteIndex = next_idx;
}

float AudioDmaDevice::normalizeU8(uint8_t val) const {
    // Convert unsigned 8-bit (0-255) to signed float (-1.0 to 1.0)
    return (val / 127.5f) - 1.0f;
}

float AudioDmaDevice::normalizeS16(int16_t val) const {
    // Convert signed 16-bit (-32768 to 32767) to float (-1.0 to 1.0)
    return val / 32768.0f;
}

int AudioDmaDevice::pullSamples(float* buffer, int maxSamples) {
    int samples_available = 0;

    if (m_sampleReadIndex < m_sampleWriteIndex) {
        samples_available = m_sampleWriteIndex - m_sampleReadIndex;
    } else if (m_sampleReadIndex > m_sampleWriteIndex) {
        samples_available = m_sampleBuffer.size() - m_sampleReadIndex + m_sampleWriteIndex;
    }

    int samples_to_pull = std::min(samples_available / 2, maxSamples * 2) / 2;

    for (int i = 0; i < samples_to_pull * 2; ++i) {
        buffer[i] = m_sampleBuffer[m_sampleReadIndex];
        m_sampleReadIndex = (m_sampleReadIndex + 1) % m_sampleBuffer.size();
    }

    return samples_to_pull;
}

void AudioDmaDevice::getDeviceInfo(DeviceInfo& out) const {
    out.name = m_name;
    out.baseAddr = m_base;
    out.addrMask = addrMask();

    for (int ch = 0; ch < 4; ++ch) {
        char buf[64];
        const AudioChannel& channel = m_channels[ch];
        std::snprintf(buf, sizeof(buf), "Ch%d: EN=%d LOOP=%d SINE=%d Vol=%3d Addr=$%06X",
                      ch, channel.enabled ? 1 : 0, channel.loopMode ? 1 : 0,
                      channel.sineMode ? 1 : 0, channel.volume, channel.currentAddr);
        out.state.push_back({std::string(buf), ""});
    }

    out.dependencies.push_back({"Audio Bus", m_bus ? "connected" : "none"});
}
