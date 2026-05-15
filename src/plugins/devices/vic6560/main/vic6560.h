#pragma once

#include "libdevices/main/io_handler.h"
#include "libdevices/main/ivideo_output.h"
#include "libdevices/main/iaudio_output.h"
#include "libdevices/main/isignal_line.h"
#include <cstdint>
#include <string>

/**
 * MOS 6560/6561 Video Interface Chip (VIC-I).
 * Used in the VIC-20.
 *
 * Implements IAudioOutput: tick() synthesises square-wave and noise samples
 * into an internal ring buffer; the host audio layer calls pullSamples().
 */
class VIC6560 : public IOHandler, public IVideoOutput, public IAudioOutput {
public:
    VIC6560() : m_name("VIC-I"), m_baseAddr(0x9000) { VIC6560::reset(); }
    VIC6560(const std::string& name, uint32_t baseAddr);
    virtual ~VIC6560() = default;

    // Configuration
    void setName(const std::string& name) override { m_name = name; }
    void setBaseAddr(uint32_t addr) override { m_baseAddr = addr; }
    void setDmaBus(IBus* bus) override { m_bus = bus; }
    void setIrqLine(ISignalLine* line) override { m_irqLine = line; }
    void setColorRam(const uint8_t* colorRam) override { m_colorRam = colorRam; }
    void setAudioSampleRate(int hz) { m_sampleRate = hz; }

    // IOHandler interface
    const char* name() const override { return m_name.c_str(); }
    uint32_t baseAddr() const override { return m_baseAddr; }
    uint32_t addrMask() const override { return 0x000F; } // 16 registers

    bool ioRead(IBus* bus, uint32_t addr, uint8_t* val) override;
    bool ioWrite(IBus* bus, uint32_t addr, uint8_t val) override;
    void reset() override;
    void tick(uint64_t cycles) override;

    // IVideoOutput interface
    VideoDimensions getDimensions() const override;
    void renderFrame(uint32_t* buffer) override;

    // IAudioOutput interface
    int nativeSampleRate() const override { return m_sampleRate; }
    int pullSamples(float* buffer, int maxSamples) override;

private:
    uint32_t getVicColor(uint8_t index);
    void     tickAudio(uint64_t cycles);
    void     pushSample(float s);

    std::string m_name;
    uint32_t    m_baseAddr;
    uint8_t     m_regs[16];
    uint64_t    m_rasterCounter = 0;

    ISignalLine*    m_irqLine  = nullptr;
    const uint8_t*  m_colorRam = nullptr;
    IBus*           m_bus      = nullptr;

    // ---- Sound synthesis ------------------------------------------------
    // NTSC φ2 clock. PAL would be 1108405; NTSC is the only variant here.
    static constexpr uint32_t PHI2_NTSC = 1022727;

    // Half-period prescalers per voice (voice 0=bass .. 2=treble, 3=noise).
    // half_period_cycles = PRESCALER[v] × (128 − F)  where F = reg & 0x7F
    static constexpr uint32_t PRESCALER[4] = {128, 64, 32, 16};

    static constexpr int AUDIO_BUF = 8192; // must be power-of-2

    int      m_sampleRate  = 44100;
    uint64_t m_voiceAccum[4]  = {};     // cycle accumulator per voice
    uint8_t  m_voiceOut[4]    = {};     // current square-wave level (0/1)
    uint16_t m_lfsr           = 0xACE1; // noise LFSR state (never zero)
    uint64_t m_sampleFrac     = 0;      // fractional sample accumulator

    float    m_audioBuf[AUDIO_BUF] = {};
    int      m_audioBufRead  = 0;
    int      m_audioBufWrite = 0;
    int      m_audioBufCount = 0;
};
