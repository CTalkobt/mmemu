#include "test_harness.h"
#include "plugins/devices/vdc8563/main/vdc8563.h"

namespace {
void reg(Vdc8563& v, int r, uint8_t val) { v.ioWrite(nullptr, 0xD600, (uint8_t)r); v.ioWrite(nullptr, 0xD601, val); }
uint8_t get(Vdc8563& v, int r) { uint8_t x = 0; v.ioWrite(nullptr, 0xD600, (uint8_t)r); v.ioRead(nullptr, 0xD601, &x); return x; }
}  // namespace

// Status, register read-back masks (VICE vdc-mem.c regmask), mirrors
TEST_CASE(vdc_status_and_registers) {
    Vdc8563 v;
    uint8_t s = 0;
    v.ioRead(nullptr, 0xD600, &s);
    EXPECT_EQ(s & 0x87, 0x82);                     // ready, revision 2
    reg(v, 0, 0x7E);
    EXPECT_EQ(get(v, 0), 0x7E);
    reg(v, 5, 0x00);
    EXPECT_EQ(get(v, 5), 0xE0);                    // unused bits read 1
    reg(v, 36, 0x00);
    EXPECT_EQ(get(v, 36), 0xF0);
    EXPECT_EQ(get(v, 40), 0xFF);                   // no such register
    v.ioWrite(nullptr, 0xD6FE, 12);                // $D6FE mirrors $D600
    v.ioWrite(nullptr, 0xD6FF, 0x55);
    EXPECT_EQ(v.reg(12), 0x55);
}

// R31 writes/reads RAM at R18/R19 and advances; R30 fills or copies
TEST_CASE(vdc_memory_fill_copy) {
    Vdc8563 v;
    reg(v, 18, 0x20); reg(v, 19, 0x00);
    reg(v, 31, 0xAA); reg(v, 31, 0xBB);
    EXPECT_EQ(v.ramByte(0x2000), 0xAA);
    EXPECT_EQ(v.ramByte(0x2001), 0xBB);
    EXPECT_EQ(v.reg(19), 0x02);
    reg(v, 18, 0x20); reg(v, 19, 0x00);
    EXPECT_EQ(get(v, 31), 0xAA);                   // read advances too
    EXPECT_EQ(get(v, 31), 0xBB);
    // Fill 16 bytes of $11 at $1000
    reg(v, 18, 0x10); reg(v, 19, 0x00);
    reg(v, 24, 0x00); reg(v, 31, 0x11); reg(v, 30, 15);   // R31 write stores one, fill 15 more
    EXPECT_EQ(v.ramByte(0x1000), 0x11);
    EXPECT_EQ(v.ramByte(0x100F), 0x11);
    EXPECT_EQ(v.ramByte(0x1010), 0x00);
    EXPECT_EQ(v.reg(19), 0x10);
    // Copy $2000-$2001 to $3000
    reg(v, 18, 0x30); reg(v, 19, 0x00);
    reg(v, 32, 0x20); reg(v, 33, 0x00);
    reg(v, 24, 0x80); reg(v, 30, 2);
    EXPECT_EQ(v.ramByte(0x3000), 0xAA);
    EXPECT_EQ(v.ramByte(0x3001), 0xBB);
    EXPECT_EQ(v.reg(33), 0x02);
    // 16 KB: addresses wrap at $4000
    reg(v, 18, 0x40); reg(v, 19, 0x00); reg(v, 31, 0x77);
    EXPECT_EQ(v.ramByte(0x0000), 0x77);
}

