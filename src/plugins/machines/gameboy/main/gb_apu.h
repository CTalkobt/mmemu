#pragma once

// The Game Boy's APU, after Mesen2 (Core/Gameboy/APU/: GbApu, GbSquareChannel,
// GbWaveChannel, GbNoiseChannel, GbEnvelope, GbChannelDac; GPL-3.0, Sour and
// the Mesen contributors): two square channels (the first with the sweep),
// the wave channel with its DMG-only wave RAM access and corruption, the
// noise channel, the frame sequencer clocked from DIV, length counters with
// the extra clocking quirk, the envelope's "zombie mode", and the DACs'
// slow fade in and out. On the CGB (as Mesen): wave RAM reads and writes
// reach the byte the channel is playing at any time and retriggering doesn't
// corrupt it, powering on clears the length counters, and PCM12/PCM34 read
// the channels' digital outputs.
//
// Where SameSuite's and AGE's tests (verified on hardware) differ from Mesen:
// the envelope is SameBoy's (apu.c, MIT: a countdown and a clock raised on
// the frame sequencer bit's rising edge), frequency changes as the square's
// timer reloads, channel 1's sweep held just after a trigger, its overflow a
// moment after the step, the frame sequencer's delay after a switch to
// double speed (AGE's), and the noise channel: SameBoy's counter clocking
// the LFSR, its trigger alignment and NR43 glitches (channel_4_freq_change).
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
    /** DIV's frame sequencer bit falling: a step. */
    void clockFrameSequencer();
    /** And rising (the envelopes' clocks are raised then). */
    void frameSequencerRise();
    /** The CGB's speed changed (to double: `high`). */
    void speedChanged(bool high);
    /** The frame sequencer's steps come late after the speed switch (AGE's double speed delay). */
    bool speedSwitchDelay() const { return m_speedSwitchDelay; }
    uint8_t read(uint16_t addr);
    uint8_t peek(uint16_t addr) const;
    void write(uint16_t addr, uint8_t v);
    /** The CGB's PCM12 ($FF76: square 1 low, square 2 high) and PCM34 ($FF77: wave, noise), as they are now. */
    uint8_t pcm(uint16_t addr) const;

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
        uint8_t volume = 0, envVolume = 0, envPeriod = 0, envCountdown = 0;
        bool envRaise = false, envClock = false, envLocked = false, envShouldLock = false;
    };
    struct Square : Envelope {
        uint16_t frequency = 0, timer = 0;
        uint16_t sweepTimer = 0, sweepFreq = 0, sweepPeriod = 0;
        uint8_t sweepUpdateDelay = 0, sweepOverflowDelay = 0, sweepShift = 0;
        bool sweepNegate = false, sweepEnabled = false, sweepNegateCalcDone = false;
        uint8_t duty = 0, length = 0;
        bool lengthEnabled = false, enabled = false, firstStep = false;
        uint8_t dutyPos = 0, output = 0;
        uint64_t triggerClock = 0;
        Dac dac;
    };
    static constexpr uint64_t SWEEP_RESTART_HOLD = 10;
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
        bool lengthEnabled = false, enabled = false;
        uint8_t nr43 = 0, nr44 = 0;
        bool narrow = false;
        uint16_t lfsr = 0;                   // SameBoy's sense: 0 at a trigger, the output on with bit 0 set
        uint16_t counter = 0;                // 14 bits; bit (NR43 >> 4) rising clocks the LFSR
        uint8_t countdown = 0;               // 2 MHz cycles to the counter's next increment
        uint8_t alignment = 0;               // 2 MHz cycles since power on
        uint8_t half = 0;                    // an APU clock left over (double speed)
        bool counterActive = false, backgroundActive = false, startedWithDacOff = false;
        bool didStepCounter = false, countdownReloaded = false;
        bool steppedInNarrow = false, bit7BeforeStep = false;
        bool feedbackLatch = false;          // the DMG's feedback, latched on the counter bit's falling edge
        uint8_t dmgDelayedStart = 0;
        uint8_t output = 0;
        Dac dac;
    };

    // Square channels (Mesen's GbSquareChannel)
    void sqDisable(Square& s);
    void sqClockSweep(Square& s);
    uint16_t sqSweepTarget(const Square& s) const;
    template <class T> void clockLength(T& c);
    void sqUpdateOutput(Square& s);
    void sqSetFrequency(Square& s, uint16_t f, bool sweep = false);
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
    // Noise (SameBoy's)
    void noiseDisable();
    void noiseUpdateOutput();
    bool noiseFeedback() const;
    void noiseShift(bool highBit);
    void noiseClock();
    uint32_t noiseNextEvent() const;
    void noiseExec(uint32_t clocks);
    void noiseCycles(uint32_t cycles);
    void noiseTrigger();
    void noisePrepareStart();
    void noiseNr43(uint8_t v);
    void noiseWrite(int reg, uint8_t v);
    // Envelopes (Mesen's GbEnvelope), the channel's output refreshed by `update`
    template <class T, class U> void clockEnvelope(T& s, U update);
    template <class T> void armEnvelope(T& s);
    template <class T> void setEnvelopeClock(T& s, bool on);
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
    bool m_speedSwitchDelay = false;
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
