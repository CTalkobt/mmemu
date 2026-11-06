#pragma once

// Atari 2600 TIA (Television Interface Adaptor): video, collisions, the
// fire-button and paddle inputs, and sound.
//
// After Stella's "new TIA" (src/emucore/tia/TIA.cxx/.hxx and friends; see
// tia_objects.h), by Christian Speckner (DirtyHairy), Stephen Anthony and the
// Stella team, GPL-2.0-or-later. The core is clocked every colour clock (no
// line cache). Left out: the debugger (toggles, fixed debug colours, access
// counters), developer settings (TIA revision quirks, random undriven pins,
// jitter), auto-phosphor, colour loss, serialization and Stella's frame-layout
// detection.

#include "libdevices/main/io_handler.h"
#include "libdevices/main/ivideo_output.h"
#include "libdevices/main/iaudio_output.h"
#include "tia_objects.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

/**
 * The TIA. The machine maps it and calls read()/write() itself (registers
 * $00-$0F read, $00-$3F write, after masking), and clocks it with clock() —
 * 3 colour clocks per CPU cycle. As in Stella, the TIA should be clocked up to
 * and including the CPU cycle of a register access before the access is made
 * (Stella advances its system clock before each bus access).
 *
 * WSYNC: write() sets haltRequested(); the machine then stalls the CPU (RDY)
 * by clocking clocksToLineEnd() colour clocks (Stella's TIA::onHalt; may be 0
 * at the very start of a line) and calls clearHalt(). The halt also clears by
 * itself when a new line starts.
 *
 * Video: Stella's FrameManager (without jitter) follows VSYNC. A VSYNC pulse
 * counts once it has lasted 2 lines; when it ends (or after 50 lines of
 * waiting with VBLANK on), the frame completes and the next starts. The frame
 * shows FRAME_HEIGHT lines starting YSTART lines after the end of VSYNC
 * (NTSC 228 lines from 23, PAL/SECAM 274 from 32), 160 pixels doubled to 320.
 * Completed frames are double-buffered; getDimensions() is constant.
 */
class Tia : public IOHandler, public IVideoOutput, public IAudioOutput {
public:
    enum class Standard { NTSC, PAL, SECAM };

    static constexpr int FRAME_WIDTH = 320;               ///< 160 TIA pixels, doubled
    static constexpr int NTSC_FRAME_HEIGHT = (int)tiacore::FrameManager::BASE_HEIGHT_NTSC;   // 228
    static constexpr int PAL_FRAME_HEIGHT = (int)tiacore::FrameManager::BASE_HEIGHT_PAL;     // 274
    static constexpr int DEFAULT_SAMPLE_RATE = 44100;
    static constexpr uint32_t CLOCKS_PER_LINE = tiacore::H_CLOCKS;                            // 228

    /** Colour clock in Hz (the CPU runs at a third of it). SECAM uses the PAL rate. */
    static constexpr uint32_t clockHz(Standard s) { return s == Standard::NTSC ? 3579545u : 3546894u; }

    explicit Tia(Standard s = Standard::NTSC);
    ~Tia() override = default;

    /** Selects palette, frame layout and paddle timing; resets the frame manager. */
    void setStandard(Standard s);
    Standard standard() const { return m_std; }

    // ------------------------------------------------------------------
    // Machine interface
    // ------------------------------------------------------------------
    /** Advance by colour clocks (3 per CPU cycle). */
    void clock(uint32_t colorClocks);
    /** A register read ($00-$0F after masking); bits 0-5 come from `dataBus` (the last byte on the bus). */
    uint8_t read(uint8_t reg, uint8_t dataBus);
    /** read() without side effects (the INPT4/5 latch and paddle charge are left alone), for the debugger. */
    uint8_t peek(uint8_t reg) const;
    /** A register write ($00-$3F after masking). */
    void write(uint8_t reg, uint8_t value);
    /** The last value written to a register (Stella's shadow registers), once it has taken effect. */
    uint8_t registerValue(uint8_t reg) const { return m_shadow[reg & 0x3F]; }

    /** WSYNC was written: the CPU must wait (RDY) until the next line starts. */
    bool haltRequested() const { return m_halt; }
    void clearHalt() { m_halt = false; }
    /** Colour clocks until the current line ends (hctr wraps to 0), for WSYNC. */
    uint32_t clocksToLineEnd() const { return (tiacore::H_CLOCKS - m_hctr) % tiacore::H_CLOCKS; }