namespace {
// 80 x 25 text with the KERNAL's PAL layout: screen $0000, attributes $0800,
// characters $2000 (16 bytes each), 8 x 8 cells, 40 rows of 8 lines a frame
void setupText(Vdc8563& v) {
    v.setPal(true);
    v.setClockHz(985248);
    v.reset();
    const uint8_t regs[][2] = {{0, 126}, {1, 80}, {2, 102}, {3, 0x49}, {4, 39}, {5, 0}, {6, 25}, {7, 32},
                               {8, 0}, {9, 7}, {12, 0x00}, {13, 0x00},
                               {20, 0x08}, {21, 0x00}, {22, 0x78}, {23, 8}, {24, 0x00}, {25, 0x47},
                               {26, 0xE0}, {27, 0}, {28, 0x20}, {10, 0x20}, {11, 7}, {14, 0xFF}, {15, 0xFF},
                               {34, 0x7D}, {35, 0x64}};
    for (auto& r : regs) reg(v, r[0], r[1]);
    // Character 1: a left column of dots on every line; character 2: all set
    for (int y = 0; y < 8; ++y) {
        reg(v, 18, 0x20); reg(v, 19, (uint8_t)(0x10 + y)); reg(v, 31, 0x80);
        reg(v, 18, 0x20); reg(v, 19, (uint8_t)(0x20 + y)); reg(v, 31, 0xFF);
    }
}
// A line is 127 * 8 dots at 16 MHz: ~62.6 cycles of a 985248 Hz CPU
constexpr uint64_t LINE = 63;
// Run to the start of a frame, then `frames` whole frames
void frames(Vdc8563& v, int n) {
    for (int i = 0; i < n; ++i) {
        const uint32_t f = v.frameCounter();
        for (int guard = 0; v.frameCounter() == f && guard < 1000; ++guard) v.tick(LINE);
    }
}
uint32_t px(const std::vector<uint32_t>& f, int x, int line) { return f[(size_t)(line * 2) * Vdc8563::FRAME_W + x]; }
// The first line and dot of a colour (the top-left of the display)
bool findFirst(const std::vector<uint32_t>& f, uint32_t c, int& x, int& line) {
    for (line = 0; line < Vdc8563::FRAME_H / 2; ++line)
        for (x = 0; x < Vdc8563::FRAME_W; ++x)
            if (px(f, x, line) == c) return true;
    return false;
}
std::vector<uint32_t> frame(Vdc8563& v) {
    std::vector<uint32_t> f((size_t)Vdc8563::FRAME_W * Vdc8563::FRAME_H);
    v.renderFrame(f.data());
    return f;
}
}  // namespace

// Text mode with attributes: the cell's colour, reverse, R26 background;
// the display starts after the left border (R2) and the top rows
TEST_CASE(vdc_render_text) {
    Vdc8563 v;
    setupText(v);
    reg(v, 18, 0x00); reg(v, 19, 0x00); reg(v, 31, 1); reg(v, 31, 2);   // screen: chars 1, 2
    reg(v, 18, 0x08); reg(v, 19, 0x00); reg(v, 31, 0x07); reg(v, 31, 0x4D);   // cyan; reverse yellow
    frames(v, 3);
    auto f = frame(v);
    int x0 = 0, y0 = 0;
    ASSERT(findFirst(f, Vdc8563::palette(7), x0, y0));
    EXPECT_EQ(x0, 112 - Vdc8563::X_OFFSET);                    // VICE: border (116 - R2) * 8
    printf("  display at %d, line %d\n", x0, y0);
    EXPECT_TRUE(y0 > 20 && y0 < 80);
    EXPECT_EQ(px(f, x0, y0 + 7), Vdc8563::palette(7));         // all 8 lines of the cell
    EXPECT_EQ(px(f, x0 + 1, y0), Vdc8563::palette(0));        // background (R26 low)
    EXPECT_EQ(px(f, x0 + 8, y0), Vdc8563::palette(0));        // char 2 reversed: all background
    EXPECT_EQ(f[(size_t)(y0 * 2 + 1) * Vdc8563::FRAME_W + x0], Vdc8563::palette(7));   // each line drawn twice
    // Monochrome (R25 bit 6 off): R26 colours for all
    reg(v, 25, 0x07);
    frames(v, 2);
    f = frame(v);
    EXPECT_EQ(px(f, x0, y0), Vdc8563::palette(14));
    EXPECT_EQ(px(f, x0 + 8, y0), Vdc8563::palette(14));       // no reverse without attributes
}

