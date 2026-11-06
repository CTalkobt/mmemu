#include "test_harness.h"
#include "plugins/devices/tia/main/tia.h"
#include <vector>

// TIA (#11, Atari 2600): WSYNC, playfield, players, NUSIZ, HMOVE,
// collisions, INPT4/5 latch, paddles and sound. Expected values follow
// Stella's TIA core and the Stella Programmer's Guide.

namespace {

enum : uint8_t {
    VSYNC = 0x00, VBLANK = 0x01, WSYNC = 0x02, NUSIZ0 = 0x04, COLUP0 = 0x06, COLUP1 = 0x07,
    COLUPF = 0x08, COLUBK = 0x09, CTRLPF = 0x0a, PF0 = 0x0d, PF1 = 0x0e, PF2 = 0x0f,
    RESP0 = 0x10, RESP1 = 0x11, AUDC0 = 0x15, AUDF0 = 0x17, AUDV0 = 0x19, GRP0 = 0x1b,
    GRP1 = 0x1c, HMP0 = 0x20, HMOVE = 0x2a, HMCLR = 0x2b, CXCLR = 0x2c,
    CXPPMM = 0x07, INPT0 = 0x08, INPT4 = 0x0c
};

constexpr uint32_t LINE = Tia::CLOCKS_PER_LINE;
constexpr int ROW = 100;   // a line well inside the visible frame

struct T {
    Tia t;
    explicit T(Tia::Standard s = Tia::Standard::NTSC) : t(s) {}

    void w(uint8_t reg, uint8_t v) { t.write(reg, v); }
    uint8_t r(uint8_t reg, uint8_t bus = 0) { return t.read(reg, bus); }

    /** Run to colour clock `h` of the next line that reaches it (h 0-227). */
    void toHpos(int h) {
        uint32_t n = (uint32_t)((h - t.hpos() + (int)LINE) % (int)LINE);
        t.clock(n);
    }
    /** Visible pixel x (0-159) is drawn at colour clock 68 + x. */
    void toPixel(int x) { toHpos(68 + x); }

    /** One 262-line NTSC frame: 3 lines of VSYNC, then 259 lines. Starts at a line start. */
    void frame() {
        toHpos(0);
        w(VSYNC, 0x02);
        t.clock(3 * LINE);
        w(VSYNC, 0x00);
        t.clock(259 * LINE);
    }
    /** Two frames, so the front buffer holds a complete frame drawn with the current setup. */
    std::vector<uint32_t> render() {
        frame();
        frame();
        auto d = t.getDimensions();
        std::vector<uint32_t> buf((size_t)d.width * d.height, 0);
        t.renderFrame(buf.data());
        return buf;
    }
    /** TIA pixel x (0-159) of the frame row; both halves of the doubled pixel must agree. */
    uint32_t px(const std::vector<uint32_t>& f, int x, int y = ROW) {
        uint32_t a = f[(size_t)y * Tia::FRAME_WIDTH + 2 * x];
        uint32_t b = f[(size_t)y * Tia::FRAME_WIDTH + 2 * x + 1];
        return a == b ? a : 0xDEADBEEF;
    }
    uint32_t col(uint8_t c) { return t.paletteColor(c); }
};

}  // namespace

TEST_CASE(tia_geometry_and_palette) {
    T n(Tia::Standard::NTSC);
    auto d = n.t.getDimensions();
    ASSERT_EQ(d.width, 320);
    ASSERT_EQ(d.height, 228);
    T p(Tia::Standard::PAL);
    ASSERT_EQ(p.t.getDimensions().height, 274);
    // Stella's NTSC palette: $00 black, $0E light grey, $1E yellow
    ASSERT_EQ(n.col(0x00), 0xFF000000u);
    ASSERT_EQ(n.col(0x0E), 0xFFECECECu);
    ASSERT_EQ(n.col(0x1E), 0xFFFCFC54u);
    ASSERT_EQ(n.col(0x1F), n.col(0x1E));   // bit 0 is not a colour bit
    ASSERT_EQ(p.col(0x00), 0xFF0B0B0Bu);
}

