#pragma once

// Apple II / II+ / IIe video: the beam-racing NTSC composite renderer.
//
// After AppleWin's NTSC renderer (source/NTSC.cpp/.h, NTSC_CharSet.cpp/.h and
// the video scanner of source/Video.cpp), by William S Simms, Michael
// Pohoreski, Tom Charlesworth and the AppleWin team (and, for Video.cpp,
// Michael O'Brien, Oliver Schmidt and Nick Westgate), GPL-2.0-or-later.
//
// Ported: AppleWin's default video type and style, "Color (TV)" with 50%
// scan lines (VT_COLOR_TV + VS_HALF_SCANLINES): the 4-phase x 4096-sequence
// chroma tables built from AppleWin's signal / chroma / luma filters, the
// colour-burst detector and its slow TV colour killer, 40-column text with
// inverse and flash (16 frames per half-period, as AppleWin), lo-res, hi-res
// with the half-dot delay of bit 7, mixed mode, page 2 and 80STORE, and for
// the IIe 80-column text, double lo-res, double hi-res (40- and 80-column),
// the alternate character set and MouseText. Lines are drawn as the beam
// passes them (AppleWin's NTSC_VideoUpdateCycles), so mode changes mid-frame
// and mid-line show where they happen. The floating bus and VBL sense follow
// NTSC_VideoGetScannerAddress / NTSC_GetVblBar.
//
// Left out: the other monitor types (idealized, RGB card, NTSC monitor,
// monochrome), VidHD / IIgs SHR, the debugger's pseudo pages, PAL 50 Hz
// (only the 262-line NTSC frame is kept; AppleWin's 312-line path would only
// change the line count, the scanner tables and the mixed-mode colour-burst
// kill), and the II J-Plus, Pravets, TK3000 and Base64A character sets.

#include "libdevices/main/ivideo_output.h"

#include <cstdint>
#include <vector>

/**
 * The Apple II video generator. The machine owns the RAM and the soft
 * switches; it hands this class pointers to main (and aux) RAM, tells it the
 * video mode with AppleWin's VF_* bits, and clocks it with clock() in CPU
 * cycles (65 per line, 262 lines = 17030 per frame).
 *
 * Timing follows AppleWin, which advances the video after each instruction:
 * a mode change made with setMode() lands at the current beam position, except
 * that TEXT and MIXED changes take effect one cycle later (AppleWin's GH#656
 * delay). floatingBus() reads the byte at the scanner address one cycle
 * behind the beam, as NTSC_VideoGetScannerAddress does.
 *
 * Frame: 560 x 384 pixels, 0xAARRGGBB — 280 hi-res dots doubled to 560
 * (14 pixels per CPU cycle, the 14 MHz dot clock), and each of the 192
 * visible scan lines followed by an in-between line at half the brightness of
 * the blend of the lines either side (AppleWin's "50% scan lines"). There is no
 * border; as in AppleWin's framebuffer each line starts two pixels left of the
 * frame (the composite filter's delay), the 14M modes (80-column text, double
 * lo-res / hi-res) one pixel further left (GH#555), and the in-between line
 * after line 191 is a quarter of its brightness. The frame is
 * double-buffered: renderFrame() returns the last frame completed, which is
 * published when the beam reaches line 192 (the start of vertical blanking).
 */
class Apple2Video : public IVideoOutput {
public:
    enum class Model { II, IIPlus, IIe, IIeEnhanced };

    /** AppleWin's video mode flags (Video.h VideoFlag_e), as passed to setMode(). */
    enum : uint32_t {
        VF_80COL = 0x00000001,
        VF_DHIRES = 0x00000002,
        VF_HIRES = 0x00000004,
        VF_80STORE = 0x00000008,
        VF_MIXED = 0x00000010,
        VF_PAGE2 = 0x00000020,
        VF_TEXT = 0x00000040,
    };

    static constexpr int FRAME_WIDTH = 560;          ///< 40 columns x 14 dots
    static constexpr int FRAME_HEIGHT = 384;         ///< 192 lines doubled
    static constexpr uint32_t CYCLES_PER_LINE = 65;
    static constexpr uint32_t LINES_PER_FRAME = 262;
    static constexpr uint32_t CYCLES_PER_FRAME = CYCLES_PER_LINE * LINES_PER_FRAME;   // 17030
    static constexpr uint32_t VISIBLE_LINES = 192;
    static constexpr uint32_t MIXED_LINE = 160;      ///< first text line in mixed mode
    static constexpr uint32_t HORZ_START = 25;       ///< first cycle of a line that fetches visible data

    explicit Apple2Video(Model m);
    ~Apple2Video() override = default;

    Model model() const { return m_model; }

    /**
     * The character generator ROM, as in AppleWin's resources: 2 KB for the
     * II / II+ (Apple2_Video.rom), 4 KB for the IIe (Apple2e_Video.rom /
     * Apple2e_Enhanced_Video.rom). For Model::IIe, the MouseText glyphs of the
     * alternate set ($40-$5F) are replaced by the primary set's, as AppleWin
     * does for the original IIe. A ROM of the wrong size leaves blank glyphs.
     */
    void setCharRom(std::vector<uint8_t> rom);
    /** Main and aux RAM, 64 KB each. aux may be null (II/II+, or a IIe without an aux card). */
    void setMemory(const uint8_t* mainRam, const uint8_t* auxRam);

