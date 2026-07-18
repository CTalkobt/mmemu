#pragma once

#include "libdevices/main/io_handler.h"
#include "libdevices/main/iaudio_output.h"
#include "combined_waveforms.h"
#include <cstdint>
#include <string>

/**
 * MOS 6581 Sound Interface Device (SID).
 *
 * Used as the audio chip in the Commodore 64 ($D400–$D41C).
 *
 * Implements:
 *   - Three voices, each with:
 *       24-bit phase accumulator (advances by FREQ register each clock cycle)
 *       Four waveforms: Triangle, Sawtooth, Pulse (variable duty cycle), Noise
 *         (23-bit maximal-length LFSR, taps at 22 and 17; clocked on phase bit-19 rise)
 *       Combined waveforms: complex harmonic patterns via analog bit-mixing lookup tables
 *         (empirically-derived reSIDfp model; replaces incorrect AND operation)
 *       ADSR envelope generator (8-bit level, linear A, linear D/R approximation)
 *       Gate (0→1 triggers attack; 1→0 triggers release)
 *       Hard sync: voice N oscillator reset by voice N-1's phase overflow
 *       Ring modulation: triangle output XOR'd with MSB of voice N-1's oscillator
 *       Test bit: freezes oscillator, loads LFSR with all-1s
 *   - Multi-mode Chamberlin state-variable filter:
 *       11-bit cutoff frequency register, 4-bit resonance
 *       Filter routing: each voice individually routed to filter ($D417 bits 0-2)
 *       Mode: LP/BP/HP individually selectable ($D418 bits 4-6)
 *       Voice-3-disconnect ($D418 bit 7)
 *   - Volume register ($D418 bits 0-3, 0–15)
 *   - Read-only registers: OSC3 (phase accumulator MSB), ENV3 (envelope level)
 *   - Paddle X/Y ($D419/$D41A) return 0xFF (A/D not implemented)
 *
 * IAudioOutput pull model:
 *   tick() synthesises samples into an internal 8192-sample ring buffer.
 *   The host audio layer calls pullSamples() to drain it.
 *   Mono float32 at the configured sample rate (default 44100 Hz).
 *
 * Usage:
 *   sid->setSampleRate(44100);
 *   sid->setClockHz(985248); // PAL; NTSC = 1022727
 *   ioRegistry->registerHandler(sid);
 *   audioOutput->start(sid);
 */
class SID6581 : public IOHandler, public IAudioOutput {
public:
    SID6581() : m_name("SID"), m_baseAddr(0xD400) { SID6581::reset(); }
    SID6581(const std::string& name, uint32_t baseAddr);
    ~SID6581() override = default;

    void setName(const std::string& name) override { m_name = name; }
    void setBaseAddr(uint32_t addr) override       { m_baseAddr = addr; }
    void setSampleRate(int hz)                     { m_sampleRate = hz; }
    void setClockHz(uint32_t hz) override          { m_clockHz = hz; }

    // -----------------------------------------------------------------------
    // IOHandler interface  ($D400–$D41C, 29 registers)
    // -----------------------------------------------------------------------

    const char* name()     const override { return m_name.c_str(); }
    uint32_t    baseAddr() const override { return m_baseAddr; }
    uint32_t    addrMask() const override { return 0x001F; } // 32-byte window

    bool ioRead (IBus* bus, uint32_t addr, uint8_t* val) override;
    bool ioWrite(IBus* bus, uint32_t addr, uint8_t  val) override;
    void reset()  override;
    void tick(uint64_t cycles) override;
    void getDeviceInfo(DeviceInfo& out) const override;

    // -----------------------------------------------------------------------
    // IAudioOutput interface
    // -----------------------------------------------------------------------

    int nativeSampleRate() const override { return m_sampleRate; }
    int pullSamples(float* buffer, int maxSamples) override;

