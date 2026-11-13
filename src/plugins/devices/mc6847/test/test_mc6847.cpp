#include "test_harness.h"
#include "plugins/devices/mc6847/main/mc6847.h"
#include <cstring>
#include <vector>

// A VDG fed from its own address counter (as the MC-10 wires it): byte
// `addr` of `mem`, with A/S from D7 and INV from D6 (the CoCo's wiring)
namespace {
struct Rig {
    Mc6847 vdg;
    std::vector<uint8_t> mem = std::vector<uint8_t>(0x2000, 0);
    uint16_t ext = 0;
    int hsFalls = 0, fsFalls = 0, fsRises = 0;
    explicit Rig(bool t1 = false) : vdg(t1) {
        vdg.fetch = [this](uint16_t addr, int n, uint16_t* out) {
            for (int i = 0; i < n; ++i) {
                const uint8_t d = mem[(addr + i) & 0x1FFF];
                out[i] = (uint16_t)(d | ext | ((d & 0x80) ? Mc6847::F_AS : 0) | ((d & 0x40) ? Mc6847::F_INV : 0));
            }
        };
        vdg.onHs = [this](bool l) { if (!l) ++hsFalls; };
        vdg.onFs = [this](bool l) { if (l) ++fsRises; else ++fsFalls; };
    }
    void frame() { vdg.advance((uint64_t)Mc6847::LINE_TICKS * Mc6847::FRAME_LINES); }
};
}  // namespace

// NTSC timing: 912 ticks a line, 262 lines; FS falls once a frame at the
// end of the active area (line 230), HS once a line
TEST_CASE(mc6847_frame_timing) {
    Rig r;
    r.frame();
    ASSERT_EQ(r.hsFalls, 262);
    ASSERT_EQ(r.fsFalls, 1);
    ASSERT_EQ(r.fsRises, 1);
    ASSERT_EQ(r.vdg.scanline(), 0);
    // FS falls exactly when line 230 starts
    Rig s;
    s.vdg.advance((uint64_t)Mc6847::LINE_TICKS * 230 - 1);
    ASSERT_EQ(s.fsFalls, 0);
    s.vdg.advance(1);
    ASSERT_EQ(s.fsFalls, 1);
    ASSERT(!s.vdg.fs());
    // HS is low for the first 64 ticks of a line
    s.vdg.advance(10);
    ASSERT(!s.vdg.hs());
    s.vdg.advance(60);
    ASSERT(s.vdg.hs());
}

// Alphanumerics: the internal font, green on dark green, D6 inverting
TEST_CASE(mc6847_text_internal_font) {
    Rig r;
    r.mem[0] = 0x08;           // H, normal: dark green dots on... the font's 1s are the foreground
    r.mem[1] = 0x48;           // H with D6: inverse
    r.vdg.setMode(0);
    r.frame();
    // H's row 3 is $22: ..X...X. (six dots, bit 5 leftmost of the 8-dot cell)
    const uint8_t fg = Mc6847::GREEN, bg = Mc6847::DARK_GREEN;
    const uint8_t row3[8] = {bg, bg, fg, bg, bg, bg, fg, bg};
    for (int x = 0; x < 8; ++x) ASSERT_EQ(r.vdg.activeDot(x, 3), row3[x]);
    for (int x = 0; x < 8; ++x) ASSERT_EQ(r.vdg.activeDot(8 + x, 3), row3[x] == fg ? bg : fg);
    // Twelve lines a row: the next row of characters starts at line 12 (address 32)
    r.mem[32] = 0x48;
    r.frame();
    ASSERT_EQ(r.vdg.activeDot(2, 12 + 3), bg);
    ASSERT_EQ(r.vdg.activeDot(0, 12 + 3), fg);
    // CSS: orange text
    r.vdg.setMode(0x08);
    r.frame();
    ASSERT_EQ(r.vdg.activeDot(2, 3), Mc6847::BRIGHT_ORANGE);
    ASSERT_EQ(r.vdg.activeDot(0, 3), Mc6847::DARK_ORANGE);
    // Border: black in text modes
    ASSERT_EQ(r.vdg.frame()[0], Mc6847::rgb(Mc6847::BLACK));
}

// Semigraphics 4: D7 set, colour in D4-D6, a 2x2 block per character
TEST_CASE(mc6847_semigraphics4) {
    Rig r;
    r.mem[0] = 0x80 | (3 << 4) | 0x9;     // red; top-left and bottom-right
    r.vdg.setMode(0);
    r.frame();
    ASSERT_EQ(r.vdg.activeDot(0, 0), Mc6847::RED);
    ASSERT_EQ(r.vdg.activeDot(4, 0), Mc6847::BLACK);
    ASSERT_EQ(r.vdg.activeDot(0, 6), Mc6847::BLACK);
    ASSERT_EQ(r.vdg.activeDot(7, 11), Mc6847::RED);
}