    /**
     * The video soft switches as VF_* bits. On a II / II+ the IIe-only bits
     * (80COL, DHIRES, 80STORE) are ignored. With no aux RAM (setMemory() with
     * a null aux, which must come before setMode()), DHIRES is ignored
     * and 80COL only applies to text (AppleWin with an empty aux slot), and
     * 80-column text reads the floating bus for its aux bytes.
     */
    void setMode(uint32_t vfFlags);
    uint32_t mode() const { return m_newModeFlags; }
    /** IIe ALTCHARSET ($C00E off / $C00F on); ignored on a II / II+. Takes effect at once. */
    void setAltCharSet(bool on);

    /** Advance the beam by CPU cycles and draw the part of the screen it passes. */
    void clock(uint32_t cpuCycles);

    /** The byte the video is fetching from main RAM (AppleWin's floating bus). */
    uint8_t floatingBus() const;
    /** The scanner address floatingBus() reads (NTSC_VideoGetScannerAddress: one cycle behind the beam). */
    uint16_t scannerAddress() const;
    /** IIe $C019 bit 7 sense: true while the beam is on lines 0-191 (AppleWin NTSC_GetVblBar). */
    bool vblBar() const { return m_vert < VISIBLE_LINES; }

    /** Beam position: line 0-261 and cycle 0-64 in the line (25-64 fetch visible data). */
    uint16_t vert() const { return m_vert; }
    uint16_t horz() const { return m_horz; }
    /** Cycles since the start of the frame (line 0, cycle 0). */
    uint32_t cycleInFrame() const { return (uint32_t)m_vert * CYCLES_PER_LINE + m_horz; }
    /** Frames completed since reset (counted when the beam reaches line 192). */
    uint32_t frameCount() const { return m_frameCount; }
    /** Text flash phase (true = flashing characters shown inverted). */
    bool flashInverted() const { return m_flashMask != 0; }

    /** Beam to line 0 cycle 0, TEXT mode, primary character set, flash phase and frames cleared. */
    void reset();

    // IVideoOutput
    VideoDimensions getDimensions() const override;
    /** The last completed frame, FRAME_WIDTH x FRAME_HEIGHT pixels of 0xAARRGGBB. */
    void renderFrame(uint32_t* buffer) override;

private:
    /** AppleWin's g_pFuncUpdate*Screen functions. */
    enum class Screen : uint8_t {
        Text40, Text80, Lores40, DoubleLores40, DoubleLores80, Hires40, DoubleHires40, DoubleHires80
    };

    void applyMode(uint32_t flags);
    void updateCycles(int cycles);            // AppleWin VideoUpdateCycles
    void updateScreen(Screen s, long cycles);
    void updateGraphics(Screen s, long cycles);
    void updateText40(long cycles);
    void updateText80(long cycles);
    bool graphicsShowsText() const { return m_mixed && m_vert >= MIXED_LINE; }
    void colorBurstOn();

    void updateHorzEOL();
    void updateScannerAddress();
    void publishFrame();

    void updatePixels(uint16_t bits);
    void pixelHue(uint16_t signal);
    void pixelBnW(uint16_t signal);
    void writeTVPixel(uint16_t signal, const uint32_t* table);
    bool colorBurst() const { return m_colorBurstPixels >= 2; }

    uint16_t addrTXT(int v, int h) const;      // getVideoScannerAddressTXT at line v, cycle h
    uint16_t addrHGR(int v, int h) const;
    uint16_t addrTXTorHGR(int v, int h) const;
    uint8_t charSetBits(uint8_t ch) const { return m_csbits[m_charSet][ch][m_vert & 7]; }
    uint16_t loResBits(uint8_t b) const;
    uint8_t mainByte(uint16_t addr) const { return m_main ? m_main[addr] : 0; }
    uint8_t auxByte(uint16_t addr) const { return m_aux ? m_aux[addr] : 0; }

    bool isIIe() const { return m_model == Model::IIe || m_model == Model::IIeEnhanced; }

    Model m_model;
    const uint8_t* m_main = nullptr;
    const uint8_t* m_aux = nullptr;

    // Character sets: [0] primary, [1] alternate (IIe), 256 chars x 8 lines,
    // 7 dots per byte, leftmost in bit 0 (AppleWin's csbits)
    uint8_t m_csbits[2][256][8] = {};
    std::vector<uint8_t> m_charRom;

    // Video mode (AppleWin's g_* state in NTSC.cpp)
    uint32_t m_newModeFlags = VF_TEXT;        ///< g_uNewVideoModeFlags
    bool m_delayMode = false;                 ///< g_bDelayVideoMode
    bool m_altCharSet = false;
    int m_charSet = 0;
    bool m_mixed = false;
    int m_textPage = 1;
    int m_hiresPage = 1;
    Screen m_textScreen = Screen::Text40;
    Screen m_graphicsScreen = Screen::Text40;

    // Beam and composite signal state
    uint16_t m_vert = 0;                      ///< 0-261
    uint16_t m_horz = 0;                      ///< 0-64
    int m_colorBurstPixels = 0;
    int m_colorPhase = 0;
    int m_signalBits = 0;
    int m_lastColumnPixel = 0;
    uint8_t m_flashCounter = 0;
    uint16_t m_flashMask = 0;
    uint32_t m_frameCount = 0;

    // Back buffer: BUF_WIDTH x BUF_HEIGHT with margins round the 560 x 384
    // frame, for the pre-rendered and ringing pixels and the in-between line
    // above line 0; m_videoAddress indexes it (AppleWin's g_pVideoAddress)
    static constexpr int BUF_LEFT = 8;
    static constexpr int BUF_TOP = 2;
    static constexpr int BUF_WIDTH = FRAME_WIDTH + 2 * BUF_LEFT;
    static constexpr int BUF_HEIGHT = FRAME_HEIGHT + BUF_TOP + 2;
    std::vector<uint32_t> m_back;
    std::vector<uint32_t> m_front;
    int m_videoAddress = 0;
};
