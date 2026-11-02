#pragma once

#include <cstdint>
#include <string>

/**
 * Optional interface for sound chips whose voices can be inspected by
 * debugger tools such as the GUI SID Scope pane (#30). Obtain it with
 * dynamic_cast from the chip's IOHandler.
 *
 * A handler may contain several chips (e.g. the MEGA65's SID pair). Each chip
 * has voiceCount() voices plus one extra scope channel, index voiceCount(),
 * holding the chip's mixed output.
 *
 * Scope samples are recorded at the chip's audio sample rate while the
 * machine runs: each voice as waveform x envelope before the filter, in
 * [-1, 1]; the output channel after filter and master volume.
 */
class IAudioInspect {
public:
    virtual ~IAudioInspect();

    enum class EnvPhase { Idle, Attack, Decay, Sustain, Release };

    struct VoiceInfo {
        uint16_t freq = 0;        ///< frequency register
        double   freqHz = 0.0;    ///< oscillator frequency at the chip clock
        uint16_t pulseWidth = 0;  ///< 12-bit pulse width register
        uint8_t  control = 0;     ///< control register (gate, sync, ring, test, waveforms)
        uint8_t  attack = 0, decay = 0, sustain = 0, release = 0;  ///< ADSR nibbles
        EnvPhase envPhase = EnvPhase::Idle;
        uint8_t  envLevel = 0;    ///< envelope level 0-255
        bool     filtered = false;  ///< routed through the filter
        bool     muted = false;     ///< disconnected from the output (SID voice 3 off)
    };

    virtual int chipCount() const { return 1; }
    /** Short name for the chip, e.g. "SID $D400". */
    virtual std::string chipName(int chip) const = 0;
    virtual int voiceCount(int chip) const = 0;
    virtual bool voiceInfo(int chip, int voice, VoiceInfo& out) const = 0;

    /** Sample rate of the scope channels in Hz. */
    virtual int scopeSampleRate() const = 0;
    /** Maximum number of samples scopeSamples() can return per channel. */
    virtual int scopeCapacity() const = 0;
    /**
     * Copy up to n of the most recent samples of a channel (0..voiceCount-1 =
     * voices, voiceCount = output), oldest first, into out.
     * @return number of samples written.
     */
    virtual int scopeSamples(int chip, int channel, float* out, int n) const = 0;
};
