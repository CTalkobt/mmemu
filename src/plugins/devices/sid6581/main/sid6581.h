#pragma once

#include "libdevices/main/io_handler.h"
#include "libdevices/main/iaudio_output.h"
#include "libdevices/main/iaudio_inspect.h"
#include "combined_waveforms.h"
#include "sid_voice.h"
#include <cstdint>
#include <string>

/**
 * MOS 6581 Sound Interface Device (SID).
 *
 * Used as the audio chip in the Commodore 64 ($D400–$D41C).
 *
 * Implements (clocked every cycle; the oscillators and envelopes are reSIDfp's,
 * see sid_voice.h; #123):
 *   - Three voices, each with:
 *       24-bit phase accumulator (advances by FREQ register each clock cycle)
 *       Four waveforms: Triangle, Sawtooth, Pulse (variable duty cycle), Noise
 *         (23-bit LFSR, taps 22 and 17, shifted in two phases after
 *         accumulator bit 19 rises)
 *       Combined waveforms: pulldown model fitted to real chips (#119), with
 *         noise writeback (noise lockup)
 *       ADSR envelope: 15-bit rate LFSR + exponential counter, ADSR delay
 *         bug, the state-change pipeline
 *       Hard sync and ring modulation from the previous voice
 *       Test bit: holds the oscillator at 0; the LFSR fades to 1s
 *       No waveform: the DAC input floats, then fades (OSC3)
 *   - State-variable filter (#120): measured 6581/8580 cutoff curves,
 *       Q = 0.707 + res/15, integrators limited by the op-amps' measured
 *       output swing (#121); LP/BP/HP mix, per-voice routing, voice 3 off
 *   - Mixer with the 6581's DC offsets, scaled by the master volume, so
 *       $D418 writes alone play samples (#122); C64 output RC filters
 *   - Read-only registers: OSC3 (voice 3 waveform output), ENV3 (envelope level)
 *   - Paddle X/Y ($D419/$D41A) return 0xFF (A/D not implemented)
 *
 * The per-cycle output is averaged down to the sample rate.
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
 *
 * IAudioInspect (#30): per-voice state and a scope history of each voice
 * (waveform x envelope, before the filter) and of the mixed output.
 */
class SID6581 : public IOHandler, public IAudioOutput, public IAudioInspect {
public:
    SID6581() : m_name("SID"), m_baseAddr(0xD400) { SID6581::reset(); }
    SID6581(const std::string& name, uint32_t baseAddr);
    ~SID6581() override = default;

    void setName(const std::string& name) override { m_name = name; }
    void setBaseAddr(uint32_t addr) override       { m_baseAddr = addr; }
    void setSampleRate(int hz)                     { m_sampleRate = hz; }
    void setClockHz(uint32_t hz) override          { m_clockHz = hz; }
    void setFilterVariant(int variant) {           // 0=6581, 1=8580
        m_filterVariant = variant;
        for (auto& v : m_voices) v.wave.setChip(chipModel());
    }

    // -----------------------------------------------------------------------
    // IOHandler interface  ($D400–$D41C, 29 registers)
    // -----------------------------------------------------------------------

    const char* name()     const override { return m_name.c_str(); }
    uint32_t    baseAddr() const override { return m_baseAddr; }
    uint32_t    addrMask() const override { return 0x001F; } // 32-byte window

    bool ioRead (IBus* bus, uint32_t addr, uint8_t* val) override;
    bool ioPeek (IBus* bus, uint32_t addr, uint8_t* val) override;
    bool ioWrite(IBus* bus, uint32_t addr, uint8_t  val) override;
    void reset()  override;
    void tick(uint64_t cycles) override;
    void getDeviceInfo(DeviceInfo& out) const override;

    // -----------------------------------------------------------------------
    // IAudioOutput interface
    // -----------------------------------------------------------------------

    int nativeSampleRate() const override { return m_sampleRate; }
    int pullSamples(float* buffer, int maxSamples) override;
    /** A second SID fitted at run time (a stereo-SID cartridge): "mix" links
     *  its output into this one's (the frontends play one sound device). */
    void setLinkedDevice(const char* role, IOHandler* dev) override;
    /** An expansion's option: "base" ($D420, $D500, $DE00, $DF00...). */
    bool setOption(const std::string& key, const std::string& value, std::string& err) override;

    // -----------------------------------------------------------------------
    // IAudioInspect interface (#30): one chip, 3 voices + output channel
    // -----------------------------------------------------------------------

