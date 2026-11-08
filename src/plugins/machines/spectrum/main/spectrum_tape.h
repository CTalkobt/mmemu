#pragma once

#include "libdevices/main/io_handler.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class Z80;

namespace spectrum {

/**
 * A Spectrum cassette, after Fuse (tape.c, tape_traps.c) and libspectrum
 * (tap.c, tzx_read.c, tape_block.c): `.tap` and `.tzx` (also gzipped or in
 * a zip), played as pulses on EAR or loaded at once by the ROM trap.
 *
 * Pulses: the edges in T-states at 3.5 MHz (TZX's clock), scaled to the
 * machine's (the 128K's 3.5469 MHz). Blocks: standard and turbo data
 * (0x10, 0x11), pure tone (0x12), pulse sequence (0x13), pure data (0x14),
 * direct recording (0x15), pause and stop (0x20, 0x2A), set level (0x2B),
 * loops (0x24/0x25); the text and information blocks are skipped. A `.tap`
 * block is a standard block with a 1-second pause.
 *
 * The ROM traps (48 BASIC's LD-BYTES at $056B and SA-BYTES at $04D0, as
 * Fuse's tape_load_trap / tape_save_trap): a standard block at the tape's
 * position is loaded (or verified) straight into memory, with the
 * registers as the ROM leaves them; a block SAVE writes is kept, and `tape
 * save` writes them as a `.tap`.
 */
class SpectrumTape : public IOHandler {
public:
    explicit SpectrumTape(uint32_t cpuHz) : m_cpuHz(cpuHz) {}

    const char* name() const override { return "Tape"; }
    uint32_t baseAddr() const override { return 0; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void reset() override {}
    void tick(uint64_t) override {}

    bool mountTape(const std::string& path) override;
    /** play, stop, rewind */
    void controlTape(const std::string& op) override;
    bool startTapeRecord() override { m_saved.clear(); m_recording = true; return true; }
    void stopTapeRecord() override { m_recording = false; }
    bool saveTapeRecording(const std::string& path) override;
    bool isTapeRecording() const override { return m_recording; }

    /** A tape from bytes (tests): `tzx` or `.tap`. */
    bool load(const std::vector<uint8_t>& bytes, bool tzx, std::string* err = nullptr);
    bool mounted() const { return !m_blocks.empty(); }
    bool playing() const { return m_playing; }
    size_t blockIndex() const { return m_block; }
    size_t blockCount() const { return m_blocks.size(); }
    /** Blocks SAVE wrote (flag, data, parity), as a `.tap` holds them. */
    const std::vector<std::vector<uint8_t>>& saved() const { return m_saved; }
    /** Stop at the 0x2A "stop if 48K" blocks. */
    void set48k(bool on) { m_is48 = on; }

    /** Start playing at T-state `now`. */
    void play(uint64_t now);
    void stop() { m_playing = false; }
    /** The EAR level at `now` (playing up to it). */
    bool ear(uint64_t now) { advance(now); return m_level; }
    void advance(uint64_t now);

    /** A read of the ULA port at `now` with B = `b` (Fuse loader.c): a tight
     *  loop counting in B plays the tape; other reads stop it. */
    void earRead(uint64_t now, uint8_t b);
    /** Loader detection on (the default). */
    void setAutoPlay(bool on) { m_autoPlay = on; }

    /** LD-BYTES: load the block at the tape's position if it is a standard
     *  one; false to let the ROM read the pulses. */
    bool trapLoad(Z80& cpu, const std::function<uint8_t(uint16_t)>& rd,
                  const std::function<void(uint16_t, uint8_t)>& wr);
    /** SA-BYTES: keep the block. */
    void trapSave(Z80& cpu, const std::function<uint8_t(uint16_t)>& rd);

private:
    struct Block {
        enum Kind { Data, Tone, Pulses, Direct, Pause, Stop, Stop48, Level } kind = Data;
        bool rom = false;                       // standard timings (TAP, TZX 0x10)
        uint32_t pilot = 2168, pilotCount = 0, sync1 = 667, sync2 = 735, zero = 855, one = 1710;
        uint32_t toneLen = 0, toneCount = 0, sampleT = 0, pauseMs = 0;
        int usedBits = 8;
        bool level = false;
        std::vector<uint8_t> data;
        std::vector<uint32_t> pulses;
    };
    // An edge: set the level (`set` -1 toggles), then wait `len` T-states (3.5 MHz)
    struct Edge { uint32_t len; int8_t set; };

    bool parseTap(const std::vector<uint8_t>& b, std::string* err);
    bool parseTzx(const std::vector<uint8_t>& b, std::string* err);
    void expand(const Block& b);                // the block's edges into m_edges
    bool nextEdge();                            // take the next edge; false at the end
    uint64_t scaled(uint32_t len);

    uint32_t m_cpuHz;
    std::vector<Block> m_blocks;
    size_t m_block = 0;
    std::vector<Edge> m_edges;
    size_t m_edge = 0;
    bool m_playing = false, m_level = false, m_is48 = true;
    uint64_t m_nextAt = 0, m_scaleRem = 0;
    std::vector<std::vector<uint8_t>> m_saved;
    bool m_recording = false;
    bool m_startPending = false;                // `tape play`: starts at the next advance
    bool m_autoPlay = true;
    int m_reads = 0;
    uint64_t m_lastReadT = 0;
    uint8_t m_lastB = 0;
};

}  // namespace spectrum
