#pragma once

// MOS 7360/8360 TED (Text Editing Device), the Commodore Plus/4, C16 and
// C116 video, sound, timer and keyboard-latch chip.
//
// Ported from VICE, the Versatile Commodore Emulator (src/plus4/ted.c,
// ted-mem.c, ted-draw.c, ted-fetch.c, ted-badline.c, ted-irq.c,
// ted-timer.c, ted-sound.c, ted-color.c, ted-timing.c; src/video/
// video-color.c for the palette). TED files by Andreas Boose, Ettore
// Perazzoli, Tibor Biczo and Marco van den Heuvel; palette by John Selck,
// Philip Timmermann and Groepaz; VICE's sound logic follows SDL-YAPE
// (github.com/calmopyrin/yapesdl). GPL-2.0-or-later. VICE's raster_t
// framework is not ported: the behaviour it implements is, a line at a time.

#include "libdevices/main/io_handler.h"
#include "libdevices/main/ivideo_output.h"
#include "libdevices/main/iaudio_output.h"
#include "libdevices/main/isignal_line.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

/**
 * TED at $FF00-$FF3F.
 *
 *  - $FF00-$FF05: three 16-bit timers counting at the single clock. Timer 1
 *    reloads from its latch, timers 2 and 3 run on from $FFFF. Writing a low
 *    byte stops a timer, writing its high byte (re)starts it.
 *  - $FF06/$FF07: scroll, 24/25 rows, 38/40 columns, display enable, ECM,
 *    bitmap, multicolour, 256-character (reverse off) mode.
 *  - $FF08: keyboard latch (with the joysticks), read through KeyboardFn.
 *  - $FF09/$FF0A/$FF0B: IRQ flags (raster bit 1, timers bits 3/4/6, summary
 *    bit 7), enable mask and the 9-bit raster compare.
 *  - $FF0C/$FF0D: hardware cursor position. $FF0E-$FF12: sound.
 *  - $FF12-$FF14: bitmap, character and screen/attribute bases, ROM/RAM
 *    select for character and bitmap fetches, single-clock force.
 *  - $FF15-$FF19: background colours 0-3 and the border.
 *  - $FF1A-$FF1F: character position, raster line, horizontal position,
 *    flash counter and character row counter.
 *  - $FF3E/$FF3F: ROM / RAM in the upper 32 KB (through RomSelectFn).
 *
 * $FF20-$FF3D are not TED registers: as in VICE, reads and writes there are
 * not handled (return false) so they reach the RAM/ROM underneath, and so
 * are reads of $FF3E/$FF3F while ROM is selected; with RAM selected those
 * two read $FF.
 */
class Ted : public IOHandler, public IVideoOutput, public IAudioOutput {
public:
    enum class Standard { PAL, NTSC };

    static constexpr int CYCLES_PER_LINE = 114;      ///< VICE plus4.h, PAL and NTSC
    static constexpr int PAL_LINES  = 312;
    static constexpr int NTSC_LINES = 262;
    static constexpr int LEFT_BORDER  = 32;          ///< "normal" borders, as VICE shows them
    static constexpr int RIGHT_BORDER = 32;
    static constexpr int FRAME_WIDTH  = 320 + LEFT_BORDER + RIGHT_BORDER;   // 384
    static constexpr int PAL_FRAME_HEIGHT  = 306 - 19 + 1;                  // 288
    static constexpr int NTSC_FRAME_HEIGHT = 260 - 19 + 1;                  // 242
    static constexpr int FETCH_CYCLE = 4;            ///< TED_FETCH_CYCLE
    static constexpr int DMA_STEAL = (40 + 3) * 2;   ///< double-clock cycles a fetch line takes
    static constexpr int DEFAULT_SAMPLE_RATE = 44100;

    /** TED double clock in Hz: tick() counts these. */
    static constexpr uint32_t clockHz(Standard s) {
        return s == Standard::PAL ? 1773447u : 1789772u;
    }

    explicit Ted(Standard s = Standard::PAL);
    ~Ted() override = default;

    /** PAL (312 lines, 288-line frame) or NTSC (262 lines, 242-line frame); also picks the palette. */
    void setStandard(Standard s);
    Standard standard() const { return m_std; }

    // ------------------------------------------------------------------
    // Machine hooks
    // ------------------------------------------------------------------

