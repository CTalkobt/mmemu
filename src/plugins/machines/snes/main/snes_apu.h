#pragma once

// The SNES sound system (#15): the SPC700 with its 64 KB of RAM, the IPL
// boot ROM, the three timers, the four ports to the main CPU and the S-DSP.
//
// After Mesen2 (Core/SNES/Spc.cpp, SpcTimer.h), GPL-3.0, Sour and the Mesen
// contributors. The SPC700 runs at 1.024 MHz (24.576 MHz / 24), one DSP step
// per cycle (32 kHz stereo). It is clocked lazily: the main CPU's port
// accesses and the end of each instruction bring it up to time. Left out:
// the TEST register's wait states ($F0 speed bits) and the half-cycle port
// latch delay.

#include "libdevices/main/io_handler.h"
#include "libdevices/main/iaudio_output.h"
#include "libmem/main/ibus.h"
#include "snes_dsp.h"
#include "spc700.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace snes {

class Apu : public IOHandler, public IAudioOutput {
public:
    static constexpr uint32_t CLOCK_HZ = 1024000;
    static constexpr int DEFAULT_SAMPLE_RATE = 44100;

    Apu();
    ~Apu() override;

    Spc700* cpu() { return m_cpu; }
    IBus* bus() { return m_bus; }
    /** The machine descriptor takes over deleting the SPC700 and its bus (its CPU and bus slots). */
    void releaseCoreAndBus() { m_owns = false; }
    Dsp& dsp() { return m_dsp; }
    uint8_t* ram() { return m_ram; }

    /** Run the SPC700 up to APU cycle `cycle`. */
    void runTo(uint64_t cycle);
    uint64_t cycle() const { return m_cycle; }

    /** The main CPU's side of the ports ($2140-$2143, `port` 0-3). */
    uint8_t cpuRead(int port) const { return m_toCpu[port & 3]; }
    void cpuWrite(int port, uint8_t v) { m_fromCpu[port & 3] = v; }
    /** What the SPC700 has written for the main CPU / the main CPU for it. */
    uint8_t portToCpu(int port) const { return m_toCpu[port & 3]; }
    uint8_t portFromCpu(int port) const { return m_fromCpu[port & 3]; }

    // ------------------------------------------------------------------
    // IOHandler
    // ------------------------------------------------------------------
    const char* name() const override { return m_name.c_str(); }
    void setName(const std::string& n) override { m_name = n; }
    uint32_t baseAddr() const override { return 0x2140; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    /** Power-on: RAM cleared, the IPL ROM in, the SPC700 at its reset vector. */
    void reset() override;
    void tick(uint64_t) override {}

    // ------------------------------------------------------------------
    // IAudioOutput
    // ------------------------------------------------------------------
    int nativeSampleRate() const override { return m_sampleRate; }
    int pullSamples(float* buffer, int maxSamples) override;
    void setSampleRate(int hz) { m_sampleRate = hz > 0 ? hz : DEFAULT_SAMPLE_RATE; }

    // The SPC700's view of $0000-$FFFF (for its bus)
    uint8_t smpRead(uint16_t a);
    uint8_t smpPeek(uint16_t a) const;
    void smpWrite(uint16_t a, uint8_t v);

private:
    /** Timer 0/1 at 8 kHz (every 128 cycles), timer 2 at 64 kHz (16). */
    struct Timer {
        int rate = 128;
        bool enabled = false;
        uint8_t stage0 = 0, stage2 = 0, target = 0, output = 0;
        void enable(bool on) { if (!enabled && on) { stage2 = 0; output = 0; } enabled = on; }
        void clock() {
            if (++stage0 < rate) return;
            stage0 = 0;
            if (!enabled) return;
            if (++stage2 == target) { stage2 = 0; output = (uint8_t)((output + 1) & 0x0F); }
        }
    };

    void cycles(int n);
    void drainDsp();

    std::string m_name = "apu";
    uint8_t m_ram[0x10000] = {};
    Spc700* m_cpu;
    IBus* m_bus;
    bool m_owns = true;
    Dsp m_dsp;
    Timer m_timers[3];
    uint8_t m_fromCpu[4] = {}, m_toCpu[4] = {};
    uint8_t m_dspAddr = 0;
    uint8_t m_ramReg[2] = {};
    bool m_iplOn = true;
    uint64_t m_cycle = 0;

    // Output: the DSP's 32 kHz stereo, mixed to mono and resampled
    int m_sampleRate = DEFAULT_SAMPLE_RATE;
    uint64_t m_phase = 0;
    double m_acc = 0;
    int m_accCount = 0;
    static constexpr int AUDIO_BUF = 16384;
    std::vector<float> m_audioBuf;
    int m_audioRead = 0, m_audioWrite = 0, m_audioCount = 0;
};

}  // namespace snes