// Semigraphics 6 (INT/EXT with A/S): 2x3 blocks, colour from CSS and D6-D7
TEST_CASE(mc6847_semigraphics6) {
    Rig r;
    r.ext = Mc6847::F_EXT;
    r.mem[0] = 0x80 | 0x20;               // colour 2 (blue), top-left block
    r.vdg.setMode(0x10);                  // GM0 (INT/EXT on the CoCo)
    r.frame();
    ASSERT_EQ(r.vdg.activeDot(0, 0), Mc6847::BLUE);
    ASSERT_EQ(r.vdg.activeDot(0, 4), Mc6847::BLACK);
    ASSERT_EQ(r.vdg.activeDot(4, 0), Mc6847::BLACK);
}

// Full graphics: RG6 (PMODE 4) is 256x192 two-colour; CG1 64x64 four-colour
TEST_CASE(mc6847_graphics_modes) {
    Rig r;
    r.mem[0] = 0xA5;                      // 1010 0101
    r.vdg.setMode(0xF0);                  // A/G, GM 111: RG6, green / dark green
    r.frame();
    const uint8_t on = Mc6847::GREEN, off = Mc6847::DARK_GREEN;
    const uint8_t want[8] = {on, off, on, off, off, on, off, on};
    for (int x = 0; x < 8; ++x) ASSERT_EQ(r.vdg.activeDot(x, 0), want[x]);
    ASSERT_EQ(r.vdg.activeDot(0, 1), off);           // one line a row: line 1 is byte 32
    r.vdg.setMode(0xF8);                  // CSS: buff / black, buff border
    r.frame();
    ASSERT_EQ(r.vdg.activeDot(0, 0), Mc6847::BUFF);
    ASSERT_EQ(r.vdg.activeDot(1, 0), Mc6847::BLACK);
    ASSERT_EQ(r.vdg.frame()[0], Mc6847::rgb(Mc6847::BUFF));

    Rig c;
    c.mem[0] = 0x1B;                      // 00 01 10 11: green yellow blue red
    c.vdg.setMode(0x80);                  // CG1: 16 bytes a line, four-dot pixels, three lines a row
    c.frame();
    ASSERT_EQ(c.vdg.activeDot(0, 0), Mc6847::GREEN);
    ASSERT_EQ(c.vdg.activeDot(3, 0), Mc6847::GREEN);
    ASSERT_EQ(c.vdg.activeDot(4, 0), Mc6847::YELLOW);
    ASSERT_EQ(c.vdg.activeDot(8, 2), Mc6847::BLUE);
    ASSERT_EQ(c.vdg.activeDot(12, 0), Mc6847::RED);
    ASSERT_EQ(c.vdg.activeDot(0, 3), Mc6847::GREEN);  // row 2 is bytes 16-31 (zero)
    ASSERT_EQ(c.vdg.activeDot(4, 3), Mc6847::GREEN);
    c.vdg.setMode(0x88);                  // CSS: buff cyan magenta orange
    c.frame();
    ASSERT_EQ(c.vdg.activeDot(12, 0), Mc6847::ORANGE);
}

// Artifact colours in PMODE 4 with CSS: pairs of dots by which is lit
TEST_CASE(mc6847_artifact_colours) {
    Rig r;
    r.mem[0] = 0x55;                      // 01 01 01 01
    r.mem[1] = 0xAA;                      // 10 10 10 10
    r.mem[2] = 0xF0;
    r.vdg.setMode(0xF8);
    r.frame();
    r.frame();
    ASSERT_EQ(r.vdg.activeDot(0, 0), Mc6847::BLACK);           // off: plain dots
    ASSERT_EQ(r.vdg.activeDot(1, 0), Mc6847::BUFF);
    r.vdg.setArtifact(Mc6847::Artifact::BlueRed);
    r.frame();
    ASSERT_EQ(r.vdg.activeDot(0, 0), Mc6847::ARTIFACT_BLUE);
    ASSERT_EQ(r.vdg.activeDot(1, 0), Mc6847::ARTIFACT_BLUE);
    ASSERT_EQ(r.vdg.activeDot(8, 0), Mc6847::ARTIFACT_RED);
    ASSERT_EQ(r.vdg.activeDot(16, 0), Mc6847::BUFF);
    ASSERT_EQ(r.vdg.activeDot(20, 0), Mc6847::BLACK);
    r.vdg.setArtifact(Mc6847::Artifact::RedBlue);
    r.frame();
    ASSERT_EQ(r.vdg.activeDot(0, 0), Mc6847::ARTIFACT_RED);
    ASSERT_EQ(r.vdg.activeDot(8, 0), Mc6847::ARTIFACT_BLUE);
    // Without CSS (green) no artifacts
    r.vdg.setMode(0xF0);
    r.frame();
    ASSERT_EQ(r.vdg.activeDot(1, 0), Mc6847::GREEN);
}