TEST_CASE(tia_wsync_halt) {
    T s;
    s.t.clock(10);
    ASSERT_EQ(s.t.hpos(), 10);
    ASSERT(!s.t.haltRequested());
    s.w(WSYNC, 0);
    ASSERT(s.t.haltRequested());
    ASSERT_EQ(s.t.clocksToLineEnd(), 218u);
    s.t.clock(217);
    ASSERT(s.t.haltRequested());
    s.t.clock(1);   // the line wraps: RDY is released
    ASSERT_EQ(s.t.hpos(), 0);
    ASSERT(!s.t.haltRequested());
    ASSERT_EQ(s.t.clocksToLineEnd(), 0u);
    // clearHalt() for the machine
    s.w(WSYNC, 0);
    s.t.clearHalt();
    ASSERT(!s.t.haltRequested());
}

TEST_CASE(tia_frame_counting) {
    T s;
    ASSERT_EQ(s.t.frameCount(), 0u);
    s.frame();
    ASSERT_EQ(s.t.frameCount(), 1u);
    s.frame();
    ASSERT_EQ(s.t.frameCount(), 2u);
    ASSERT_EQ(s.t.linesLastFrame(), 262);
    // A one-line VSYNC is not a VSYNC (FrameManager needs 2 lines)
    s.toHpos(0);
    s.w(VSYNC, 0x02);
    s.t.clock(LINE);
    s.w(VSYNC, 0x00);
    ASSERT_EQ(s.t.frameCount(), 2u);
}

TEST_CASE(tia_playfield_repeated_and_reflected) {
    T s;
    s.w(COLUBK, 0x00);
    s.w(COLUPF, 0x1E);
    s.w(PF0, 0xF0);   // PF0 bits 4-7: playfield pixels 0-15
    s.w(PF1, 0x00);
    s.w(PF2, 0x00);
    s.w(CTRLPF, 0x00);
    auto f = s.render();
    const uint32_t pf = s.col(0x1E), bk = s.col(0x00);
    ASSERT_EQ(s.px(f, 0), pf);
    ASSERT_EQ(s.px(f, 15), pf);
    ASSERT_EQ(s.px(f, 16), bk);
    ASSERT_EQ(s.px(f, 79), bk);
    // Repeated: the right half starts with PF0 again
    ASSERT_EQ(s.px(f, 80), pf);
    ASSERT_EQ(s.px(f, 95), pf);
    ASSERT_EQ(s.px(f, 96), bk);
    ASSERT_EQ(s.px(f, 159), bk);

    // Reflected: the right half is mirrored, PF0 at the right edge
    s.w(CTRLPF, 0x01);
    f = s.render();
    ASSERT_EQ(s.px(f, 0), pf);
    ASSERT_EQ(s.px(f, 15), pf);
    ASSERT_EQ(s.px(f, 80), bk);
    ASSERT_EQ(s.px(f, 143), bk);
    ASSERT_EQ(s.px(f, 144), pf);
    ASSERT_EQ(s.px(f, 159), pf);

    // PF1 is drawn MSB first (pixels 16-47), PF2 LSB first (48-79)
    s.w(CTRLPF, 0x00);
    s.w(PF0, 0x00);
    s.w(PF1, 0x80);
    s.w(PF2, 0x01);
    f = s.render();
    ASSERT_EQ(s.px(f, 15), bk);
    ASSERT_EQ(s.px(f, 16), pf);
    ASSERT_EQ(s.px(f, 19), pf);
    ASSERT_EQ(s.px(f, 20), bk);
    ASSERT_EQ(s.px(f, 47), bk);
    ASSERT_EQ(s.px(f, 48), pf);
    ASSERT_EQ(s.px(f, 51), pf);
    ASSERT_EQ(s.px(f, 52), bk);
}

TEST_CASE(tia_vblank_blacks_out) {
    T s;
    s.w(COLUBK, 0x0E);
    auto f = s.render();
    ASSERT_EQ(s.px(f, 10), s.col(0x0E));
    s.w(VBLANK, 0x02);
    f = s.render();
    ASSERT_EQ(s.px(f, 10), s.col(0x00));
}

