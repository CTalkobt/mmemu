#pragma once

#include "libdevices/main/io_handler.h"
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace coco {

/**
 * The CoCo's cassette: a recorder whose motor the CoCo switches (PIA1
 * CA2), its playback on the comparator at PIA1 PA0, recording from the
 * 6-bit DAC (PIA1 PA2-PA7).
 *
 * Formats: .cas (and the MC-10's .c10: the bytes as BASIC writes them; each bit one cycle,
 * least significant first: a 1 a short cycle of about 2 kHz, a 0 a long
 * one of about 1.1 kHz, the timings XRoar uses), and .wav (8/16-bit PCM,
 * any rate, mono or stereo: played by its zero crossings; saved as 8-bit
 * mono at 22050 Hz).
 *
 * Playback runs while PLAY is down (mounting presses it) and the motor is
 * on. Recording (startTapeRecord) captures the DAC while the motor is on;
 * saveTapeRecording writes .cas (decoded from the half-cycles: two short
 * halves a 1, two long a 0, bytes aligned on the $3C sync byte after each
 * leader) or .wav by the file's extension.
 *
 * Time is in ticks of the machine's crystal (16 a CPU cycle at 0.89 MHz):
 * 14.31818 MHz, or a PAL board's (setMasterHz) for .wav timing.
 */
class Tape : public IOHandler {
public:
    static constexpr uint32_t TICKS_BIT0 = 813 * 16;    ///< cycle of a 0 (about 1100 Hz)
    static constexpr uint32_t TICKS_BIT1 = 435 * 16;    ///< cycle of a 1 (about 2060 Hz)

    const char* name() const override { return "Tape"; }
    uint32_t baseAddr() const override { return 0; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void reset() override {}
    void tick(uint64_t) override {}

    bool mountTape(const std::string& path) override;
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

    // The machine's side
    void setMasterHz(double hz) { m_hz = hz; }
    void setMotor(bool on) { m_motor = on; }
    bool motor() const { return m_motor; }
    bool playing() const { return m_play && m_motor && m_pos < m_pulses.size(); }
    /** Time passes. */
    void clock(uint64_t ticks);
    /** The comparator's output (PIA1 PA0): true = 1. */
    bool input() const { return m_level; }
    /** The DAC's 6-bit output, for recording. */
    void output(uint8_t dac6);

    size_t position() const { return m_pos; }
    size_t length() const { return m_pulses.size(); }
    const std::string& path() const { return m_path; }

private:
    struct Pulse {
        uint32_t ticks;
        bool level;        // PA0 during the pulse
    };
    double m_hz = 14318180.0;
    std::vector<Pulse> m_pulses;
    size_t m_pos = 0;
    uint64_t m_left = 0;
    bool m_level = true;
    bool m_motor = false, m_play = false;
    std::string m_path;

    bool m_recording = false;
    uint64_t m_now = 0;
    uint8_t m_lastDac = 0xFF;
    std::vector<std::pair<uint64_t, uint8_t>> m_rec;     // (tick, 6-bit DAC level) changes
};

}  // namespace coco