// A mode change mid-line takes effect from there on
TEST_CASE(mc6847_mid_line_mode_change) {
    Rig r;
    std::memset(r.mem.data(), 0xFF, 64);
    r.vdg.setMode(0xF0);
    // To line 38 (the first active line), then half way across the active area
    r.vdg.advance((uint64_t)Mc6847::LINE_TICKS * Mc6847::ACTIVE_FIRST + Mc6847::ACTIVE_START + 256);
    r.vdg.setMode(0xF8);
    r.vdg.advance((uint64_t)Mc6847::LINE_TICKS * Mc6847::FRAME_LINES);
    ASSERT_EQ(r.vdg.activeDot(10, 0), Mc6847::GREEN);
    ASSERT_EQ(r.vdg.activeDot(200, 0), Mc6847::BUFF);
}

// The MC6847T1 (CoCo 2B): lower case with INT/EXT; GM1 inverts the text
TEST_CASE(mc6847t1_lower_case) {
    Rig r(true);
    ASSERT(r.vdg.isT1());
    r.mem[0] = 0x41;                      // 'A' (upper case with the internal set)
    r.vdg.setMode(0);
    r.frame();
    // Upper case: the T1 inverts with D6 as the 6847 does: the background is lit
    ASSERT_EQ(r.vdg.activeDot(0, 0), Mc6847::GREEN);
    r.ext = Mc6847::F_EXT;                // lower case: $01 = 'a'
    r.mem[0] = 0x01;
    r.vdg.setMode(0x10);
    r.frame();
    // 'a' row 3 is $1C: ...XXX.. in the font; the T1 shows lower case inverted (lit background)
    ASSERT_EQ(r.vdg.activeDot(0, 3), Mc6847::GREEN);
    ASSERT_EQ(r.vdg.activeDot(3, 3), Mc6847::DARK_GREEN);
    r.vdg.setMode(0x30);                  // GM1: inverse video
    r.frame();
    ASSERT_EQ(r.vdg.activeDot(3, 3), Mc6847::GREEN);
}

// PAL padding (the Dragon's): 25 blank lines at line 0 and 25 at line 254
// make a 312-line frame; FS rises every 312 x 912 ticks, HS falls on every
// line, the active area keeps its 192 lines
TEST_CASE(mc6847_pal_padding) {
    Rig r;
    Mc6847::PalPadding pal;
    pal.stop0 = 262; pal.delay0 = 25;
    pal.stop1 = 254; pal.delay1 = 25;
    r.vdg.setPal(pal);
    ASSERT(r.vdg.isPal());
    ASSERT_EQ(r.vdg.frameLines(), 312);
    // To the first FS rise after reset (no padding at reset's line 0)
    uint64_t t = 0;
    while (r.fsRises == 0) { r.vdg.advance(1); ++t; }
    ASSERT_EQ(t, (uint64_t)912 * (262 + 25));
    ASSERT_EQ(r.vdg.paddingLeft(), 25);      // stopped at line 0
    // A whole frame: FS rise to FS rise
    const int hs0 = r.hsFalls, fall0 = r.fsFalls;
    t = 0;
    while (r.fsRises == 1) { r.vdg.advance(1); ++t; }
    ASSERT_EQ(t, (uint64_t)912 * 312);
    ASSERT_EQ(r.hsFalls - hs0, 312);
    ASSERT_EQ(r.fsFalls - fall0, 1);
    // FS is low from line 230 to the frame's end: 32 VDG lines and 25 padding
    Rig s;
    s.vdg.setPal(pal);
    s.frame();                                // 262 lines in: inside the line-254 padding
    while (s.fsRises == 0) s.vdg.advance(1);
    uint64_t low = 0;
    while (s.vdg.fs()) s.vdg.advance(1);
    while (!s.vdg.fs()) { s.vdg.advance(1); ++low; }
    ASSERT_EQ(low, (uint64_t)912 * (32 + 25));
}

// The Dragon 64's PAL logic holds HS during its padding: 262 HS a frame;
// the PAL CoCo's stops (24 at 16, 26 at 256) give 312 lines too
TEST_CASE(mc6847_pal_hs_inhibit_and_coco_stops) {
    Rig r;
    Mc6847::PalPadding pal;
    pal.stop0 = 262; pal.delay0 = 25;
    pal.stop1 = 254; pal.delay1 = 25;
    pal.hsInhibit = true;
    r.vdg.setPal(pal);
    while (r.fsRises == 0) r.vdg.advance(912);
    const int hs0 = r.hsFalls;
    uint64_t t = 0;
    while (r.fsRises == 1) { r.vdg.advance(1); ++t; }
    ASSERT_EQ(t, (uint64_t)912 * 312);
    ASSERT_EQ(r.hsFalls - hs0, 262);
    Rig c;
    pal.stop0 = 16; pal.delay0 = 24;
    pal.stop1 = 256; pal.delay1 = 26;
    pal.hsInhibit = false;
    c.vdg.setPal(pal);
    while (c.fsRises == 0) c.vdg.advance(1);
    t = 0;
    while (c.fsRises == 1) { c.vdg.advance(1); ++t; }
    ASSERT_EQ(t, (uint64_t)912 * 312);
    // NTSC again
    c.vdg.setPal(Mc6847::PalPadding());
    ASSERT(!c.vdg.isPal());
    ASSERT_EQ(c.vdg.frameLines(), 262);
}
