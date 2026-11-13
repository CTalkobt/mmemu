#pragma once

#include <cstdint>
#include <functional>
#include <vector>

namespace mc6847_font {
extern const uint8_t STANDARD[768];   ///< MC6847: 64 characters x 12 rows
extern const uint8_t T1[1536];        ///< MC6847T1: 128 characters x 12 rows
}

/**
 * Motorola MC6847 Video Display Generator (and the MC6847T1, the CoCo 2B's,
 * with lower case), after the data sheet; the timing and the per-byte
 * behaviour as XRoar models them (mc6847.c).
 *
 * Time is counted in ticks of 14.31818 MHz, a quarter of the VDG's 3.58 MHz
 * clock: one tick is half a pixel of the 256-dot modes. A line is 912 ticks
 * (HS low for the first 64), a frame 262 lines (NTSC): 13 of vertical blank,
 * 25 of top border, 192 active, 26 of bottom border, 6 of retrace. FS falls
 * at the end of the active area (line 230) and rises at line 0.
 *
 * Modes (setMode, the A/G, GM2-0 and CSS pins): alphanumerics with the
 * internal font (inverse per character), semigraphics 4 (A/S, internal) and
 * 6 (A/S with INT/EXT), and the eight graphics modes CG1-CG6 / RG1-RG6.
 * The per-character pins INV, A/S and INT/EXT come with each byte fetched:
 * the machine wires them (the CoCo: D6, D7, GM0). Mode changes take effect
 * at once, mid-line (two bits of data at a time), as on the chip.
 *
 * Video data is fetched through `fetch` as the beam reaches it: 32 bytes a
 * line (42 clocked by line end) in the 32-byte modes, 16 (22) in the
 * others. On the CoCo the SAM counts them; the VDG's own address (passed
 * to `fetch`, used by the MC-10) advances 32 or 16 per row of nLPR lines.
 *
 * Output is a 320x240 frame: the 256x192 active area with 32 dots of
 * border each side and 24 lines above and below. Artifact colours (NTSC
 * cross-colour, as PMODE 4 games use): in two-colour graphics with black
 * and buff, each aligned pair of dots of the active area shows black,
 * white, or one of two colours by which dot is lit (XRoar's 2-bit LUT);
 * the phase picks which pair is red and which blue.
 *
 * PAL (setPal): the 6847 only knows NTSC's 262 lines; a PAL board (the
 * Dragon, the PAL CoCo) stops its clock twice a frame and makes blank
 * lines of its own, so the frame is 312 lines (50 Hz). Each stop is at the
 * fall of HS that starts a line the board counts from FS rising: the VDG's
 * line N waits there for `delay` lines of 912 ticks, each with an HS pulse
 * (unless `hsInhibit`, as the Dragon 64 and Professional), then resumes.
 * The padding lines are black and outside the output frame (XRoar's
 * vdg_pal: the Dragon pads 25 at line 0 and 25 at 254, the CoCo 24 at 16
 * and 26 at 256).
 */
class Mc6847 {
public:
    /** The colours the VDG makes (palette indices). */
    enum Colour : uint8_t {
        GREEN, YELLOW, BLUE, RED, BUFF, CYAN, MAGENTA, ORANGE,
        BLACK, DARK_GREEN, DARK_ORANGE, BRIGHT_ORANGE,
        ARTIFACT_BLUE, ARTIFACT_RED, NUM_COLOURS
    };
    /** Artifact colours: off, or the two NTSC phases. */
    enum class Artifact { Off, BlueRed, RedBlue };
    /** A PAL board's padding: two stops (lines from FS rising) and the blank lines at each. */
    struct PalPadding {
        int stop0 = 0, delay0 = 0;
        int stop1 = 0, delay1 = 0;
        bool hsInhibit = false;      ///< no HS pulses during the padding
    };

    // Horizontal timing (ticks from the fall of HS)
    static constexpr int LINE_TICKS = 912;
    static constexpr int HS_RISE = 64;
    static constexpr int LEFT_BORDER = 134;
    static constexpr int ACTIVE_START = 254;
    static constexpr int RIGHT_BORDER = 766;
    static constexpr int RIGHT_BORDER_END = 878;
    static constexpr int HBLANK = 168;          ///< first fetch reference (front porch + HS + back porch)
    // Vertical timing (lines)
    static constexpr int FRAME_LINES = 262;
    static constexpr int ACTIVE_FIRST = 38;
    static constexpr int ACTIVE_END = 230;
    // Output
    static constexpr int OUT_W = 320, OUT_H = 240;
    static constexpr int OUT_LEFT = ACTIVE_START - 64;     ///< tick of the first output dot
    static constexpr int OUT_TOP = ACTIVE_FIRST - 24;      ///< line of the first output row

    // Bits of a fetched word: the byte and the per-character pins
    static constexpr uint16_t F_INV = 0x100, F_AS = 0x200, F_EXT = 0x400;

