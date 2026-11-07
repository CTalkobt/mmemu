#include "test_harness.h"
#include "plugins/devices/apple2_video/main/apple2_video.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

// Apple II video (AppleWin's NTSC renderer, "Color (TV)" with 50% scan lines):
// text, inverse and flash, lo-res, hi-res and the half-dot delay, mixed mode,
// mid-frame mode changes, IIe 80-column / double hi-res / MouseText, and the
// floating bus. Expected scanner addresses are derived from the Apple II
// screen layout (UTAIIe ch. 5): a text byte for line v, column c is at
// $400 * page + (row % 8) * $80 + (row / 8) * $28 + c with row = v / 8; a
// hi-res byte at $2000 * page + (v % 8) * $400 + ((v / 8) % 8) * $80 +
// (v / 64) * $28 + c. Column c is fetched on cycle 25 + c of the line, and
// AppleWin's floating bus reads the byte for one cycle behind the beam.

namespace {

using V = Apple2Video;
constexpr uint32_t LINE = V::CYCLES_PER_LINE;
constexpr uint32_t FRAME = V::CYCLES_PER_FRAME;
constexpr int W = V::FRAME_WIDTH;

std::vector<uint8_t> readRom(const char* name) {
    std::string path = std::string("roms/apple2/") + name;
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

struct Rig {
    std::vector<uint8_t> main = std::vector<uint8_t>(65536, 0);
    std::vector<uint8_t> aux = std::vector<uint8_t>(65536, 0);
    std::vector<uint8_t> rom;
    V v;
    std::vector<uint32_t> frame = std::vector<uint32_t>((size_t)W * V::FRAME_HEIGHT, 0);

    explicit Rig(V::Model m, bool withAux = true) : v(m) {
        const bool iie = (m == V::Model::IIe || m == V::Model::IIeEnhanced);
        rom = readRom(iie ? "Apple2e_Enhanced_Video.rom" : "Apple2_Video.rom");
        v.setCharRom(rom);
        v.setMemory(main.data(), (iie && withAux) ? aux.data() : nullptr);
    }
    bool ok() const { return !rom.empty(); }

    void fill(std::vector<uint8_t>& mem, uint16_t from, uint16_t to, uint8_t b) {
        for (uint32_t a = from; a < to; a++) mem[a] = b;
    }
    /** Clock to cycle `c` of the frame (0-17029), moving forward. */
    void toCycle(uint32_t c) { v.clock((c + FRAME - v.cycleInFrame()) % FRAME); }
    /** Two whole frames, then the last one completed. */
    void render() {
        v.clock(2 * FRAME);
        v.renderFrame(frame.data());
    }
    uint32_t px(int x, int row) const { return frame[(size_t)row * W + x]; }
    /** Luma-ish brightness of a pixel (the green channel; the text path is grey). */
    int lum(int x, int row) const { return (int)((px(x, row) >> 8) & 0xFF); }
};

int r8(uint32_t p) { return (int)((p >> 16) & 0xFF); }
int g8(uint32_t p) { return (int)((p >> 8) & 0xFF); }
int b8(uint32_t p) { return (int)(p & 0xFF); }

/** Mean RGB of 4 pixels (one full colour cycle) starting at x on the frame row. */
struct Rgb { int r, g, b; };
Rgb avg4(const Rig& t, int x, int row) {
    Rgb s{0, 0, 0};
    for (int i = 0; i < 4; i++) {
        s.r += r8(t.px(x + i, row));
        s.g += g8(t.px(x + i, row));
        s.b += b8(t.px(x + i, row));
    }
    return {s.r / 4, s.g / 4, s.b / 4};
}

/**
 * Check a 7x8 glyph at text column `col`, character row `row`: dot d of glyph
 * line y is drawn on frame row 2 * (8 * row + y) at x = 14 * col + 2d and
 * 2d + 1. The TV's luma filter blurs, so a dot counts as lit if the mean of
 * its two pixels is at least 0x80 and dark below that; `dots(y)` gives the
 * expected 7 dots of line y, leftmost in bit 0.
 */
template <typename F>
bool glyphMatches(const Rig& t, int col, int row, F dots) {
    for (int y = 0; y < 8; y++) {
        const int fr = 2 * (8 * row + y);
        const uint8_t want = dots(y);
        for (int d = 0; d < 7; d++) {
            const int x = 14 * col + 2 * d;
            const int m = (t.lum(x, fr) + t.lum(x + 1, fr)) / 2;
            const bool lit = m >= 0x80;
            if (lit != (((want >> d) & 1) != 0)) {
                printf("    glyph col %d row %d: line %d dot %d lum %02x want %d\n", col, row, y, d, m,
                       (want >> d) & 1);
                return false;
            }
        }
    }
    return true;
}

/** The dots of II/II+ character `ch` line y from the 2 KB ROM (patterns reversed, UTAII 8-30). */
uint8_t iiPlusDots(const std::vector<uint8_t>& rom, uint8_t ch, int y) {
    uint8_t n = rom[ch * 8 + y];
    uint8_t d = 0;
    for (int b = 0; b < 7; b++) d |= (uint8_t)(((n >> (6 - b)) & 1) << b);
    return d;
}

/** True if the 14-pixel cells at text columns a and b of character row `row` are identical. */
bool sameCell(const Rig& t, int a, int b, int row) {
    for (int fr = 16 * row; fr < 16 * row + 16; fr++)
        for (int x = -2; x < 14; x++)
            if (t.px(14 * a + x, fr) != t.px(14 * b + x, fr)) return false;
    return true;
}

}  // namespace

TEST_CASE(apple2_video_geometry) {
    V v(V::Model::IIe);
    auto d = v.getDimensions();
    ASSERT_EQ(d.width, 560);
    ASSERT_EQ(d.height, 384);
    ASSERT_EQ(d.displayWidth, 560);
    ASSERT_EQ(d.displayHeight, 384);
    ASSERT_EQ(V::CYCLES_PER_FRAME, 17030u);
    // A frame completes each time the beam reaches line 192
    v.clock(192 * LINE - 1);
    ASSERT_EQ(v.frameCount(), 0u);
    v.clock(1);
    ASSERT_EQ(v.frameCount(), 1u);
    v.clock(FRAME);
    ASSERT_EQ(v.frameCount(), 2u);
    ASSERT_EQ(v.vert(), 192);
    ASSERT_EQ(v.horz(), 0);
}

TEST_CASE(apple2_video_text_glyph) {
    Rig t(V::Model::IIPlus);
    if (!t.ok()) { printf("  (skipped: roms/apple2/Apple2_Video.rom not found)\n"); return; }
    t.fill(t.main, 0x400, 0x800, 0xA0);   // normal spaces
    // 'A' (normal, $C1) at row 5 column 10: $400 + 5 * $80 + 10
    t.main[0x400 + 5 * 0x80 + 10] = 0xC1;
    // 'Z' ($DA) at row 12 column 33: $400 + 4 * $80 + 1 * $28 + 33
    t.main[0x400 + 4 * 0x80 + 0x28 + 33] = 0xDA;
    t.render();
    ASSERT(glyphMatches(t, 10, 5, [&](int y) { return iiPlusDots(t.rom, 0xC1, y); }));
    ASSERT(glyphMatches(t, 33, 12, [&](int y) { return iiPlusDots(t.rom, 0xDA, y); }));
    // The cells next to it are blank (black)
    for (int fr = 80; fr < 96; fr++) {
        ASSERT(t.lum(14 * 9 - 2, fr) < 0x10);
        ASSERT(t.lum(14 * 12, fr) < 0x10);
    }
    // Text without colour burst is grey (B&W TV table)
    const uint32_t p = t.px(14 * 10 + 2 * 3, 2 * (8 * 5 + 1));
    ASSERT_EQ(r8(p), g8(p));
    ASSERT_EQ(g8(p), b8(p));
}

TEST_CASE(apple2_video_text_inverse_flash) {
    Rig t(V::Model::IIPlus);
    if (!t.ok()) { printf("  (skipped: roms/apple2/Apple2_Video.rom not found)\n"); return; }
    t.fill(t.main, 0x400, 0x800, 0xA0);
    t.main[0x400 + 10] = 0xC1;   // normal A
    t.main[0x400 + 20] = 0x01;   // inverse A
    t.main[0x400 + 30] = 0x41;   // flashing A
    t.render();
    // Inverse: the dots of the normal glyph, complemented (7 dots wide)
    ASSERT(glyphMatches(t, 20, 0, [&](int y) { return (uint8_t)(~iiPlusDots(t.rom, 0xC1, y) & 0x7F); }));

    // Flash: 16 frames in each phase (AppleWin updateFlashRate). Frame n is
    // the one completed when the beam reaches line 192 of the n-th frame.
    Rig f(V::Model::IIPlus);
    f.fill(f.main, 0x400, 0x800, 0xA0);
    f.main[0x400 + 10] = 0xC1;
    f.main[0x400 + 20] = 0x01;
    f.main[0x400 + 30] = 0x41;
    auto frameN = [&](uint32_t n) {
        f.toCycle(192 * LINE);
        while (f.v.frameCount() < n + 1) f.v.clock(FRAME);
        f.v.renderFrame(f.frame.data());
    };
    frameN(0);
    ASSERT(!f.v.flashInverted());
    const bool firstNormal = sameCell(f, 30, 10, 0);
    ASSERT(firstNormal || sameCell(f, 30, 20, 0));
    frameN(15);
    ASSERT_EQ(sameCell(f, 30, 10, 0), firstNormal);
    frameN(16);
    ASSERT(f.v.flashInverted());
    ASSERT_EQ(sameCell(f, 30, 20, 0), firstNormal);   // now shows the other one
    ASSERT_EQ(sameCell(f, 30, 10, 0), !firstNormal);
    frameN(32);
    ASSERT(!f.v.flashInverted());
    ASSERT_EQ(sameCell(f, 30, 10, 0), firstNormal);
}

TEST_CASE(apple2_video_lores_colours) {
    Rig t(V::Model::IIPlus);
    if (!t.ok()) { printf("  (skipped: roms/apple2/Apple2_Video.rom not found)\n"); return; }
    t.v.setMode(0);   // GR, full screen
    Rgb c[16];
    for (int i = 0; i < 16; i++) {
        t.fill(t.main, 0x400, 0x800, (uint8_t)(i * 0x11));
        t.render();
        c[i] = avg4(t, 280, 200);
    }
    // Black and white are exact (AppleWin removes black ghosting and white ringing)
    t.fill(t.main, 0x400, 0x800, 0x00);
    t.render();
    ASSERT_EQ(t.px(281, 200), 0xFF000000u);
    t.fill(t.main, 0x400, 0x800, 0xFF);
    t.render();
    ASSERT_EQ(t.px(281, 200), 0xFFFFFFFFu);
    // The colours: 1 magenta, 2 dark blue, 4 dark green, 6 medium blue,
    // 9 orange, 12 light green, 13 yellow; 5 and 10 grey
    ASSERT(c[1].r > c[1].g + 80 && c[1].b > c[1].g + 80);
    ASSERT(c[2].b > c[2].r + 100 && c[2].b > c[2].g + 100);
    ASSERT(c[4].g > c[4].r + 80 && c[4].g > c[4].b + 80);
    ASSERT(c[6].b > c[6].r + 100 && c[6].g > c[6].r + 100);
    ASSERT(c[9].r > c[9].g + 100 && c[9].g > c[9].b);
    ASSERT(c[12].g > c[12].r + 80 && c[12].g > c[12].b + 150);
    ASSERT(c[13].r > c[13].b + 150 && c[13].g > c[13].b + 150);
    EXPECT_NEAR(c[5].r, c[5].g, 8);
    EXPECT_NEAR(c[10].g, c[10].b, 8);
    // Brightness order: 0 < 1 < 3 < 7 < 15 (more bits set, brighter)
    auto y = [](Rgb k) { return k.r + k.g + k.b; };
    ASSERT(y(c[1]) < y(c[3]) && y(c[3]) < y(c[7]) && y(c[7]) < 3 * 255);

    // A byte is two blocks: low nibble lines 0-3, high nibble lines 4-7
    t.fill(t.main, 0x400, 0x800, 0x0F);
    t.render();
    ASSERT_EQ(t.px(281, 2 * 50), 0xFFFFFFFFu);        // line 50 = block row 12, line 2 of 8
    ASSERT_EQ(t.px(281, 2 * 54), 0xFF000000u);        // line 54: line 6 of 8
}

TEST_CASE(apple2_video_hires_white_and_colours) {
    Rig t(V::Model::IIPlus);
    if (!t.ok()) { printf("  (skipped: roms/apple2/Apple2_Video.rom not found)\n"); return; }
    t.v.setMode(V::VF_HIRES);
    t.fill(t.main, 0x2000, 0x4000, 0x7F);
    t.render();
    for (int x = 4; x < 556; x++) ASSERT_EQ(t.px(x, 2 * 100), 0xFFFFFFFFu);

    // Colour pairs: even/odd column bytes. Bit 7 delays the dots half a dot
    // (one 14M pixel), turning violet into blue and green into orange.
    auto pair = [&](uint8_t even, uint8_t odd) {
        for (uint32_t a = 0x2000; a < 0x4000; a++) t.main[a] = ((a & 0x7F) % 0x28) % 2 ? odd : even;
        t.render();
        return avg4(t, 280, 2 * 100);
    };
    const Rgb violet = pair(0x55, 0x2A);
    const Rgb green = pair(0x2A, 0x55);
    const Rgb blue = pair(0xD5, 0xAA);
    const Rgb orange = pair(0xAA, 0xD5);
    ASSERT(violet.r > violet.g + 100 && violet.b > violet.g + 100);
    ASSERT(green.g > green.r + 100 && green.g > green.b + 100);
    ASSERT(blue.b > blue.r + 100 && blue.b > blue.g);
    ASSERT(orange.r > orange.b + 150 && orange.r > orange.g);

    // Half-dot delay: one dot ($01 vs $81) lands one pixel to the right
    auto centroid = [&](uint8_t b) {
        t.fill(t.main, 0x2000, 0x4000, 0x00);
        for (int line = 0; line < 192; line++)   // column 20 of every line
            t.main[0x2000 + (line % 8) * 0x400 + ((line / 8) % 8) * 0x80 + (line / 64) * 0x28 + 20] = b;
        t.render();
        double s = 0, sx = 0;
        for (int x = 14 * 20 - 8; x < 14 * 20 + 16; x++) {
            const uint32_t p = t.px(x, 2 * 100);
            const double l = r8(p) + g8(p) + b8(p);
            s += l;
            sx += l * x;
        }
        return sx / s;
    };
    const double c0 = centroid(0x01), c1 = centroid(0x81);
    EXPECT_NEAR(c1 - c0, 1.0, 0.35);
}

TEST_CASE(apple2_video_mixed_mode) {
    Rig t(V::Model::IIPlus);
    if (!t.ok()) { printf("  (skipped: roms/apple2/Apple2_Video.rom not found)\n"); return; }
    t.v.setMode(V::VF_HIRES | V::VF_MIXED);
    t.fill(t.main, 0x2000, 0x4000, 0x7F);    // hi-res page 1 all white
    t.fill(t.main, 0x400, 0x800, 0xA0);      // text page 1 all spaces
    t.main[0x400 + 6 * 0x80 + 2 * 0x28 + 5] = 0xC1;   // 'A' on text row 22, column 5
    t.render();
    for (int x = 20; x < 540; x += 7) {
        ASSERT_EQ(t.px(x, 2 * 0), 0xFFFFFFFFu);
        ASSERT_EQ(t.px(x, 2 * 159), 0xFFFFFFFFu);   // last graphics line
        ASSERT_EQ(t.px(x, 2 * 160), 0xFF000000u);   // first of the 4 text rows
        ASSERT_EQ(t.px(x, 2 * 191), 0xFF000000u);
    }
    // The text shows in the bottom 4 rows (in colour, the burst stays on)
    bool lit = false;
    for (int fr = 2 * 176; fr < 2 * 184; fr += 2)
        for (int x = 14 * 5; x < 14 * 6; x++) lit = lit || t.lum(x, fr) > 0x80;
    ASSERT(lit);
    // Text rows 20-23 only: the same text byte on row 19 is not shown
    t.main[0x400 + 3 * 0x80 + 2 * 0x28 + 5] = 0xC1;   // row 19 (line 152-159, graphics)
    t.render();
    for (int x = 14 * 5; x < 14 * 6; x++) ASSERT_EQ(t.px(x, 2 * 155), 0xFFFFFFFFu);
}

TEST_CASE(apple2_video_mode_change_mid_frame) {
    Rig t(V::Model::IIPlus);
    if (!t.ok()) { printf("  (skipped: roms/apple2/Apple2_Video.rom not found)\n"); return; }
    t.fill(t.main, 0x400, 0x800, 0xA0);      // text: spaces (black)
    t.fill(t.main, 0x2000, 0x4000, 0x7F);    // hi-res: white
    t.v.clock(FRAME);                        // a full TEXT frame
    t.toCycle(100 * LINE);                   // start of line 100
    t.v.setMode(V::VF_HIRES);
    t.toCycle(130 * LINE + 45);              // mid-line 130 (column 20 fetched at cycle 45)
    t.v.setMode(V::VF_TEXT);
    t.toCycle(192 * LINE);                   // frame completes
    t.v.renderFrame(t.frame.data());
    for (int x = 20; x < 540; x += 7) {
        ASSERT(t.lum(x, 2 * 99) < 4);                // above the change: text (B&W black is $010101)
        ASSERT_EQ(t.px(x, 2 * 100), 0xFFFFFFFFu);   // from the line it happened on: hi-res
        ASSERT_EQ(t.px(x, 2 * 129), 0xFFFFFFFFu);
        ASSERT_EQ(t.px(x, 2 * 131), 0xFF000000u);
    }
    // Line 130: hi-res up to the switch, text after it (the TEXT change lands one cycle late)
    ASSERT_EQ(t.px(14 * 10, 2 * 130), 0xFFFFFFFFu);
    ASSERT(t.lum(14 * 30, 2 * 130) < 4);
}

TEST_CASE(apple2_video_floating_bus) {
    Rig t(V::Model::IIe);
    for (uint32_t a = 0; a < 65536; a++) t.main[a] = (uint8_t)(a * 7 + (a >> 8) * 13);
    auto text = [](int page, int v, int col) {
        const int row = v / 8;
        return (uint16_t)(0x400 * page + (row % 8) * 0x80 + (row / 8) * 0x28 + col);
    };
    auto hgr = [](int page, int v, int col) {
        return (uint16_t)(0x2000 * page + (v % 8) * 0x400 + ((v / 8) % 8) * 0x80 + (v / 64) * 0x28 + col);
    };
    auto at = [&](uint32_t vert, uint32_t horz) { t.toCycle(vert * LINE + horz); };
    // Text page 1: at cycle h the floating bus holds the byte of column h - 26
    at(0, 26);
    ASSERT_EQ(t.v.scannerAddress(), text(1, 0, 0));          // $0400
    ASSERT_EQ(t.v.floatingBus(), t.main[0x0400]);
    at(8, 30);
    ASSERT_EQ(t.v.scannerAddress(), text(1, 8, 4));          // $0484
    at(64, 26);
    ASSERT_EQ(t.v.scannerAddress(), text(1, 64, 0));         // $0428
    ASSERT_EQ(t.v.floatingBus(), t.main[0x0428]);
    at(191, 64);
    ASSERT_EQ(t.v.scannerAddress(), text(1, 191, 38));       // $07F6
    // Hi-res page 1
    t.v.setMode(V::VF_HIRES);
    at(1, 26);
    ASSERT_EQ(t.v.scannerAddress(), hgr(1, 1, 0));           // $2400
    at(100, 50);
    ASSERT_EQ(t.v.scannerAddress(), hgr(1, 100, 24));        // $3240
    ASSERT_EQ(t.v.floatingBus(), t.main[0x3240]);
    // Mixed: lines 160-191 fetch text
    t.v.setMode(V::VF_HIRES | V::VF_MIXED);
    at(170, 30);
    ASSERT_EQ(t.v.scannerAddress(), text(1, 170, 4));        // $06D4
    // Page 2 (80STORE off)
    t.v.setMode(V::VF_HIRES | V::VF_PAGE2);
    at(10, 40);
    ASSERT_EQ(t.v.scannerAddress(), hgr(2, 10, 14));         // $4A0E
    t.v.setMode(V::VF_TEXT | V::VF_PAGE2);
    at(0, 26);
    ASSERT_EQ(t.v.scannerAddress(), text(2, 0, 0));          // $0800
    // 80STORE on: PAGE2 selects aux memory, the video shows page 1
    t.v.setMode(V::VF_TEXT | V::VF_PAGE2 | V::VF_80STORE);
    at(0, 26);
    ASSERT_EQ(t.v.scannerAddress(), text(1, 0, 0));

    // Horizontal blanking, from the UTAIIe scanner equations: at cycle 0 the
    // H counter is 0 (cycle 0 = state 40 after the preset), so the sum
    // $D + 0 + 0 gives a3-a6 = $68; the II and II+ add $1000 in HBL, the IIe not.
    t.v.setMode(V::VF_TEXT);
    at(0, 1);
    ASSERT_EQ(t.v.scannerAddress(), 0x0468);
    Rig p(V::Model::IIPlus);
    p.toCycle(1);
    ASSERT_EQ(p.v.scannerAddress(), 0x1468);
    // At line 0 cycle 0 the read is for line 261, cycle 64: V = $FF after
    // the preset gives a7-a9 = $380, H = $3F gives sum 3 -> $1F: $079F
    Rig w(V::Model::IIe);
    ASSERT_EQ(w.v.scannerAddress(), 0x079F);

    // VBL sense ($C019 bit 7 on the IIe): high on lines 0-191
    at(191, 64);
    ASSERT(t.v.vblBar());
    at(192, 0);
    ASSERT(!t.v.vblBar());
    at(261, 64);
    ASSERT(!t.v.vblBar());
    at(0, 0);
    ASSERT(t.v.vblBar());
}

TEST_CASE(apple2_video_iie_80col_text) {
    Rig t(V::Model::IIeEnhanced);
    if (!t.ok()) { printf("  (skipped: roms/apple2/Apple2e_Enhanced_Video.rom not found)\n"); return; }
    t.fill(t.main, 0x400, 0x800, 0xA0);
    t.fill(t.aux, 0x400, 0x800, 0xA0);
    t.aux[0x400 + 10] = 0x20;    // inverse space: solid block, 7 dots
    t.v.setMode(V::VF_TEXT | V::VF_80COL);
    t.render();
    // Aux column 10 is drawn first (x 140-146), its main partner (blank) after (x 147-153)
    int left = 0, right = 0;
    for (int x = 14 * 10 + 1; x < 14 * 10 + 6; x++) left += t.lum(x, 2 * 3);
    for (int x = 14 * 10 + 8; x < 14 * 10 + 13; x++) right += t.lum(x, 2 * 3);
    ASSERT(left > 5 * 0xC0);
    ASSERT(right < 5 * 0x30);

    t.aux[0x400 + 10] = 0xA0;
    t.main[0x400 + 10] = 0x20;
    t.render();
    left = right = 0;
    for (int x = 14 * 10 + 1; x < 14 * 10 + 6; x++) left += t.lum(x, 2 * 3);
    for (int x = 14 * 10 + 8; x < 14 * 10 + 13; x++) right += t.lum(x, 2 * 3);
    ASSERT(left < 5 * 0x30);
    ASSERT(right > 5 * 0xC0);
}

TEST_CASE(apple2_video_iie_double_hires) {
    Rig t(V::Model::IIeEnhanced);
    if (!t.ok()) { printf("  (skipped: roms/apple2/Apple2e_Enhanced_Video.rom not found)\n"); return; }
    t.v.setMode(V::VF_HIRES | V::VF_DHIRES | V::VF_80COL);
    t.fill(t.main, 0x2000, 0x4000, 0x7F);
    t.fill(t.aux, 0x2000, 0x4000, 0x7F);
    t.render();
    for (int x = 4; x < 556; x++) ASSERT_EQ(t.px(x, 2 * 100), 0xFFFFFFFFu);
    // Aux only: the left 7 dots of each column lit, the main half dark
    t.fill(t.main, 0x2000, 0x4000, 0x00);
    t.render();
    int left = 0, right = 0;
    for (int col = 10; col < 30; col++) {
        for (int x = 14 * col + 1; x < 14 * col + 6; x++) left += t.lum(x, 2 * 100);
        for (int x = 14 * col + 8; x < 14 * col + 13; x++) right += t.lum(x, 2 * 100);
    }
    ASSERT(left > 3 * right);
    // Without aux memory DHIRES is ignored: plain hi-res from main
    Rig n(V::Model::IIeEnhanced, false);
    n.v.setMode(V::VF_HIRES | V::VF_DHIRES | V::VF_80COL);
    n.fill(n.main, 0x2000, 0x4000, 0x7F);
    n.render();
    ASSERT_EQ(n.px(280, 2 * 100), 0xFFFFFFFFu);
    // A 1-dot-in-4 pattern over the 28 dots of two columns (aux0 main0 aux1 main1:
    // $11 $22 $44 $08) is a colour, not grey
    const uint8_t pat[4] = {0x11, 0x22, 0x44, 0x08};
    for (uint32_t a = 0x2000; a < 0x4000; a++) {
        const int col = (int)((a & 0x7F) % 0x28);
        t.aux[a] = pat[(col % 2) * 2];
        t.main[a] = pat[(col % 2) * 2 + 1];
    }
    t.render();
    const Rgb c = avg4(t, 280, 2 * 100);
    ASSERT(std::abs(c.r - c.g) > 40 || std::abs(c.g - c.b) > 40);
}

TEST_CASE(apple2_video_iie_double_lores) {
    Rig t(V::Model::IIeEnhanced);
    if (!t.ok()) { printf("  (skipped: roms/apple2/Apple2e_Enhanced_Video.rom not found)\n"); return; }
    t.v.setMode(V::VF_DHIRES | V::VF_80COL);   // DGR
    t.fill(t.main, 0x400, 0x800, 0xFF);
    t.fill(t.aux, 0x400, 0x800, 0xFF);
    t.render();
    ASSERT_EQ(t.px(281, 200), 0xFFFFFFFFu);
    t.fill(t.aux, 0x400, 0x800, 0x00);
    t.render();
    int left = 0, right = 0;
    for (int x = 14 * 20 + 1; x < 14 * 20 + 6; x++) left += t.lum(x, 200);
    for (int x = 14 * 20 + 8; x < 14 * 20 + 13; x++) right += t.lum(x, 200);
    ASSERT(right > 3 * left);   // main is the right half
}

TEST_CASE(apple2_video_iie_altcharset_mousetext) {
    Rig t(V::Model::IIeEnhanced);
    if (!t.ok()) { printf("  (skipped: roms/apple2/Apple2e_Enhanced_Video.rom not found)\n"); return; }
    t.fill(t.main, 0x400, 0x800, 0xA0);
    t.main[0x400 + 10] = 0x40;   // primary: flashing '@'; alternate: MouseText
    t.main[0x400 + 20] = 0x61;   // primary: flashing 'a'; alternate: inverse 'a'
    // Alternate set glyphs straight from the ROM: char ch line y at ch * 8 + y, inverted, bit 0 leftmost
    auto alt = [&](uint8_t ch) { return [&, ch](int y) { return (uint8_t)((t.rom[ch * 8 + y] ^ 0xFF) & 0x7F); }; };

    t.v.setAltCharSet(true);
    t.render();
    ASSERT(glyphMatches(t, 10, 0, alt(0x40)));
    ASSERT(glyphMatches(t, 20, 0, alt(0x61)));
    std::vector<uint32_t> a = t.frame;
    t.v.clock(16 * FRAME);   // the alternate set does not flash
    t.v.renderFrame(t.frame.data());
    ASSERT(t.v.flashInverted());
    ASSERT(a == t.frame);

    // Primary set: $40 flashes, so the frames differ; and it is not the MouseText glyph
    t.v.setAltCharSet(false);
    t.render();
    std::vector<uint32_t> b = t.frame;
    t.v.clock(16 * FRAME);
    t.v.renderFrame(t.frame.data());
    ASSERT(!(b == t.frame));
    ASSERT(!(b == a));

    // Original IIe: no MouseText, the alternate $40 is the primary $40 glyph (inverse '@')
    Rig o(V::Model::IIe);
    o.fill(o.main, 0x400, 0x800, 0xA0);
    o.main[0x400 + 10] = 0x40;
    o.v.setAltCharSet(true);
    o.render();
    ASSERT(glyphMatches(o, 10, 0, [&](int y) { return (uint8_t)((o.rom[0x00 * 8 + y] ^ 0xFF) & 0x7F); }));
}

TEST_CASE(apple2_video_ii_ignores_iie_switches) {
    Rig t(V::Model::IIPlus);
    if (!t.ok()) { printf("  (skipped: roms/apple2/Apple2_Video.rom not found)\n"); return; }
    t.v.setMode(V::VF_HIRES | V::VF_DHIRES | V::VF_80COL);
    ASSERT_EQ(t.v.mode(), (uint32_t)V::VF_HIRES);
    t.fill(t.main, 0x2000, 0x4000, 0x7F);
    t.render();
    ASSERT_EQ(t.px(280, 2 * 100), 0xFFFFFFFFu);
}