    std::string chipName(int chip) const override;
    int  voiceCount(int /*chip*/) const override { return 3; }
    bool voiceInfo(int chip, int voice, VoiceInfo& out) const override;
    int  scopeSampleRate() const override { return m_sampleRate; }
    int  scopeCapacity() const override { return SCOPE_LEN; }
    int  scopeSamples(int chip, int channel, float* out, int n) const override;
    uint64_t scopeTotal(int /*chip*/) const override { return m_scopeTotal; }

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
    // Per-voice state (clocked every cycle, #123)
    // -----------------------------------------------------------------------
    struct Voice {
        SidWave     wave;     // oscillator, noise shift register, OSC3
        SidEnvelope env;      // envelope generator, ENV3

        // Register mirrors (inspection)
        uint16_t freq  = 0;
        uint16_t pw    = 0;   // 12-bit pulse width
        uint8_t  cr    = 0;   // control
        uint8_t  ad    = 0;   // attack/decay nibbles
        uint8_t  sr    = 0;   // sustain/release nibbles

        enum class Env { IDLE, ATTACK, DECAY, SUSTAIN, RELEASE };   // for inspection
        Env envPhase() const;
        uint8_t envLevel() const { return env.output(); }
    };

    void    writeVoiceReg(Voice& v, int reg, uint8_t val);
    void    synthesize(uint64_t cycles);
    CombinedWaveformTable::Chip chipModel() const {
        return m_filterVariant == 0 ? CombinedWaveformTable::Chip::MOS6581
                                    : CombinedWaveformTable::Chip::MOS8580;
    }
    void    pushSample(float s);
    void    updateFilterCoeffs();

    std::string  m_name;
    uint32_t     m_baseAddr;

    uint8_t      m_regs[32];     // raw register storage (32-byte aligned)
    uint8_t      m_busValue = 0; // the data bus as the last access left it (reSID bus_value)
    uint32_t     m_busTtl = 0;   // cycles until it fades to 0
    /** reSID databus_ttl: $1D00 cycles on the 6581, $A2000 on the 8580. */
    uint32_t     busTtl() const { return chipModel() == CombinedWaveformTable::Chip::MOS8580 ? 0xA2000u : 0x1D00u; }
    bool         readReg(uint32_t addr, uint8_t* val, bool peek);
    Voice        m_voices[3];

    uint32_t     m_clockHz   = 985248;  // PAL default
    int          m_sampleRate = 44100;
    int          m_filterVariant = 0;   // Chip variant (0=6581, 1=8580)

    // Filter (#120/#121): state-variable filter clocked every cycle, in
    // voice units (one voice's full swing = VOICE_RANGE). States hold the AC
    // part; the voices' DC through the filter is added analytically.
    float        m_vhp = 0.0f, m_vbp = 0.0f, m_vlp = 0.0f;
    float        m_w0dt = 0.0f;         // 2*pi*f0*dt for the current cutoff
    float        m_invQ = 1.0f;         // 1/Q
    float        m_filterLo = 0.0f, m_filterHi = 0.0f;   // op-amp headroom (#121)
    bool         m_coeffValid = false;

    // External output stage (C64 RC filters, as reSID): low-pass ~16 kHz,
    // high-pass ~16 Hz; removes the DC the mixer adds.
    float        m_extLp = 0.0f, m_extHp = 0.0f;

    // Resampling: box average of the per-cycle output over each sample period
    uint64_t     m_sampleFrac = 0;
    double       m_sampleSum  = 0.0;
    uint32_t     m_sampleCycles = 0;

    // Scope history for IAudioInspect: voices 0-2 and the output (channel 3),
    // one entry per output sample.
    static constexpr int SCOPE_LEN = 4096;   // power of two
    float   m_scope[4][SCOPE_LEN] = {};
    int     m_scopePos   = 0;   // next write index
    int     m_scopeCount = 0;   // valid entries (<= SCOPE_LEN)
    uint64_t m_scopeTotal = 0;  // entries since reset

    IAudioOutput* m_mix = nullptr;          // a second SID mixed in
    const class IPotSource* m_pots = nullptr;   // what POTX/POTY read (a 1351 mouse)
    // Ring buffer for pullSamples()
    static constexpr int AUDIO_BUF = 8192;
    float   m_audioBuf[AUDIO_BUF] = {};
    int     m_audioBufRead  = 0;
    int     m_audioBufWrite = 0;
    int     m_audioBufCount = 0;
};