    explicit Mc6847(bool t1 = false);

    /** HS (true = high); falls at the start of each line. */
    std::function<void(bool)> onHs;
    /** FS (true = high); falls at the end of the active area. */
    std::function<void(bool)> onFs;
    /** Fetch `n` bytes of video data (with F_* pins) from the VDG's address `addr`. */
    std::function<void(uint16_t addr, int n, uint16_t* out)> fetch;
    /** A frame is complete (FS rose). */
    std::function<void()> onFrame;

    /** /RESET: line 0, alphanumerics. */
    void reset();
    /** Run the beam `ticks` (14.31818 MHz) forward. */
    void advance(uint64_t ticks);
    /** Draw (and fetch) the current line up to now, before something the
     *  fetch depends on changes (the SAM's video registers). */
    void update() { if (m_line >= ACTIVE_FIRST && m_line < ACTIVE_END) render(m_tick); }
    /** The mode pins: bit 7 A/G, 6-4 GM2-GM0, 3 CSS. */
    void setMode(uint8_t mode);
    uint8_t mode() const { return m_modePins; }
    /** Show text inverted (XRoar's "invert text" option, for green-on-black). */
    void setInvertedText(bool on) { m_invertText = on; }
    void setArtifact(Artifact a) { m_artifact = a; }
    /** PAL: the board's padding lines (delays of 0: NTSC, the chip alone). */
    void setPal(const PalPadding& p) { m_pal = p; m_padLeft = 0; m_padTick = 0; }
    bool isPal() const { return m_pal.delay0 + m_pal.delay1 > 0; }
    /** Lines a frame: 262, or 312 with PAL padding. */
    int frameLines() const { return FRAME_LINES + m_pal.delay0 + m_pal.delay1; }
    /** Padding lines still to run before the VDG resumes (0: running). */
    int paddingLeft() const { return m_padLeft; }
    Artifact artifact() const { return m_artifact; }
    bool isT1() const { return m_t1; }

    /** 0xAARRGGBB of a colour index. */
    static uint32_t rgb(uint8_t colour);
    /** The last complete frame, OUT_W x OUT_H, 0xAARRGGBB. */
    const uint32_t* frame() const { return m_front.data(); }
    /** The colour index of a dot of the last complete frame's active area (0-255, 0-191). */
    uint8_t activeDot(int x, int y) const;

    int scanline() const { return m_line; }
    int lineTick() const { return m_tick; }
    bool hs() const { return m_hs; }
    bool fs() const { return m_fs; }
    unsigned row() const { return m_row; }
    uint64_t frames() const { return m_frames; }

    /** State for snapshots. */
    std::vector<uint8_t> saveState() const;
    void loadState(const std::vector<uint8_t>& s);

private:
    enum RenderMode : uint8_t { R_SG, R_CG, R_RG };

    void endLine();
    void endPadLine();
    void render(int to);
    void byteBoundary();
    void commitLine();
    uint8_t borderColour() const;
    bool is32() const { return !m_ag || !(m_gm == 0 || ((m_gm & 1) && m_gm != 7)); }

    bool m_t1;
    Artifact m_artifact = Artifact::Off;
    bool m_invertText = false;

    // Mode pins
    uint8_t m_modePins = 0;
    unsigned m_gm = 0;
    bool m_ag = false, m_css = false;

    // Beam
    int m_line = 0;              // 0..261
    int m_tick = 0;              // within the line, 0..911
    bool m_hs = true, m_fs = true;
    unsigned m_row = 0;          // text row counter (0-11)
    unsigned m_nlpr = 12;
    uint16_t m_addr = 0;         // the VDG's own address counter
    uint64_t m_frames = 0;

    // PAL padding
    PalPadding m_pal;
    int m_padLeft = 0;           // padding lines left (the VDG is stopped)
    int m_padTick = 0;           // within the padding line

    // The line being drawn
    uint16_t m_vram[42] = {};
    int m_fetched = 0;           // bytes fetched this line
    int m_index = 0;             // next byte to show
    int m_bytesLeft = 32;
    int m_bit = 0;               // bits left of the current byte
    int m_pos = LEFT_BORDER;     // tick drawn up to
    uint8_t m_g = 0, m_sg = 0;   // the byte as graphics / semigraphics dots
    bool m_as = false, m_ext = false;
    uint8_t m_fg = GREEN, m_bg = DARK_GREEN, m_sfg = GREEN, m_sbg = BLACK;
    uint8_t m_cgBase = GREEN;
    RenderMode m_rmode = R_RG;
    bool m_cssLatched = false;
    uint8_t m_border = BLACK;
    bool m_artifactLine = false;
    uint8_t m_dots[LINE_TICKS + 16] = {};

    std::vector<uint32_t> m_back, m_front;
    std::vector<uint8_t> m_backIdx, m_frontIdx;   // active-area colour indices (256x192)
};