TEST_CASE(tia_resp0_and_grp0) {
    T s;
    s.w(COLUBK, 0x00);
    s.w(COLUP0, 0x0E);
    s.w(GRP0, 0xF0);
    // RESP0 in the visible part of the line: the player starts 5 pixels later
    s.toPixel(40);
    s.w(RESP0, 0);
    auto f = s.render();
    const uint32_t p0 = s.col(0x0E), bk = s.col(0x00);
    ASSERT_EQ(s.px(f, 44), bk);
    ASSERT_EQ(s.px(f, 45), p0);   // GRP0 bit 7 first
    ASSERT_EQ(s.px(f, 48), p0);
    ASSERT_EQ(s.px(f, 49), bk);
    ASSERT_EQ(s.px(f, 52), bk);
    ASSERT_EQ(s.px(f, 53), bk);

    // RESP0 during HBLANK puts the player at pixel 3
    s.toHpos(20);
    s.w(RESP0, 0);
    s.w(GRP0, 0xFF);
    f = s.render();
    ASSERT_EQ(s.px(f, 2), bk);
    ASSERT_EQ(s.px(f, 3), p0);
    ASSERT_EQ(s.px(f, 10), p0);
    ASSERT_EQ(s.px(f, 11), bk);
}

TEST_CASE(tia_refp_reflects_player) {
    T s;
    s.w(COLUP0, 0x0E);
    s.w(GRP0, 0xF0);
    s.w(0x0b, 0x08);   // REFP0: bit 0 first
    s.toPixel(40);
    s.w(RESP0, 0);
    auto f = s.render();
    const uint32_t p0 = s.col(0x0E), bk = s.col(0x00);
    ASSERT_EQ(s.px(f, 45), bk);
    ASSERT_EQ(s.px(f, 48), bk);
    ASSERT_EQ(s.px(f, 49), p0);
    ASSERT_EQ(s.px(f, 52), p0);
}

TEST_CASE(tia_nusiz_copies) {
    T s;
    s.w(COLUP0, 0x0E);
    s.w(GRP0, 0xFF);
    s.toPixel(40);
    s.w(RESP0, 0);
    const uint32_t p0 = s.col(0x0E), bk = s.col(0x00);

    // NUSIZ 1: two copies, 16 pixels apart
    s.w(NUSIZ0, 0x01);
    auto f = s.render();
    ASSERT_EQ(s.px(f, 45), p0);
    ASSERT_EQ(s.px(f, 52), p0);
    ASSERT_EQ(s.px(f, 53), bk);
    ASSERT_EQ(s.px(f, 60), bk);
    ASSERT_EQ(s.px(f, 61), p0);
    ASSERT_EQ(s.px(f, 68), p0);
    ASSERT_EQ(s.px(f, 69), bk);

    // NUSIZ 3: three copies close (+16, +32)
    s.w(NUSIZ0, 0x03);
    f = s.render();
    ASSERT_EQ(s.px(f, 45), p0);
    ASSERT_EQ(s.px(f, 61), p0);
    ASSERT_EQ(s.px(f, 77), p0);
    ASSERT_EQ(s.px(f, 84), p0);
    ASSERT_EQ(s.px(f, 85), bk);

    // NUSIZ 6: three copies medium (+32, +64)
    s.w(NUSIZ0, 0x06);
    f = s.render();
    ASSERT_EQ(s.px(f, 45), p0);
    ASSERT_EQ(s.px(f, 61), bk);
    ASSERT_EQ(s.px(f, 77), p0);
    ASSERT_EQ(s.px(f, 109), p0);
    ASSERT_EQ(s.px(f, 117), bk);

    // NUSIZ 5: double width, 16 pixels, one pixel to the right
    s.w(NUSIZ0, 0x05);
    f = s.render();
    ASSERT_EQ(s.px(f, 45), bk);
    ASSERT_EQ(s.px(f, 46), p0);
    ASSERT_EQ(s.px(f, 61), p0);
    ASSERT_EQ(s.px(f, 62), bk);

    // NUSIZ 7: quad width, 32 pixels
    s.w(NUSIZ0, 0x07);
    f = s.render();
    ASSERT_EQ(s.px(f, 45), bk);
    ASSERT_EQ(s.px(f, 46), p0);
    ASSERT_EQ(s.px(f, 77), p0);
    ASSERT_EQ(s.px(f, 78), bk);
}

