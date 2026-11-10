#pragma once

// The Game Boy's APU, after Mesen2 (Core/Gameboy/APU/: GbApu, GbSquareChannel,
// GbWaveChannel, GbNoiseChannel, GbEnvelope, GbChannelDac; GPL-3.0, Sour and
// the Mesen contributors): two square channels (the first with the sweep),
// the wave channel with its DMG-only wave RAM access and corruption, the
// noise channel, the frame sequencer clocked from DIV, length counters with
// the extra clocking quirk, the envelope's "zombie mode", and the DACs'
// slow fade in and out.
//
// The output is Mesen's level, ((7 - digital) * DAC) per channel summed times
// (master volume + 1) * 40, box-filtered down to the sample rate asked for.

#include <cstdint>
#include <deque>

namespace gb {

class Gameboy;

class Apu {
public:
    void attach(Gameboy* gb) { m_gb = gb; }
    /** As at power on (Mesen's Init: everything cleared, the wave RAM's DMG pattern). */
    void init();
    /** Catch up with the Game Boy's clock. */
    void run();
    void clockFrameSequencer();
    uint8_t read(uint16_t addr);
    uint8_t peek(uint16_t addr) const;
    void write(uint16_t addr, uint8_t v);

    /** Output `rate` samples a second when the Game Boy's clock runs at `clockHz` (default 32 kHz, 4.19 MHz). */
    void setSampleRate(double rate, double clockHz);
    /** Stereo samples made so far (left, right); taken by the caller. */
    std::deque<int16_t>& samples() { return m_samples; }
    /** The level now (for tests). */
    int16_t levelLeft() const { return m_prevLeft; }
    int16_t levelRight() const { return m_prevRight; }

    bool oddApuCycle() const;
    uint64_t elapsedApuCycles() const;

private:
    struct Dac {
        int16_t counter = 0;
        uint16_t volume = 0;
        void exec(uint32_t clocks, bool enabled) {
            counter = (int16_t)(counter - (int32_t)clocks);
            if (counter <= 0) {
                if (enabled) volume = volume < 100 ? volume + 1 : 100;
                else volume = volume > 0 ? volume - 1 : 0;
                counter = (int16_t)(counter + 250);
            }
        }
    };
    struct Envelope {
        uint8_t volume = 0, envVolume = 0, envPeriod = 0, envTimer = 0;
        bool envRaise = false, envStopped = false;
    };
    struct Square : Envelope {
        uint16_t frequency = 0, timer = 0;
        uint16_t sweepTimer = 0, sweepFreq = 0, sweepPeriod = 0;
        uint8_t sweepUpdateDelay = 0, sweepShift = 0;
        bool sweepNegate = false, sweepEnabled = false, sweepNegateCalcDone = false;
        uint8_t duty = 0, length = 0;
        bool lengthEnabled = false, enabled = false, firstStep = false;
        uint8_t dutyPos = 0, output = 0;
        Dac dac;
    };
    struct Wave {
        bool dacEnabled = false;
        uint8_t sampleBuffer = 0, ram[16] = {}, position = 0, volume = 0;
        uint16_t frequency = 0, length = 0;
        bool lengthEnabled = false, enabled = false;
        uint16_t timer = 0;
        uint8_t output = 0;
        bool allowRamAccess = false;
        Dac dac;
    };
    struct Noise : Envelope {
        uint8_t length = 0;
        bool lengthEnabled = false;
        uint16_t shiftRegister = 0;
        uint8_t periodShift = 0, divisor = 0;
        bool shortWidth = false, enabled = false;
        uint32_t timer = 0;
        uint8_t output = 0;
        Dac dac;
    };

    // Square channels (Mesen's GbSquareChannel)
    void sqDisable(Square& s);
    void sqClockSweep(Square& s);
    uint16_t sqSweepTarget(const Square& s) const;
    template <class T> void clockLength(T& c);
    void sqUpdateOutput(Square& s);
    void sqExec(Square& s, uint32_t clocks);
    uint8_t sqRead(const Square& s, int reg) const;
    void sqWrite(Square& s, int reg, uint8_t v);
    // Wave
    void waveDisable();
    void waveUpdateOutput();
    void waveExec(uint32_t clocks);
    void waveWrite(int reg, uint8_t v);
    void waveCorruption();
    uint8_t waveReadRam(uint16_t addr) const;
    void waveWriteRam(uint16_t addr, uint8_t v);
    // Noise
    void noiseDisable();
    uint32_t noisePeriod() const;
    void noiseUpdateOutput();
    void noiseExec(uint32_t clocks);
    void noiseWrite(int reg, uint8_t v);
    // Envelopes (Mesen's GbEnvelope), the channel's output refreshed by `update`
    template <class T, class U> void clockEnvelope(T& s, U update);
    template <class T, class U> void writeEnvelope(T& s, uint8_t v, U update);
    template <class L> void lengthEnableFlag(uint8_t v, L& length, bool& lengthEnabled, bool& enabled);

    static double analog(uint8_t output, const Dac& d) { return (7 - (int8_t)output) * (double)d.volume / 100; }
    void updateOutput();
    void span(uint32_t clocks);

    Gameboy* m_gb = nullptr;
    Square m_sq1, m_sq2;
    Wave m_wave;
    Noise m_noise;

    bool m_enabled = false;
    uint8_t m_frameStep = 0;
    uint8_t m_enLeft[4] = {}, m_enRight[4] = {};     // 0 or 0xFF: square 1, square 2, wave, noise
    uint8_t m_leftVolume = 0, m_rightVolume = 0;
    bool m_extLeft = false, m_extRight = false;
    int16_t m_prevLeft = 0, m_prevRight = 0;
    uint64_t m_prevClock = 0;
    uint32_t m_skipFirstEvent = 0;
    uint64_t m_powerOnCycle = 0;

    // Resampling
    double m_period = 4194304.0 / 32000.0, m_phase = 0;
    double m_accL = 0, m_accR = 0;
    std::deque<int16_t> m_samples;
};

}  // namespace gb
