#pragma once
#include "libdevices/main/io_handler.h"
#include "libdevices/main/io_registry.h"
#include "libdevices/main/iaudio_output.h"
#include "libmem/main/ibus.h"
#include <cstdint>
#include <string>
#include <vector>
#include <cstring>

/**
 * Audio DMA Controller for MEGA65
 *
 * Provides 4 channels of DMA-driven audio playback.
 * Register map (base $D710):
 *   $D711:      Audio DMA control register
 *   $D720-$D72F: Channel 0 (4 channels × 16 bytes each)
 *   $D730-$D73F: Channel 1
 *   $D740-$D74F: Channel 2
 *   $D750-$D75F: Channel 3
 *
 * Control register $D711:
 *   Bit 7: AUDEN — Enable audio DMA (and switch audio cross-bar to DMA source)
 *   Bit 4: NOMIX — Bypass audio mixer, play DMA audio at full volume
 *
 * Per-channel registers (relative to channel base at $D720, $D730, $D740, $D750):
 *   +$00: CH0EN (bit 7=enable), CH0LOOP (bit 6), CH0SINE (bit 5), reserved (4:0)
 *   +$01-$03: Loop start address (24-bit, little-endian)
 *   +$04-$06: Frequency step (24-bit, little-endian) — added to freq counter each cycle
 *   +$07-$08: Sample end address (16-bit, only checks lower 16 bits)
 *   +$09: Volume (0x00-0xFF)
 *   +$0A-$0C: Current sample address (24-bit, little-endian)
 *   +$0D: CH0STP (bit 0=stopped flag, read-only)
 *   +$0E-$0F: Reserved
 *
 * Audio generation:
 *   - Each channel has a 24-bit frequency counter
 *   - Counter increments by frequency step each CPU cycle
 *   - When counter overflows (reaches 0x1000000), fetch next sample
 *   - Samples are 8-bit or 16-bit (controlled by sample format flag)
 *   - Output is normalized to [-1.0, 1.0] float32 for IAudioOutput
 *
 * References:
 *   - MEGA65 Book Appendix L (Audio DMA)
 *   - gs4510.vhdl (MEGA65 processor VHDL)
 */
class AudioDmaDevice : public IOHandler, public IAudioOutput {
public:
    explicit AudioDmaDevice(uint32_t baseAddr = 0xD720);
    virtual ~AudioDmaDevice() = default;

    const char* name() const override { return m_name.c_str(); }
    uint32_t    baseAddr() const override { return m_base; }
    uint32_t    addrMask() const override { return 0x7F; }  // 80 bytes: $D710-$D75F

    void setName(const std::string& n) override { m_name = n; }
    void setBaseAddr(uint32_t a) override { m_base = a; }
    void setDmaBus(IBus* bus) override { m_bus = bus; }
    void setIoRegistry(IORegistry* io) { m_ioRegistry = io; }
    void setSampleRate(int hz) { m_sampleRate = hz; }
    void setClockHz(uint32_t hz) override { m_clockHz = hz; }

    void reset() override;
    bool ioRead (IBus* bus, uint32_t addr, uint8_t* val) override;
    bool ioWrite(IBus* bus, uint32_t addr, uint8_t val) override;
    void tick(uint64_t cycles) override;
    void getDeviceInfo(DeviceInfo& out) const override;

    // IAudioOutput interface
    int nativeSampleRate() const override { return m_sampleRate; }
    int pullSamples(float* buffer, int maxSamples) override;

    std::vector<std::string> deviceAliases() const override { return {"AUDIODMA", "MEGA65AUDIO"}; }

private:
    struct AudioChannel {
        bool     enabled;           // Bit 7 of control reg
        bool     loopMode;          // Bit 6 of control reg
        bool     sineMode;          // Bit 5 of control reg
        bool     stopped;           // Bit 0 of status reg (read-only)

        uint32_t loopStartAddr;     // 24-bit loop start address ($01-$03)
        uint32_t freqStep;          // 24-bit frequency step ($04-$06)
        uint16_t sampleEndAddr;     // 16-bit sample end address ($07-$08)
        uint8_t  volume;            // Volume 0-255 ($09)
        uint32_t currentAddr;       // 24-bit current sample address ($0A-$0C)

        uint32_t freqCounter;       // 24-bit frequency counter (internal)
        uint32_t sampleBufferIndex; // Internal: position in sample buffer
    };

    uint32_t m_base;
    std::string m_name{"Audio DMA"};
    IBus* m_bus;
    IORegistry* m_ioRegistry;

    AudioChannel m_channels[4];     // 4 audio channels
    uint8_t m_audioControlReg;      // Shadow of $D711 (AUDEN, NOMIX)

    // Audio output
    int m_sampleRate;               // Native sample rate (typically 44100 Hz)
    uint32_t m_clockHz;             // CPU clock frequency (typically 40 MHz)
    std::vector<float> m_sampleBuffer;  // Ring buffer for audio samples
    uint32_t m_sampleWriteIndex;    // Next write position in sample buffer
    uint32_t m_sampleReadIndex;     // Next read position in sample buffer

    void tickChannel(int ch, uint64_t cycles);
    void fetchSample(int ch, uint8_t& sample8bit, int16_t& sample16bit, bool& is16bit);
    void pushSample(float left, float right);
    float normalizeU8(uint8_t val) const;  // Convert unsigned 8-bit to [-1.0, 1.0]
    float normalizeS16(int16_t val) const; // Convert signed 16-bit to [-1.0, 1.0]
};
