#pragma once

// The SNES sound system (#15): the SPC700 with its 64 KB of RAM, the IPL
// boot ROM, the three timers, the four ports to the main CPU and the S-DSP.
//
// After Mesen2 (Core/SNES/Spc.cpp, SpcTimer.h), GPL-3.0, Sour and the Mesen
// contributors. The SPC700 runs at 1.024 MHz (24.576 MHz / 24), one DSP step
// per cycle (32 kHz stereo). It is clocked lazily: the main CPU's port
// accesses and the end of each instruction bring it up to time. Each of its
// bus accesses takes its time as it happens, idle cycles at the end of the
// instruction. The TEST register ($F0) works as in Mesen: wait states for
// internal ($F0-$FF, the IPL ROM, idle cycles) and external accesses, the
// timers' rate with them, the timers' global enable and disable, and RAM
// writes on or off. Left out: the half-cycle port latch delay.

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

class AudioSource;

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
    /** `visibleFrom`: the APU cycle the SPC700 first sees it in (Mesen: a write in the second half of a
     *  cycle is seen a cycle later); 0 for at once. */
    void cpuWrite(int port, uint8_t v, uint64_t visibleFrom = 0);
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
    /** Mixed into the DSP's output (the MSU-1); null for nothing. Not owned. */
    void setExtraAudio(AudioSource* src) { m_extra = src; }

    // The SPC700's view of $0000-$FFFF (for its bus)
    uint8_t smpRead(uint16_t a);
    uint8_t smpPeek(uint16_t a) const;
    void smpWrite(uint16_t a, uint8_t v);

    /** The TEST register's state (for tests). */
    bool ramWritesOn() const { return m_writeEnabled; }

private:
    /**
     * Mesen's SpcTimer: stage 0 counts by 2 a cycle at normal speed (more or less with the TEST wait states)
     * and toggles stage 1 every `rate`; stage 2 counts stage 1's falls (8 kHz for timers 0/1, 64 kHz for 2).
     * The global disable holds stage 1 low, so turning the timers off can itself count one.
     */
    struct Timer {
        uint8_t rate = 128;
        bool enabled = false, global = true;
        uint8_t stage0 = 0, stage1 = 0, prev1 = 0, stage2 = 0, target = 0, output = 0x0F;
        void clockTimer() {
            const uint8_t cur = global ? stage1 : 0;
            const uint8_t prev = prev1;
            prev1 = cur;
            if (!enabled || !prev || cur) return;
            if (++stage2 == target) { stage2 = 0; ++output; }
        }
        void enable(bool on) { if (!enabled && on) { stage2 = 0; output = 0; } enabled = on; }
        void setGlobal(bool on) { global = on; clockTimer(); }
        void run(uint8_t step) {
            stage0 = (uint8_t)(stage0 + step);
            if (stage0 >= rate) { stage1 ^= 1; stage0 = (uint8_t)(stage0 - rate); clockTimer(); }
        }
    };

    /** One access's (or idle cycle's) time at speed `sel`: a DSP step, the wait, the timers. */
    void tick(uint8_t sel);
    uint8_t speedFor(uint16_t a) const {
        return ((a & 0xFFF0) == 0x00F0 || (a >= 0xFFC0 && m_iplOn)) ? m_internalSpeed : m_externalSpeed;
    }
    void drainDsp();

    std::string m_name = "apu";
    uint8_t m_ram[0x10000] = {};
    Spc700* m_cpu;
    IBus* m_bus;
    bool m_owns = true;
    Dsp m_dsp;
    Timer m_timers[3];
    uint8_t m_fromCpu[4] = {}, m_toCpu[4] = {};
    uint8_t m_fromCpuNew[4] = {};      // the CPU's last writes, which the SPC700 sees from m_portsAt
    uint64_t m_portsAt = 0;
    bool m_portsPending = false;
    uint8_t m_dspAddr = 0;
    uint8_t m_ramReg[2] = {};
    bool m_iplOn = true;
    uint64_t m_cycle = 0;
    // TEST ($F0)
    uint8_t m_internalSpeed = 0, m_externalSpeed = 0;
    bool m_timersEnabled = true, m_timersDisabled = false, m_writeEnabled = true;
    // The accesses in the instruction being run (time is only counted while the SPC700 runs)
    bool m_inStep = false;
    int m_accesses = 0;

    // Output: the DSP's 32 kHz stereo, mixed to mono and resampled
    int m_sampleRate = DEFAULT_SAMPLE_RATE;
    uint64_t m_phase = 0;
    double m_acc = 0;
    int m_accCount = 0;
    float m_lastOut = 0;
    static constexpr int AUDIO_BUF = 16384;
    std::vector<float> m_audioBuf;
    int m_audioRead = 0, m_audioWrite = 0, m_audioCount = 0;
    AudioSource* m_extra = nullptr;
};

}  // namespace snes