TEST_CASE(tia_hmove_moves_player) {
    T s;
    s.w(COLUP0, 0x0E);
    s.w(GRP0, 0x80);   // one pixel
    s.toPixel(40);
    s.w(RESP0, 0);
    auto f = s.render();
    ASSERT_EQ(s.px(f, 45), s.col(0x0E));

    // HMP0 = +7: seven pixels left, with one HMOVE at the start of a line
    s.w(HMP0, 0x70);
    s.toHpos(0);
    s.w(HMOVE, 0);
    s.t.clock(LINE);
    s.w(HMCLR, 0);
    f = s.render();
    ASSERT_EQ(s.px(f, 45), s.col(0x00));
    ASSERT_EQ(s.px(f, 38), s.col(0x0E));

    // HMP0 = -8 ($80): eight pixels right
    s.w(HMP0, 0x80);
    s.toHpos(0);
    s.w(HMOVE, 0);
    s.t.clock(LINE);
    s.w(HMCLR, 0);
    f = s.render();
    ASSERT_EQ(s.px(f, 38), s.col(0x00));
    ASSERT_EQ(s.px(f, 46), s.col(0x0E));

    // HMP0 = 0: HMOVE alone does not move it
    s.toHpos(0);
    s.w(HMOVE, 0);
    s.t.clock(LINE);
    f = s.render();
    ASSERT_EQ(s.px(f, 46), s.col(0x0E));
}

TEST_CASE(tia_hmove_comb) {
    T s;
    s.w(COLUBK, 0x0E);
    // HMOVE on every line blanks the first 8 pixels (the "comb")
    s.frame();
    for (int frames = 0; frames < 2; ++frames) {
        s.toHpos(0);
        s.w(VSYNC, 0x02);
        s.t.clock(3 * LINE);
        s.w(VSYNC, 0x00);
        for (int l = 0; l < 259; ++l) {
            s.w(HMOVE, 0);
            s.t.clock(LINE);
        }
    }
    std::vector<uint32_t> f((size_t)Tia::FRAME_WIDTH * Tia::NTSC_FRAME_HEIGHT);
    s.t.renderFrame(f.data());
    ASSERT_EQ(s.px(f, 0), s.col(0x00));
    ASSERT_EQ(s.px(f, 7), s.col(0x00));
    ASSERT_EQ(s.px(f, 8), s.col(0x0E));
}

TEST_CASE(tia_collision_p0_p1) {
    T s;
    s.w(GRP0, 0xFF);
    s.w(GRP1, 0xFF);
    s.toPixel(40);
    s.w(RESP0, 0);
    s.t.clock(3);      // one CPU cycle later
    s.w(RESP1, 0);
    s.w(CXCLR, 0);
    s.t.clock(2 * LINE);
    ASSERT_EQ(s.r(CXPPMM) & 0x80, 0x80);
    ASSERT_EQ(s.t.peek(CXPPMM) & 0x80, 0x80);
    ASSERT_EQ(s.r(CXPPMM) & 0x40, 0);   // no missiles
    // Bits 0-5 come from the data bus
    ASSERT_EQ(s.r(CXPPMM, 0x2A), 0x80 | 0x2A);
    s.w(CXCLR, 0);
    ASSERT_EQ(s.r(CXPPMM), 0);

    // Far apart: no collision
    s.toPixel(100);
    s.w(RESP1, 0);
    s.t.clock(LINE);
    s.w(CXCLR, 0);
    s.t.clock(2 * LINE);
    ASSERT_EQ(s.r(CXPPMM) & 0x80, 0);

    // Collisions do not latch during VBLANK
    s.toPixel(40);
    s.w(RESP1, 0);
    s.w(VBLANK, 0x02);
    s.t.clock(LINE);
    s.w(CXCLR, 0);
    s.t.clock(2 * LINE);
    ASSERT_EQ(s.r(CXPPMM) & 0x80, 0);
    s.w(VBLANK, 0x00);
    s.t.clock(2 * LINE);
    ASSERT_EQ(s.r(CXPPMM) & 0x80, 0x80);
}

TEST_CASE(tia_collision_p0_playfield) {
    T s;
    s.w(GRP0, 0xFF);
    s.w(PF0, 0xF0);    // pixels 0-15 and 80-95
    s.toPixel(100);
    s.w(RESP0, 0);     // player at 105-112: clear of the playfield
    s.t.clock(LINE);
    s.w(CXCLR, 0);
    s.t.clock(2 * LINE);
    ASSERT_EQ(s.r(0x02) & 0x80, 0);    // CXP0FB
    s.toPixel(80);
    s.w(RESP0, 0);     // player at 85-92: over the playfield
    s.t.clock(2 * LINE);
    ASSERT_EQ(s.r(0x02) & 0x80, 0x80);
}