    // Beam position for the debugger
    /** Horizontal colour clock in the line, 0-227 (68 = first visible pixel). */
    int hpos() const { return m_hctr; }
    /** Lines since the last frame completed. */
    int scanline() const { return (int)m_frameManager.currentFrameTotalLines(); }
    /** Frames completed since reset. */
    uint64_t frameCount() const { return m_frameManager.totalFrames(); }
    /** Lines in the last completed frame. */
    int linesLastFrame() const { return (int)m_frameManager.lastFrameLines(); }
    /** Colour clocks since reset. */
    uint64_t totalClocks() const { return m_timestamp; }
    bool vsyncOn() const { return m_frameManager.vsync(); }
    bool vblankOn() const { return m_frameManager.vblank(); }

    // Inputs
    /** Fire buttons, INPT4 (port 0) / INPT5 (port 1); true = pressed. */
    void setButton(int port, bool pressed);
    /**
     * Paddle pot for INPT0-3: position 0.0-1.0 is the resistance as a fraction
     * of 1 MOhm (Stella AnalogReadout: 0.0 trips almost at once after the dump
     * is released, 1.0 after ~379 lines). A negative position disconnects the
     * input (it then never charges and reads 0, as with no paddle plugged in).
     */
    void setPaddle(int idx, double position);

    /** ARGB colour of a TIA colour value (bit 0 ignored) in the current palette. */
    uint32_t paletteColor(uint8_t color) const;

    // ------------------------------------------------------------------
    // IOHandler
    // ------------------------------------------------------------------
    const char* name() const override { return m_name.c_str(); }
    void setName(const std::string& n) override { m_name = n; }
    uint32_t baseAddr() const override { return 0; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    /** Power-on state (Stella's TIA::initialize). */
    void reset() override;
    /** Does nothing: the machine clocks the TIA with clock(). */
    void tick(uint64_t) override {}

    // ------------------------------------------------------------------
    // IVideoOutput
    // ------------------------------------------------------------------
    VideoDimensions getDimensions() const override;
    void renderFrame(uint32_t* buffer) override;

    // ------------------------------------------------------------------
    // IAudioOutput
    // ------------------------------------------------------------------
    int nativeSampleRate() const override { return m_sampleRate; }
    int pullSamples(float* buffer, int maxSamples) override;
    /** Output rate for pullSamples() (default 44100). */
    void setSampleRate(int hz);

private:
    enum class Priority : uint8_t { PFP, SCORE, NORMAL };

    void cycleOne();
    void tickMovement();
    void tickHblank();
    void tickHframe();
    void renderPixel(uint32_t x);
    void nextLine();
    void applyRsync();
    void clearHmoveComb();
    void onFrameComplete();
    void delayedWrite(uint8_t address, uint8_t value);
    uint8_t resxCounter() const;
    uint8_t readRegister(uint8_t reg, bool sideEffects);
    uint8_t collisionBits(uint8_t reg) const;
    void pushSample(float v);
    int frameHeight() const { return (int)m_frameManager.height(); }

    std::string m_name = "TIA";
    Standard m_std;
    const uint32_t* m_palette;

    tiacore::DelayQueue m_delayQueue;
    tiacore::Background m_background;
    tiacore::Playfield m_playfield;
    tiacore::Missile m_missile0, m_missile1;
    tiacore::Player m_player0, m_player1;
    tiacore::Ball m_ball;
    tiacore::Audio m_audio;
    tiacore::FrameManager m_frameManager;
    std::array<tiacore::AnalogReadout, 4> m_analogReadouts;
    std::array<tiacore::AnalogReadout::Connection, 4> m_analogConnections{};
    tiacore::LatchedInput m_input0, m_input1;
    bool m_buttons[2] = {false, false};

    // Frame buffers of colour values, 160 per line (Stella's back / front buffers)
    std::vector<uint8_t> m_backBuffer;
    std::vector<uint8_t> m_frontBuffer;

    bool m_hblank = true;              ///< Stella's HState::blank
    uint8_t m_hctr = 0;
    int32_t m_hctrDelta = 0;
    uint16_t m_collisionMask = 0;
    uint32_t m_movementClock = 0;
    bool m_movementInProgress = false;
    bool m_extendedHblank = false;
    Priority m_priority = Priority::NORMAL;
    uint64_t m_timestamp = 0;          ///< colour clocks since reset
    std::array<uint8_t, 64> m_shadow{};
    bool m_halt = false;

    // Audio output ring buffer, resampled from the two samples per line
    static constexpr int AUDIO_BUF = 16384;
    std::vector<float> m_audioBuf;
    int m_audioRead = 0, m_audioWrite = 0, m_audioCount = 0;
    uint64_t m_audioAcc = 0;
    int m_sampleRate = DEFAULT_SAMPLE_RATE;
};