    /**
     * Video fetches: the machine returns the byte TED sees at addr. fromRom
     * is true when TED fetches characters/bitmap from ROM (register $FF12
     * bit 2 set) — the machine then reads its current ROM banking for that
     * address. Screen and attribute fetches pass fromRom = true only for
     * addresses at $8000 and up while ROM is selected ($FF3E), as VICE
     * does; a character/bitmap fetch from ROM below $8000 sees open space
     * (zeros in VICE) and does not call fetch.
     */
    using FetchFn = std::function<uint8_t(uint16_t addr, bool fromRom)>;
    void setFetch(FetchFn fn) { m_fetch = std::move(fn); }

    /**
     * $FF08: the keyboard column bits (0 = pressed) AND the selected
     * joystick, given the value written to $FF08 (its bit 2 selects joystick
     * 1, bit 1 joystick 2, active low). The machine owns the keyboard row
     * latch ($FD30) and the joysticks. As in VICE (and the chip) the result
     * is latched when $FF08 is written; reads return the latch.
     */
    using KeyboardFn = std::function<uint8_t(uint8_t ff08Written)>;
    void setKeyboardRead(KeyboardFn fn) { m_keyboard = std::move(fn); }

    /** Writes to $FF3E (ROM on) / $FF3F (RAM on): called with true for $FF3E. */
    using RomSelectFn = std::function<void(bool romOn)>;
    void setRomSelect(RomSelectFn fn) { m_romSelect = std::move(fn); }
    /** ROM in the upper 32 KB ($FF13 bit 0). Reset: ROM on. */
    bool romSelected() const { return (m_regs[0x13] & 0x01) != 0; }

    /** set(true) while TED asserts IRQ (wired to the CPU's IRQ). */
    void setIrqLine(ISignalLine* line) override;

    // ------------------------------------------------------------------
    // Clocking
    // ------------------------------------------------------------------

    /**
     * True when the CPU runs at the single clock this cycle, so the machine
     * charges 2 ticks per CPU cycle: $FF13 bit 1 set, or (as VICE's
     * ted_delay_clk) cycles 4-100 of lines 0-$CB while the display is on
     * (character fetches), or cycles 91-100 of any line (RAM refresh).
     */
    bool cpuSingleClock() const;
    /**
     * True while TED holds the bus for a DMA fetch: (40 + 3) * 2 double
     * clocks from cycle 4 of each character-pointer (bad) line and each
     * attribute line. The scheduler then calls consumeBlockedCycle() and
     * tick(1) without running the CPU, as for ANTIC.
     */
    bool cpuBlocked() const { return m_stealPending > 0; }
    /** The scheduler spent one double-clock cycle without the CPU. */
    void consumeBlockedCycle() { if (m_stealPending > 0) --m_stealPending; }
    /** Double-clock cycles elapsed since reset (for real-time pacing). */
    uint64_t totalCycles() const { return m_totalCycles; }

    // ------------------------------------------------------------------
    // IOHandler
    // ------------------------------------------------------------------
    const char* name()     const override { return m_name.c_str(); }
    void        setName(const std::string& n) override { m_name = n; }
    uint32_t    baseAddr() const override { return 0xFF00; }
    uint32_t    addrMask() const override { return 0x3F; }
    bool ioRead (IBus* bus, uint32_t addr, uint8_t* val) override;
    bool ioPeek (IBus* bus, uint32_t addr, uint8_t* val) override;
    bool ioWrite(IBus* bus, uint32_t addr, uint8_t  val) override;
    /** VICE's ted_reset() + ted_reset_registers(): $00-$3D written with 0, then $3E (ROM on). */
    void reset() override;
    /** Advance by `cycles` TED double-clock cycles. */
    void tick(uint64_t cycles) override;
    void getDeviceInfo(DeviceInfo& out) const override;
    std::vector<std::pair<std::string, uint32_t>> getDerivedValues() const override;
    std::vector<std::string> deviceAliases() const override { return {"TED"}; }

    // ------------------------------------------------------------------
    // IVideoOutput: the last complete frame, 384 x 288 (PAL) / 384 x 242 (NTSC)
    // ------------------------------------------------------------------
    VideoDimensions getDimensions() const override {
        return {FRAME_WIDTH, frameHeight(), FRAME_WIDTH, frameHeight()};
    }
    void renderFrame(uint32_t* buffer) override;
    int frameHeight() const { return m_std == Standard::PAL ? PAL_FRAME_HEIGHT : NTSC_FRAME_HEIGHT; }