    // -----------------------------------------------------------------------
    // Register offsets (relative to base address)
    // -----------------------------------------------------------------------
    // Voice N starts at base + N*7
    enum Reg {
        // Voice 1
        V1_FREQ_LO = 0x00, V1_FREQ_HI = 0x01,
        V1_PW_LO   = 0x02, V1_PW_HI   = 0x03,
        V1_CR      = 0x04, V1_AD      = 0x05, V1_SR = 0x06,
        // Voice 2
        V2_FREQ_LO = 0x07, V2_FREQ_HI = 0x08,
        V2_PW_LO   = 0x09, V2_PW_HI   = 0x0A,
        V2_CR      = 0x0B, V2_AD      = 0x0C, V2_SR = 0x0D,
        // Voice 3
        V3_FREQ_LO = 0x0E, V3_FREQ_HI = 0x0F,
        V3_PW_LO   = 0x10, V3_PW_HI   = 0x11,
        V3_CR      = 0x12, V3_AD      = 0x13, V3_SR = 0x14,
        // Filter
        FC_LO      = 0x15, FC_HI      = 0x16,
        RES_FILT   = 0x17,
        MODE_VOL   = 0x18,
        // Read-only
        POTX       = 0x19, POTY       = 0x1A,
        OSC3       = 0x1B, ENV3       = 0x1C
    };

    // Control register bit masks (same for all 3 voices)
    static constexpr uint8_t CR_GATE  = 0x01;
    static constexpr uint8_t CR_SYNC  = 0x02;
    static constexpr uint8_t CR_RING  = 0x04;
    static constexpr uint8_t CR_TEST  = 0x08;
    static constexpr uint8_t CR_TRI   = 0x10;
    static constexpr uint8_t CR_SAW   = 0x20;
    static constexpr uint8_t CR_PULSE = 0x40;
    static constexpr uint8_t CR_NOISE = 0x80;

    // MODE_VOL bit masks
    static constexpr uint8_t MV_LP    = 0x10;
    static constexpr uint8_t MV_BP    = 0x20;
    static constexpr uint8_t MV_HP    = 0x40;
    static constexpr uint8_t MV_3OFF  = 0x80; // disconnect voice 3 from output

private:
    // -----------------------------------------------------------------------
    // Per-voice state
    // -----------------------------------------------------------------------
    struct Voice {
        // Oscillator
        uint32_t phase    = 0;       // 24-bit phase accumulator (stored in 32-bit)
        uint32_t lfsr     = 0x7FFFFF; // 23-bit noise LFSR
        uint32_t prevBit19 = 0;      // tracks bit-19 state for LFSR clocking

        // Envelope
        enum class Env { IDLE, ATTACK, DECAY, SUSTAIN, RELEASE } envPhase = Env::IDLE;
        uint8_t  envLevel = 0;       // 0–255
        uint64_t envAccum = 0;       // cycle accumulator for envelope timing

        // Register mirrors (updated from the raw register array)
        uint16_t freq  = 0;
        uint16_t pw    = 0;   // 12-bit pulse width
        uint8_t  cr    = 0;   // control
        uint8_t  ad    = 0;   // attack/decay nibbles
        uint8_t  sr    = 0;   // sustain/release nibbles

        bool prevGate  = false;

        // Waveform output: returns 12-bit value (0–4095)
        // Multiple active waveforms are ANDed.
        uint16_t waveOutput(bool syncReset, bool ringMsb) const;
        // Noise output bits {20,18,14,11,9,5,3,0} of LFSR → 12-bit value
        uint16_t noiseOutput() const;
    };

    // -----------------------------------------------------------------------
    // Filter state (Chamberlin state-variable)
    // -----------------------------------------------------------------------
    struct Filter {
        float lp = 0.0f;  // low-pass memory
        float bp = 0.0f;  // band-pass memory

        // Process one sample; returns {lp, bp, hp} mixed per mode flags.
        float process(float in, float f, float q, uint8_t mode);
    };

    void    synthesize(uint64_t cycles);
    void    tickVoice(Voice& v, Voice& syncSrc, uint64_t cycles, uint32_t clockHz);
    float   voiceOutput(const Voice& v) const;
    void    pushSample(float s);

    // Envelope rates: cycles per envelope-level step at the configured clock.
    static uint32_t atkCycles(uint8_t nibble, uint32_t clockHz);
    static uint32_t decRelCycles(uint8_t nibble, uint32_t clockHz);

    std::string  m_name;
    uint32_t     m_baseAddr;

    uint8_t      m_regs[32];     // raw register storage (32-byte aligned)
    Voice        m_voices[3];
    Filter       m_filter;

    uint32_t     m_clockHz   = 985248;  // PAL default
    int          m_sampleRate = 44100;

    // Fractional sample accumulator (same approach as VIC-I)
    uint64_t     m_sampleFrac = 0;

    // Ring buffer for pullSamples()
    static constexpr int AUDIO_BUF = 8192;
    float   m_audioBuf[AUDIO_BUF] = {};
    int     m_audioBufRead  = 0;
    int     m_audioBufWrite = 0;
    int     m_audioBufCount = 0;
};
