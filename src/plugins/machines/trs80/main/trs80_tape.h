#pragma once

#include "libdevices/main/io_handler.h"
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace trs80 {

/**
 * The TRS-80's cassette recorder.
 *
 * The signal is three-level, as the machine drives it (port $FF bits 1-0):
 * 0 the middle (no signal), 1 high, 2 low. Playback feeds the same levels
 * back: the 250/500 baud input is a flip-flop set by a rise out of the
 * middle and cleared by any write to the cassette port; the Model III/4's
 * 1500 baud input sees each rise and fall (as interrupts) and the level.
 *
 * Formats (after xtrs, Tim Mann):
 *  - .cas: the bits as recorded, most significant first. Played back with
 *    the pulse shapes of the speed the machine reads at (speed()): 250 baud
 *    (Level I: a clock pulse each 3.8 ms bit, a data pulse between for a 1),
 *    500 baud (Level II, Model III low speed: the same at 2.1 ms) and 1500
 *    baud (Model III/4 high speed: one cycle a bit, 752 us for a 0, 376 us
 *    for a 1). Recording decodes the pulses back to bytes.
 *  - .wav: 8/16-bit PCM, any rate, mono or stereo, played by its levels
 *    (above, below or near the middle); saved as 8-bit mono 22050 Hz.
 *
 * Playback runs while the motor is on (and PLAY is down: mounting presses
 * it). Recording captures the machine's output while the motor is on.
 * Time is in the machine's master clock ticks (setMasterHz).
 */
class Tape : public IOHandler {
public:
    /** The speed a .cas image plays at. */
    enum class Speed { Baud250, Baud500, Baud1500 };

    const char* name() const override { return "Tape"; }
    uint32_t baseAddr() const override { return 0; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void reset() override {}
    void tick(uint64_t) override {}

    bool mountTape(const std::string& path) override;
    /** "play", "stop", "rewind" (and "eject"). */
    void controlTape(const std::string& op) override;
    bool startTapeRecord() override;
    void stopTapeRecord() override;
    bool saveTapeRecording(const std::string& path) override;
    bool isTapeRecording() const override { return m_recording; }

    /** A .cas image's bytes as the tape. */
    void loadCas(const std::vector<uint8_t>& bytes);
    /** A .wav file's bytes; false if it isn't PCM WAV. */
    bool loadWav(const std::vector<uint8_t>& bytes);
    /** The recording decoded to .cas bytes. */
    std::vector<uint8_t> recordedCas() const;
    /** The recording as an 8-bit mono 22050 Hz WAV file. */
    std::vector<uint8_t> recordedWav() const;

    // The machine's side ---------------------------------------------------
    void setMasterHz(double hz) { m_hz = hz; }
    /** How a .cas plays from the next bit on (the machine's read speed). */
    std::function<Speed()> speed;
    /** The 1500 baud input's edges: rise (true) or fall. */
    std::function<void(bool rise)> onEdge;
    /** The relay. */
    void setMotor(uint64_t now, bool on);
    bool motor() const { return m_motor; }
    bool playing() const { return m_play && m_motor && !atEnd(); }
    /** Bring playback up to `now`. */
    void advance(uint64_t now);
    /** The 500/250 baud flip-flop (port $FF bit 7) and its reset (a write to the port). */
    bool flipflop() const { return m_flipflop; }
    void clearFlipflop() { m_flipflop = false; }
    /** The 1500 baud input: the last level away from the middle was high. */
    bool highLevel() const { return m_lastNonzero == 1; }
    /** The level being played (0 middle, 1 high, 2 low). */
    int level() const { return m_value; }
    /** The machine's output (port $FF bits 1-0) at `now`. */
    void output(uint64_t now, int level);

    bool mounted() const { return !m_cas.empty() || !m_wav.empty(); }
    const std::string& path() const { return m_path; }
    /** Bytes (.cas) or transitions (.wav) played so far. */
    size_t position() const { return m_isWav ? m_wavPos : m_casPos; }
    size_t length() const { return m_isWav ? m_wav.size() : m_cas.size(); }

private:
    struct Edge {
        uint32_t ticks;    // until the next level
        uint8_t level;     // the level from here
    };
    bool atEnd() const {
        if (!mounted()) return true;
        if (m_pending) return false;
        return m_isWav ? m_wavPos >= m_wav.size()
                       : m_casPos > m_cas.size() && m_bit < 0 && m_pulse >= m_pulses.size();
    }
    bool nextTransition();
    uint64_t us(double us) const { return (uint64_t)(us * m_hz / 1e6 + 0.5); }

    double m_hz = 10644480.0;
    std::string m_path;
    bool m_play = false, m_motor = false;

    // .cas playback: a byte at a time, each bit as its speed's pulses
    std::vector<uint8_t> m_cas;
    size_t m_casPos = 0;            // the next byte
    int m_byte = -1, m_bit = -1;    // the byte and bit being played (-1: none)
    std::vector<std::pair<double, int>> m_pulses;   // (us until, level) left of this bit
    size_t m_pulse = 0;

    // .wav playback
    bool m_isWav = false;
    std::vector<Edge> m_wav;
    size_t m_wavPos = 0;

    // Where playback is
    uint64_t m_now = 0;             // ticks played to
    uint64_t m_nextAt = 0;          // the next transition
    int m_next = 0;                 // its level
    bool m_pending = false;
    int m_value = 0, m_lastNonzero = 0;
    bool m_flipflop = false;

    // Recording: (tick, level) of each change
    bool m_recording = false;
    int m_outLevel = 0;
    std::vector<std::pair<uint64_t, int>> m_rec;
};

}  // namespace trs80
