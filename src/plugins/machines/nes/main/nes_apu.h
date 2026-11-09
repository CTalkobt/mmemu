#pragma once

// NES / Famicom APU, the sound half of the Ricoh 2A03 (NTSC).
//
// After Mesen2 (Core/NES/APU/: NesApu.cpp, ApuFrameCounter.h, SquareChannel.h,
// TriangleChannel.h, NoiseChannel.h, DeltaModulationChannel.cpp,
// ApuEnvelope.h, ApuLengthCounter.h; NesSoundMixer.cpp), GPL-3.0, Sour and
// the Mesen contributors, and the nesdev wiki ("APU"). Left out: the
// one-cycle delays of the length counter halt/reload and of $4015 enabling
// the DMC, the DMC DMA's exact stall cycles (a flat 4), the frame counter's
// odd/even write delay (always 3 cycles), expansion audio and PAL rates.

#include "libdevices/main/io_handler.h"
#include "libdevices/main/iaudio_output.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace nes {

class Apu : public IOHandler, public IAudioOutput {
public:
    static constexpr uint32_t CPU_HZ = 1789773;    // NTSC: 21.477272 MHz / 12
    static constexpr int DEFAULT_SAMPLE_RATE = 44100;

    Apu();
    ~Apu() override = default;

    /** The DMC's sample fetches: read a byte of CPU memory (the machine stalls the CPU). */
    void setDmaRead(std::function<uint8_t(uint16_t)> fn) { m_dmaRead = std::move(fn); }

    // ------------------------------------------------------------------
    // Machine interface
    // ------------------------------------------------------------------
    /** Run `cycles` CPU cycles. */
    void clock(uint32_t cycles);
    /** A write to $4000-$4013, $4015 or $4017. */
    void write(uint16_t addr, uint8_t value);
    /** $4015: length counters, DMC active, the frame and DMC IRQ flags (clears the frame IRQ). */
    uint8_t readStatus(uint8_t openBus);
    uint8_t peekStatus(uint8_t openBus) const;
    /** /IRQ: the frame counter or the DMC. */
    bool irq() const { return m_frameIrq || m_dmcIrq; }
    /** CPU cycles the DMC's fetches stole since the last call (the machine stalls the CPU). */
    int takeStall() { int s = m_stall; m_stall = 0; return s; }

    /** Each channel's current output level (pulse 0-15, triangle 0-15, noise 0-15, DMC 0-127), for tests. */
    int output(int channel) const;
    /** Length counter of channel 0-3 (pulse 1, pulse 2, triangle, noise). */
    int lengthCounter(int channel) const;

    // ------------------------------------------------------------------
    // IOHandler
    // ------------------------------------------------------------------
    const char* name() const override { return m_name.c_str(); }
    void setName(const std::string& n) override { m_name = n; }
    uint32_t baseAddr() const override { return 0x4000; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    /** Power-on state. */
    void reset() override;
    /**
     * The RESET button (Mesen NesApu::Reset(true)): $4015 is cleared
     * (channels off, length counters zeroed except the triangle's), $4017's
     * mode is kept and rewritten, the DMC keeps its sample address/length.
     */
    void softReset();
    void tick(uint64_t) override {}

    // ------------------------------------------------------------------
    // IAudioOutput
    // ------------------------------------------------------------------
    int nativeSampleRate() const override { return m_sampleRate; }
    int pullSamples(float* buffer, int maxSamples) override;
    void setSampleRate(int hz) { m_sampleRate = hz > 0 ? hz : DEFAULT_SAMPLE_RATE; }

private:
    struct Envelope {
        bool constant = false, loop = false, start = false;
        uint8_t volume = 0, divider = 0, decay = 0;
        void write(uint8_t v) { loop = v & 0x20; constant = v & 0x10; volume = v & 0x0F; }
        void tick();
        uint8_t level() const { return constant ? volume : decay; }
    };
    struct Length {
        bool enabled = false, halt = false;
        uint8_t counter = 0;
        void load(uint8_t index);
        void tick() { if (counter && !halt) --counter; }
    };
    struct Pulse {
        bool second = false;
        Envelope env;
        Length len;
        uint8_t duty = 0, step = 0;
        uint16_t period = 0, timer = 0;
        bool sweepOn = false, sweepNegate = false, sweepReload = false;
        uint8_t sweepPeriod = 1, sweepShift = 0, sweepDivider = 0;
        uint16_t target() const;
        bool muted() const { return period < 8 || (!sweepNegate && target() > 0x7FF); }
        void tickSweep();
        void clock();               // every APU cycle (2 CPU cycles)
        uint8_t out() const;
    };
    struct Triangle {
        Length len;
        bool control = false, linearReload = false;
        uint8_t linearPeriod = 0, linear = 0, step = 0;
        uint16_t period = 0, timer = 0;
        void tickLinear();
        void clock();               // every CPU cycle
        uint8_t out() const;
    };
    struct Noise {
        Envelope env;
        Length len;
        bool mode = false;
        uint16_t period = 4, timer = 0, shift = 1;
        void clock();               // every APU cycle
        uint8_t out() const { return (!(shift & 1) && len.counter) ? env.level() : 0; }
    };
    struct Dmc {
        bool irqOn = false, loop = false;
        uint16_t rate = 428, timer = 0;
        uint8_t level = 0;
        uint16_t sampleAddr = 0xC000, sampleLength = 1;
        uint16_t addr = 0, remaining = 0;
        uint8_t buffer = 0, shift = 0, bits = 8;
        bool empty = true, silence = true;
    };

    void quarterFrame();
    void halfFrame();
    void clockFrameCounter();
    void clockDmc();
    void dmcFetch();
    void dmcStart() { m_dmc.addr = m_dmc.sampleAddr; m_dmc.remaining = m_dmc.sampleLength; }
    float mix() const;

    std::string m_name = "apu";
    std::function<uint8_t(uint16_t)> m_dmaRead;

    Pulse m_pulse[2];
    Triangle m_tri;
    Noise m_noise;
    Dmc m_dmc;

    // Frame counter (Mesen ApuFrameCounter)
    int32_t m_frameCycle = 0;
    int m_frameStep = 0;
    bool m_fiveStep = false, m_irqInhibit = false;
    int m_frameWriteDelay = 0;
    uint8_t m_frameWriteValue = 0;
    int m_frameBlock = 0;
    bool m_frameIrq = false, m_dmcIrq = false;

    uint64_t m_cycle = 0;
    int m_stall = 0;

    // Output: averaged over each output sample, a DC-blocking high-pass
    int m_sampleRate = DEFAULT_SAMPLE_RATE;
    double m_acc = 0;
    uint32_t m_accCount = 0;
    uint64_t m_phase = 0;
    float m_hpIn = 0, m_hpOut = 0;
    static constexpr int AUDIO_BUF = 16384;
    std::vector<float> m_audioBuf;
    int m_audioRead = 0, m_audioWrite = 0, m_audioCount = 0;
};

}  // namespace nes