    // ------------------------------------------------------------------
    // IAudioOutput: mono samples at nativeSampleRate() (44100 Hz default)
    // ------------------------------------------------------------------
    int  nativeSampleRate() const override { return m_sampleRate; }
    int  pullSamples(float* buffer, int maxSamples) override;
    /** Host sample rate the sound is generated at (VICE's "speed"). */
    void setSampleRate(int hz);
    int  samplesAvailable() const { return m_audioCount; }

    // ------------------------------------------------------------------
    // State access (tests, debugger)
    // ------------------------------------------------------------------
    /** Palette entry for a TED colour byte (luminance bits 6-4, hue 3-0), 0xFFRRGGBB. */
    uint32_t paletteColor(uint8_t colorByte) const { return m_palette[colorByte & 0x7F]; }
    uint16_t rasterLine() const { return m_rasterCounter; }
    int      rasterCycle() const { return m_cycle; }
    uint16_t rasterCompare() const { return m_rasterIrqLine; }
    uint8_t  irqStatus() const { return (uint8_t)m_irqStatus; }
    bool     irqAsserted() const { return m_irqOut; }
    bool     timerRunning(int t) const { return m_timerRunning[t % 3]; }
    uint16_t timerValue(int t) const { return (uint16_t)((m_tval[t % 3] >> 1) & 0xFFFF); }
    uint16_t timer1Latch() const { return m_t1Start; }
    bool     fastMode() const { return m_fastMode; }
    int      videoMode() const { return ((m_regs[0x06] & 0x60) | (m_regs[0x07] & 0x10)) >> 4; }
    uint16_t screenBase() const { return (uint16_t)(((m_regs[0x14] & 0xF8) << 8) | 0x400); }
    uint16_t attributeBase() const { return (uint16_t)((m_regs[0x14] & 0xF8) << 8); }
    uint16_t charBase() const;
    uint16_t bitmapBase() const { return (uint16_t)((m_regs[0x12] & 0x38) << 10); }
    uint16_t cursorPos() const { return (uint16_t)m_crsrpos; }
    bool     cursorVisible() const { return m_cursorVisible; }
    uint64_t frameCount() const { return m_frameCount; }
    uint16_t voiceFreq(int v) const;      ///< 10-bit frequency register of voice 0/1
    uint8_t  soundControl() const { return m_snd.data[3]; }

private:
    // registers (ted-mem.c)
    uint8_t readReg(uint8_t reg, bool sideEffects);
    void    store(uint8_t reg, uint8_t value);
    void    store06(uint8_t value);
    void    store07(uint8_t value);
    void    updateVideoMode();
    void    irqCheckState(uint8_t value, bool high);
    void    setIrqLineState();
    void    storeRaster(uint8_t reg, uint8_t value);
    void    store1F(uint8_t value);

    // timers (ted-timer.c)
    void    timerStoreLow(int t, uint8_t value);
    void    timerStoreHigh(int t, uint8_t value);
    uint8_t timerRead(int reg) const;
    void    timersAdvance(int64_t n);

    // raster (ted.c, ted-fetch.c)
    void    advance(int n);
    void    doMatrixFetch();
    void    endOfLine();
    void    lineStart();

    // drawing (ted-draw.c)
    uint8_t fetchVideo(uint16_t addr, bool fromRom) const;
    uint8_t fetchChar(uint16_t addr) const;
    void    drawLine(int row);
    void    drawGraphics(uint8_t* gfx);
    void    buildPalette();

    // sound (ted-sound.c)
    void    soundInit();
    void    soundStore(uint8_t reg, uint8_t val);
    uint8_t soundRead(uint8_t reg) const;
    void    soundClockShift();
    void    renderSample();
    void    pushSample(float s);

    std::string  m_name = "TED";
    Standard     m_std = Standard::PAL;
    ISignalLine* m_irqLine = nullptr;
    bool         m_irqOut = false;
    FetchFn      m_fetch;
    KeyboardFn   m_keyboard;
    RomSelectFn  m_romSelect;

