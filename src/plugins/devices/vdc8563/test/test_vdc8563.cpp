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
// 80 x 25 text with the KERNAL's layout: screen $0000, attributes $0800,
// characters $2000 (16 bytes each), 8 x 8 cells
void setupText(Vdc8563& v) {
    const uint8_t regs[][2] = {{0, 126}, {1, 80}, {4, 32}, {6, 25}, {9, 7}, {12, 0x00}, {13, 0x00},
                               {20, 0x08}, {21, 0x00}, {22, 0x78}, {23, 8}, {24, 0x00}, {25, 0x47},
                               {26, 0xE0}, {27, 0}, {28, 0x20}, {10, 0x20}, {11, 7}, {14, 0xFF}, {15, 0xFF}};
    for (auto& r : regs) reg(v, r[0], r[1]);
    // Character 1: a left column of dots on every line; character 2: all set
    for (int y = 0; y < 8; ++y) {
        reg(v, 18, 0x20); reg(v, 19, (uint8_t)(0x10 + y)); reg(v, 31, 0x80);
        reg(v, 18, 0x20); reg(v, 19, (uint8_t)(0x20 + y)); reg(v, 31, 0xFF);
    }
}
uint32_t px(const std::vector<uint32_t>& f, int x, int line) { return f[(size_t)(line * 2) * Vdc8563::FRAME_W + x]; }
}  // namespace

// Text mode with attributes: the cell's colour, reverse, R26 background;
// the picture is centred (640 x 200 in 768 x 272)
TEST_CASE(vdc_render_text) {
    Vdc8563 v;
    setupText(v);
    reg(v, 18, 0x00); reg(v, 19, 0x00); reg(v, 31, 1); reg(v, 31, 2);   // screen: chars 1, 2
    reg(v, 18, 0x08); reg(v, 19, 0x00); reg(v, 31, 0x07); reg(v, 31, 0x4D);   // cyan; reverse yellow
    std::vector<uint32_t> f((size_t)Vdc8563::FRAME_W * Vdc8563::FRAME_H);
    v.renderFrame(f.data());
    const int x0 = (768 - 640) / 2, y0 = (272 - 200) / 2;
    EXPECT_EQ(px(f, x0, y0), Vdc8563::palette(7));            // char 1, first dot
    EXPECT_EQ(px(f, x0 + 1, y0), Vdc8563::palette(0));        // background (R26 low)
    EXPECT_EQ(px(f, x0 + 8, y0), Vdc8563::palette(0));        // char 2 reversed: all background
    EXPECT_EQ(px(f, x0 - 1, y0), Vdc8563::palette(0));        // border
    EXPECT_EQ(f[(size_t)(y0 * 2 + 1) * 768 + x0], Vdc8563::palette(7));   // each line drawn twice
    // Monochrome (R25 bit 6 off): R26 colours for all
    reg(v, 25, 0x07);
    v.renderFrame(f.data());
    EXPECT_EQ(px(f, x0, y0), Vdc8563::palette(14));
    EXPECT_EQ(px(f, x0 + 8, y0), Vdc8563::palette(14));       // no reverse without attributes
}

// Bitmap mode: a byte per 8 dots a line; attribute low nibble foreground,
// high nibble background (vdc-draw.c draw_std_bitmap)
TEST_CASE(vdc_render_bitmap) {
    Vdc8563 v;
    setupText(v);
    reg(v, 25, 0xC7);                                          // bitmap + attributes
    reg(v, 18, 0x00); reg(v, 19, 0x00); reg(v, 31, 0xF0);      // line 0, first 8 dots
    reg(v, 18, 0x00); reg(v, 19, 80); reg(v, 31, 0x0F);        // line 1
    reg(v, 18, 0x08); reg(v, 19, 0x00); reg(v, 31, 0x2D);      // fg 13, bg 2
    std::vector<uint32_t> f((size_t)Vdc8563::FRAME_W * Vdc8563::FRAME_H);
    v.renderFrame(f.data());
    const int x0 = 64, y0 = 36;
    EXPECT_EQ(px(f, x0, y0), Vdc8563::palette(13));
    EXPECT_EQ(px(f, x0 + 4, y0), Vdc8563::palette(2));
    EXPECT_EQ(px(f, x0, y0 + 1), Vdc8563::palette(2));
    EXPECT_EQ(px(f, x0 + 7, y0 + 1), Vdc8563::palette(13));
}