TEST_CASE(tia_inpt4_latched) {
    T s;
    // Unlatched: bit 7 follows the button (0 = pressed)
    ASSERT_EQ(s.r(INPT4), 0x80);
    ASSERT_EQ(s.r(INPT4, 0x3F), 0xBF);
    s.t.setButton(0, true);
    ASSERT_EQ(s.r(INPT4), 0x00);
    s.t.setButton(0, false);
    ASSERT_EQ(s.r(INPT4), 0x80);
    ASSERT_EQ(s.r(0x0d), 0x80);        // INPT5

    // VBLANK bit 6: a press is held until the latch is released. As in
    // Stella, the latch is only reset (to "not pressed") by a VBLANK write
    // with bit 6 clear; from power-on it holds 0.
    s.w(VBLANK, 0x00);
    s.w(VBLANK, 0x40);
    ASSERT_EQ(s.r(INPT4), 0x80);
    s.t.setButton(0, true);
    ASSERT_EQ(s.t.peek(INPT4), 0x00);
    s.t.setButton(0, false);
    ASSERT_EQ(s.t.peek(INPT4), 0x80);  // peek did not latch the press
    s.t.setButton(0, true);
    ASSERT_EQ(s.r(INPT4), 0x00);
    s.t.setButton(0, false);
    ASSERT_EQ(s.r(INPT4), 0x00);       // latched
    s.w(VBLANK, 0x40);
    ASSERT_EQ(s.r(INPT4), 0x00);       // still latched
    s.w(VBLANK, 0x00);
    ASSERT_EQ(s.r(INPT4), 0x80);       // released
}

TEST_CASE(tia_paddle_dump_and_charge) {
    T s;
    s.t.setPaddle(0, 1.0);             // maximum resistance: ~379 lines
    s.t.setPaddle(1, 0.0);             // minimum
    s.w(VBLANK, 0x80);                 // dump the capacitors
    s.t.clock(10 * LINE);
    ASSERT_EQ(s.r(INPT0), 0x00);
    ASSERT_EQ(s.r(INPT0 + 1), 0x00);
    s.w(VBLANK, 0x00);
    s.t.clock(LINE);
    ASSERT_EQ(s.r(INPT0 + 1), 0x80);
    s.t.clock(368 * LINE);
    ASSERT_EQ(s.r(INPT0), 0x00);
    s.t.clock(20 * LINE);
    ASSERT_EQ(s.r(INPT0), 0x80);
    // Unconnected inputs never charge
    ASSERT_EQ(s.r(INPT0 + 2), 0x00);
}

TEST_CASE(tia_audio_tone) {
    T s;
    float buf[8192];
    const uint64_t hz = Tia::clockHz(Tia::Standard::NTSC);
    // Silence while AUDV0 = 0
    const uint32_t c1 = (uint32_t)(hz / 20);
    s.t.clock(c1);
    int n = s.t.pullSamples(buf, 8192);
    const int n1 = (int)((uint64_t)c1 * 44100 / hz);
    ASSERT_EQ(n, n1);
    for (int i = 0; i < n; ++i) ASSERT_EQ(buf[i], 0.0f);

    // AUDC0 = 4 (pure tone: the output toggles every channel clock), AUDF0 = 31
    // (a channel clock every 32 audio clocks), AUDV0 = 15
    s.w(AUDC0, 0x04);
    s.w(AUDF0, 0x1F);
    s.w(AUDV0, 0x0F);
    const uint32_t c2 = (uint32_t)(hz / 10);
    s.t.clock(c2);
    n = s.t.pullSamples(buf, 8192);
    ASSERT_EQ(n, (int)((uint64_t)(c1 + c2) * 44100 / hz) - n1);
    float lo = 1.0f, hi = 0.0f;
    int transitions = 0;
    for (int i = 0; i < n; ++i) {
        if (buf[i] < lo) lo = buf[i];
        if (buf[i] > hi) hi = buf[i];
        if (i > 0 && (buf[i] > 0.25f) != (buf[i - 1] > 0.25f)) ++transitions;
    }
    ASSERT_EQ(lo, 0.0f);
    ASSERT(hi > 0.6f && hi < 0.7f);   // Stella's mixing table: volume 15 of 30 = 21844 / 32768
    // Two audio clocks per line (31.47 kHz) / 32 / 2 = ~492 Hz: ~98 edges in 0.1 s
    ASSERT(transitions > 90 && transitions < 106);
}
