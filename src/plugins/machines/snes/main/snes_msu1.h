#pragma once

// The MSU-1 (#15), after Mesen2 (Core/SNES/Coprocessors/MSU1/, Shared/Audio/PcmReader,
// Utilities/Audio/HermiteResampler; GPL-3.0, Sour and the Mesen contributors).
//
// byuu's MSU-1 is a cartridge add-on (made for homebrew and fan patches) with a
// data file read a byte at a time and CD-quality audio tracks, at $2000-$2007
// of banks $00-$3F/$80-$BF. As Mesen it is there when the ROM's folder has
// `<rom>.msu` (the data; tracks `<rom>-<n>.pcm`) or `msu1.rom` (tracks
// `track-<n>.pcm`). A track is "MSU1", the loop point (in samples) and then
// 44.1 kHz 16-bit stereo; it is resampled (Hermite, as Mesen) to the DSP's
// 32 kHz and added to its output, times the volume / 255.
//
// Unlike Mesen the audio follows the emulated time rather than the host's
// audio pulls, and a track that ends exactly as a batch of samples ends
// still loops (Mesen's stalls there).

#include <cstdint>
#include <fstream>
#include <memory>
#include <string>

namespace snes {

/** Something mixed into the DSP's output, a 32 kHz stereo sample at a time. */
class AudioSource {
public:
    virtual ~AudioSource() = default;
    virtual void mix(int32_t& left, int32_t& right) = 0;
};

class Msu1 : public AudioSource {
public:
    /** The MSU-1 for the ROM at `romPath`, or null when its folder has neither `<rom>.msu` nor `msu1.rom`. */
    static std::unique_ptr<Msu1> open(const std::string& romPath);
    /** `dataPath`: the data file; `trackPrefix`: tracks are `<trackPrefix>-<n>.pcm`. */
    Msu1(const std::string& dataPath, const std::string& trackPrefix);

    /** As when the cartridge is loaded (Mesen makes a new one). */
    void powerOn();

    /** $2000-$2007. */
    uint8_t read(uint16_t a, bool peek);
    void write(uint16_t a, uint8_t v);

    void mix(int32_t& left, int32_t& right) override;

    // For tests and the debugger
    bool playing() const { return !m_paused; }
    bool trackMissing() const { return m_trackMissing; }
    uint16_t track() const { return m_trackSelect; }
    uint32_t dataPointer() const { return m_dataPointer; }

private:
    void loadTrack(uint32_t startOffset = 8);
    /** The next source sample; false at the end (not looping). */
    bool nextSample(int16_t& l, int16_t& r);

    std::string m_dataPath, m_trackPrefix;
    std::ifstream m_data;
    uint32_t m_dataSize = 0;

    uint8_t m_volume = 100;
    uint16_t m_trackSelect = 0;
    uint32_t m_tmpDataPointer = 0, m_dataPointer = 0;
    bool m_repeat = false, m_paused = false, m_trackMissing = false;
    static constexpr bool AUDIO_BUSY = false, DATA_BUSY = false;     // always false, as Mesen

    // The track (Mesen's PcmReader)
    std::ifstream m_track;
    uint32_t m_trackSize = 0, m_trackOffset = 0, m_loopOffset = 0;
    bool m_done = true;
    // Hermite resampling, 44.1 kHz to 32 kHz
    double m_prevL[4] = {}, m_prevR[4] = {};
    double m_fraction = 0;
};

}  // namespace snes