// Bitmap mode: a byte per 8 dots a line; attribute low nibble foreground,
// high nibble background (vdc-draw.c draw_std_bitmap)
TEST_CASE(vdc_render_bitmap) {
    Vdc8563 v;
    setupText(v);
    reg(v, 18, 0x00); reg(v, 19, 0x00); reg(v, 31, 1);        // find the text origin first
    reg(v, 18, 0x08); reg(v, 19, 0x00); reg(v, 31, 0x07);
    frames(v, 3);
    int x0 = 0, y0 = 0;
    ASSERT(findFirst(frame(v), Vdc8563::palette(7), x0, y0));
    reg(v, 25, 0xC7);                                          // bitmap + attributes
    reg(v, 18, 0x00); reg(v, 19, 0x00); reg(v, 31, 0xF0);      // line 0, first 8 dots
    reg(v, 18, 0x00); reg(v, 19, 80); reg(v, 31, 0x0F);        // line 1
    reg(v, 18, 0x08); reg(v, 19, 0x00); reg(v, 31, 0x2D);      // fg 13, bg 2
    frames(v, 2);
    auto f = frame(v);
    EXPECT_EQ(px(f, x0, y0), Vdc8563::palette(13));
    EXPECT_EQ(px(f, x0 + 4, y0), Vdc8563::palette(2));
    EXPECT_EQ(px(f, x0, y0 + 1), Vdc8563::palette(2));
    EXPECT_EQ(px(f, x0 + 7, y0 + 1), Vdc8563::palette(13));
}

// A register changed in mid-frame shows from that line on: R26's
// background split half way down
TEST_CASE(vdc_raster_split) {
    Vdc8563 v;
    setupText(v);
    frames(v, 2);
    for (int i = 0; i < 150; ++i) v.tick(LINE);                // ~150 lines into the frame
    reg(v, 26, 0xE4);                                          // background 4 (green)
    frames(v, 1);
    auto f = frame(v);
    int top = -1, bottom = -1;
    for (int y = 0; y < Vdc8563::FRAME_H / 2; ++y) {
        if (px(f, 2, y) == Vdc8563::palette(0) && top < 0) top = y;
        if (px(f, 2, y) == Vdc8563::palette(4)) bottom = y;
    }
    EXPECT_TRUE(top >= 0 && bottom > top);                     // black above, green below
    EXPECT_EQ(px(f, 2, 0), Vdc8563::palette(0));
    EXPECT_EQ(px(f, 2, Vdc8563::FRAME_H / 2 - 1), Vdc8563::palette(4));
    // The frame after has it all the way
    frames(v, 1);
    EXPECT_EQ(px(frame(v), 2, 0), Vdc8563::palette(4));
}

// Interlace (R8 = 3): the two fields go on alternate lines, each with
// every other line of the character (R9 = 15: 16 lines a row, 8 a field)
TEST_CASE(vdc_interlace) {
    Vdc8563 v;
    setupText(v);
    reg(v, 9, 15); reg(v, 23, 16); reg(v, 4, 19);              // 20 rows of 16 lines
    reg(v, 6, 12); reg(v, 7, 16);
    for (int y = 0; y < 16; ++y) {                              // char 1: even lines dot 0, odd lines dot 1
        reg(v, 18, 0x20); reg(v, 19, (uint8_t)(0x10 + y)); reg(v, 31, (y & 1) ? 0x40 : 0x80);
    }
    reg(v, 28, 0x20);
    reg(v, 18, 0x00); reg(v, 19, 0x00); reg(v, 31, 1);
    reg(v, 18, 0x08); reg(v, 19, 0x00); reg(v, 31, 0x07);
    reg(v, 8, 3);
    EXPECT_TRUE(v.interlaced());
    frames(v, 4);
    auto f = frame(v);
    int x0 = 0, y0 = 0;
    ASSERT(findFirst(f, Vdc8563::palette(7), x0, y0));         // the first dot of line 0 (even field)
    const int W = Vdc8563::FRAME_W;
    auto at = [&](int x, int row) { return f[(size_t)row * W + x]; };
    // Output rows 2*y0 (even field: char line 0) and 2*y0 + 1 (odd: line 1)
    EXPECT_EQ(at(x0, y0 * 2), Vdc8563::palette(7));
    EXPECT_EQ(at(x0 + 1, y0 * 2), Vdc8563::palette(0));
    EXPECT_EQ(at(x0, y0 * 2 + 1), Vdc8563::palette(0));
    EXPECT_EQ(at(x0 + 1, y0 * 2 + 1), Vdc8563::palette(7));
    // 8 field lines a character row: char line 14 / 15 on the 8th
    EXPECT_EQ(at(x0, (y0 + 7) * 2), Vdc8563::palette(7));
    EXPECT_EQ(at(x0 + 1, (y0 + 7) * 2 + 1), Vdc8563::palette(7));
}