    // geometry (ted-timing.c, "normal" borders)
    int m_screenHeight = PAL_LINES;
    int m_vsyncLine = 257;
    int m_firstDisplayedLine = 19;
    int m_lastDisplayedLine = 306;
    static constexpr int ROW25_START = 4, ROW25_STOP = 0xCB;
    static constexpr int ROW24_START = 8, ROW24_STOP = 0xC7;
    static constexpr int FIRST_DMA_LINE = 0, LAST_DMA_LINE = 0xCB;

    uint8_t  m_regs[64] = {};

    // timers
    uint16_t m_t1Start = 0, m_t2Start = 0, m_t3Start = 0;
    int64_t  m_tval[3] = {0, 0, 0};      // double-clock cycles left (VICE's tN_value)
    bool     m_timerRunning[3] = {false, false, false};

    // interrupts
    int      m_irqStatus = 0;
    uint16_t m_rasterIrqLine = 0;

    // raster position
    uint16_t m_rasterCounter = 0;        // ted_raster_counter
    int      m_cycle = 0;                // cycle in the line, 0-113
    int      m_tvLine = 0;               // tv_current_line
    uint64_t m_totalCycles = 0;
    uint64_t m_frameCount = 0;
    int      m_stealPending = 0;

    // video state (ted_t)
    uint8_t  m_vbuf[40] = {}, m_cbuf[40] = {}, m_cbufTmp[40] = {};
    bool     m_allowBadLines = false;
    bool     m_reverseMode = false;
    bool     m_idleState = false;
    bool     m_drawIdleState = false;    // raster.draw_idle_state
    bool     m_forceDisplayState = false;
    bool     m_ycounterResetChecked = false;
    int      m_memoryFetchDone = 0;
    int      m_memptr = 0, m_memptrCol = 0, m_memCounter = 0, m_chrPosCount = 0;
    int      m_memCounterInc = 0;
    bool     m_badLine = false;
    bool     m_cursorVisible = false;
    int      m_cursorPhase = 0;
    int      m_crsrpos = 0;
    int      m_ysmooth = 0;
    int      m_ycounter = 0;
    bool     m_blank = true;             // raster.blank: display enable ($FF06 bit 4) off
    bool     m_blankEnabled = true;      // vertical border flip-flop
    bool     m_blankThisLine = false;
    bool     m_characterFetchOn = false;
    bool     m_fastMode = true;
    uint8_t  m_idleBgColor = 0;          // raster.idle_background_color (overscan behind scroll)
    bool     m_forceBlackOverscan = false;
    uint8_t  m_idleData = 0;
    uint8_t  m_kbdval = 0;
    int      m_lastVideoMode = -1;
    int      m_xsmooth = 0, m_xsmoothNext = -1;
    int      m_displayXStart = LEFT_BORDER, m_displayXStop = LEFT_BORDER + 320;
    int      m_displayXStartNext = -1, m_displayXStopNext = -1;

    // frame buffers (0xFFRRGGBB)
    std::vector<uint32_t> m_back, m_front;
    uint32_t m_palette[128] = {};

    // sound (ted-sound.c, plus4_sound_s)
    struct Sound {
        uint8_t  data[5] = {};           // plus4_sound_data: $FF0E, $FF0F, $FF10, $FF11, $FF12
        uint32_t voice0Accu = 0, voice0Reload = 0;
        uint8_t  voice0Sign = 0, voice0OutputEnabled = 0;
        uint32_t voice1Accu = 0, voice1Reload = 0;
        uint8_t  voice1Sign = 0, voice1OutputEnabled = 0;
        uint8_t  voice0CachedOutput = 0, voice1CachedOutput = 0;
        uint16_t digitalCachedOutput = 0;
        uint32_t oscStep = 0;
        uint8_t  volume = 0;
        uint8_t  digital = 0, noise = 0;
        uint8_t  noiseShiftRegister = 0xFF;
    } m_snd;
    int      m_sampleRate = DEFAULT_SAMPLE_RATE;
    uint64_t m_audioAcc = 0;             // cycles * sampleRate
    static constexpr int AUDIO_BUF = 8192;
    float    m_audioBuf[AUDIO_BUF] = {};
    int      m_audioRead = 0, m_audioWrite = 0, m_audioCount = 0;
};
