#include "test_harness.h"
#include "libtoolchain/main/gfx_decode.h"
#include <vector>

namespace {
struct Mem {
    std::vector<uint8_t> m = std::vector<uint8_t>(0x10000, 0);
    gfx::ReadFn fn() { return [this](uint32_t a) { return m[a & 0xFFFF]; }; }
};
}

TEST_CASE(gfx_format_lookup) {
    gfx::Format f;
    ASSERT(gfx::formatById("sprite-mc", f));
    ASSERT(f == gfx::Format::SpriteMulti);
    ASSERT(gfx::formatById("CHAR-FCM", f));
    ASSERT(f == gfx::Format::CharFCM);
    ASSERT(!gfx::formatById("nope", f));
    ASSERT_EQ(gfx::info(gfx::Format::SpriteHires).stride, 64u);
    ASSERT_EQ(gfx::info(gfx::Format::CharHires).stride, 8u);
}

TEST_CASE(gfx_decode_hires_sprite) {
    Mem mem;
    mem.m[0x2000] = 0x80;   // row 0: leftmost pixel
    mem.m[0x2002] = 0x01;   // row 0: rightmost pixel (x = 23)
    mem.m[0x2000 + 20 * 3 + 1] = 0x18;   // row 20: x = 11, 12
    auto px = gfx::decode(gfx::Format::SpriteHires, mem.fn(), 0x2000);
    ASSERT_EQ(px.size(), (size_t)(24 * 21));
    ASSERT_EQ(px[0], 1);
    ASSERT_EQ(px[1], 0);
    ASSERT_EQ(px[23], 1);
    ASSERT_EQ(px[20 * 24 + 11], 1);
    ASSERT_EQ(px[20 * 24 + 12], 1);
    ASSERT_EQ(px[20 * 24 + 13], 0);
}

TEST_CASE(gfx_decode_multicolor) {
    Mem mem;
    mem.m[0x3000] = 0x1B;   // %00 01 10 11
    auto px = gfx::decode(gfx::Format::CharMulti, mem.fn(), 0x3000);
    ASSERT_EQ(px.size(), (size_t)(4 * 8));
    ASSERT_EQ(px[0], 0);
    ASSERT_EQ(px[1], 1);
    ASSERT_EQ(px[2], 2);
    ASSERT_EQ(px[3], 3);
    auto sp = gfx::decode(gfx::Format::SpriteMulti, mem.fn(), 0x3000);
    ASSERT_EQ(sp.size(), (size_t)(12 * 21));
    ASSERT_EQ(sp[3], 3);
}

TEST_CASE(gfx_decode_4bpp_and_8bpp) {
    Mem mem;
    mem.m[0x4000] = 0x12;
    // NCM: low nibble is the left pixel by default
    auto ncm = gfx::decode(gfx::Format::CharNCM, mem.fn(), 0x4000);
    ASSERT_EQ(ncm.size(), (size_t)(16 * 8));
    ASSERT_EQ(ncm[0], 2);
    ASSERT_EQ(ncm[1], 1);
    auto ncmSwapped = gfx::decode(gfx::Format::CharNCM, mem.fn(), 0x4000, true);
    ASSERT_EQ(ncmSwapped[0], 1);
    // 16-colour sprite: high nibble first (as the VIC-IV renderer draws it)
    auto s16 = gfx::decode(gfx::Format::Sprite16, mem.fn(), 0x4000);
    ASSERT_EQ(s16[0], 1);
    ASSERT_EQ(s16[1], 2);
    // 16-colour sprite without SPRX64EN: 6 pixels from 3 bytes per row
    mem.m[0x4003] = 0x9A;   // row 1, first byte
    auto s16n = gfx::decode(gfx::Format::Sprite16Narrow, mem.fn(), 0x4000);
    ASSERT_EQ(s16n.size(), (size_t)(6 * 21));
    ASSERT_EQ(s16n[0], 1);
    ASSERT_EQ(s16n[1], 2);
    ASSERT_EQ(s16n[6], 9);
    ASSERT_EQ(s16n[7], 0xA);
    // FCM: one byte per pixel
    mem.m[0x4009] = 0xAB;
    auto fcm = gfx::decode(gfx::Format::CharFCM, mem.fn(), 0x4000);
    ASSERT_EQ(fcm[9], 0xAB);   // row 1, x = 1
    // Extended height override
    auto tall = gfx::decode(gfx::Format::Sprite64, mem.fn(), 0x4000, false, 32);
    ASSERT_EQ(tall.size(), (size_t)(64 * 32));
}

TEST_CASE(gfx_render_sheet_layout_and_colors) {
    Mem mem;
    mem.m[0x1008] = 0x80;   // char 1, row 0, x 0
    gfx::Colors c;
    c.slot[0] = 0xFF000000;
    c.slot[1] = 0xFFFFFFFF;
    auto s = gfx::renderSheet(gfx::Format::CharHires, mem.fn(), 0x1000, 3, 2, 0, c);
    // 2 columns x 2 rows of 8x8 with 1-pixel gaps
    ASSERT_EQ(s.columns, 2);
    ASSERT_EQ(s.rows, 2);
    ASSERT_EQ(s.width, 2 * 8 + 3);
    ASSERT_EQ(s.height, 2 * 8 + 3);
    ASSERT_EQ(s.stride, 8u);
    // char 1 is at column 1: x = 1 + 8 + 1 = 10, y = 1
    ASSERT_EQ(s.argb[1 * s.width + 10], 0xFFFFFFFFu);
    ASSERT_EQ(s.argb[1 * s.width + 11], 0xFF000000u);
    ASSERT_EQ(s.argb[0], 0xFF303030u);   // grid
    // Hit testing
    ASSERT_EQ(gfx::itemAt(s, 1, 1), 0);
    ASSERT_EQ(gfx::itemAt(s, 10, 1), 1);
    ASSERT_EQ(gfx::itemAt(s, 1, 10), 2);
    ASSERT_EQ(gfx::itemAt(s, 10, 10), -1);   // no item 3
    ASSERT_EQ(gfx::itemAt(s, 9, 1), -1);     // gap
    ASSERT_EQ(gfx::itemAddress(s, 2), 0x1010u);

    // Multicolour sprites are drawn double-wide; 4bpp sprites use palette + base
    mem.m[0x2000] = 0x40;   // %01 -> MC0
    c.slot[1] = 0xFF112233;
    auto mc = gfx::renderSheet(gfx::Format::SpriteMulti, mem.fn(), 0x2000, 1, 1, 0, c, false, 0);
    ASSERT_EQ(mc.width, 24);
    ASSERT_EQ(mc.argb[0], 0xFF112233u);
    ASSERT_EQ(mc.argb[1], 0xFF112233u);

    mem.m[0x3000] = 0x30;
    c.palette = [](int i) { return 0xFF000000u | (uint32_t)i; };
    c.paletteBase = 0x20;
    c.transparent = 0xFF00FF00;
    auto s16 = gfx::renderSheet(gfx::Format::Sprite16, mem.fn(), 0x3000, 1, 1, 0, c, false, 0);
    ASSERT_EQ(s16.argb[0], 0xFF000023u);
    ASSERT_EQ(s16.argb[1], 0xFF00FF00u);   // nibble 0 is transparent
}
