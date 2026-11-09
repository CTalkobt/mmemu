#pragma once

// The SNES S-DSP (#15): eight voices of BRR samples with ADSR/GAIN envelopes
// and Gaussian interpolation, noise, pitch modulation, echo with an 8-tap
// FIR filter, run as the hardware's 32-step pipeline (one step per SPC700
// cycle, a stereo sample every 32: 32 kHz).
//
// After Mesen2 (Core/SNES/DSP/Dsp.cpp, DspVoice.cpp, DspInterpolation.h),
// GPL-3.0, Sour and the Mesen contributors, itself after blargg's SPC_DSP
// and the fullsnes / Anomie documentation. Left out: the cubic and sinc
// interpolation options and per-channel volume settings.

#include <cstdint>

namespace snes {

class Dsp {
public:
    /** `ram`: the APU's 64 KB, which the DSP reads samples from and writes echo to. */
    explicit Dsp(uint8_t* ram);

    void reset();
    /** One pipeline step (one SPC700 cycle). */
    void step();

    uint8_t read(uint8_t reg) const { return m_ext[reg & 0x7F]; }
    void write(uint8_t reg, uint8_t value);

    /** Stereo samples made since the last takeSamples() (interleaved L, R). */
    int sampleCount() const { return m_outCount; }
    const int16_t* samples() const { return m_out; }
    void clearSamples() { m_outCount = 0; }

    /** Voice `v`'s current envelope (0-$7FF), for tests and the debugger. */
    int envelope(int v) const { return m_voices[v].envVolume; }

private:
    enum class Env { Release, Attack, Decay, Sustain };
    struct Voice {
        int index = 0;
        uint8_t bit = 1;
        int32_t envVolume = 0, prevEnv = 0, interpPos = 0;
        Env envMode = Env::Release;
        uint16_t brrAddress = 0, brrOffset = 1;
        uint8_t keyOnDelay = 0, envOut = 0, bufferPos = 0;
        int16_t buffer[12] = {};
    };

    uint8_t reg(int r) const { return m_regs[r]; }
    uint8_t vreg(const Voice& v, int r) const { return m_regs[v.index * 0x10 + r]; }
    void writeGlobal(int r, uint8_t value) { m_ext[r] = value; m_regs[r] = value; }
    void writeVoice(const Voice& v, int r, uint8_t value) { m_ext[v.index * 0x10 + r] = value; m_regs[v.index * 0x10 + r] = value; }
    bool checkCounter(int rate) const;
    int32_t fir(int index, int ch) const;

    void decodeBrr(Voice& v);
    void envelope(Voice& v);
    void output(Voice& v, bool right);
    void v1(Voice& v);
    void v2(Voice& v);
    void v3(Voice& v) { v3a(v); v3b(v); v3c(v); }
    void v3a(Voice& v);
    void v3b(Voice& v);
    void v3c(Voice& v);
    void v4(Voice& v);
    void v5(Voice& v);
    void v6(Voice& v);
    void v7(Voice& v);
    void v8(Voice& v);
    void v9(Voice& v);
    void echo22(); void echo23(); void echo24(); void echo25(); void echo26();
    void echo27(); void echo28(); void echo29(); void echo30();

    uint8_t* m_ram;
    Voice m_voices[8];
    uint8_t m_ext[128] = {};       // what the SPC700 reads back
    uint8_t m_regs[128] = {};      // what the DSP uses
    int32_t m_noise = 0x4000;
    uint16_t m_counter = 0;
    uint8_t m_step = 0;
    uint8_t m_outBuf = 0, m_envBuf = 0, m_endBuf = 0;
    int32_t m_voiceOut = 0;
    int32_t m_outSamples[2] = {};
    int32_t m_pitch = 0;
    uint16_t m_sampleAddr = 0, m_brrNext = 0;
    uint8_t m_dir = 0, m_non = 0, m_pmon = 0, m_kon = 0, m_newKon = 0, m_koff = 0;
    uint8_t m_everyOther = 1, m_srcn = 0, m_brrHeader = 0, m_brrData = 0, m_looped = 0, m_adsr1 = 0;
    int32_t m_echoIn[2] = {}, m_echoOut[2] = {};
    int16_t m_echoHist[8][2] = {};
    uint16_t m_echoPtr = 0, m_echoLength = 0, m_echoOffset = 0;
    uint8_t m_echoHistPos = 0, m_esa = 0, m_eon = 0;
    bool m_echoEnabled = false;

    int16_t m_out[0x2000] = {};
    int m_outCount = 0;
};

}  // namespace snes
