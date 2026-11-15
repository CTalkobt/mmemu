#include "test_harness.h"
#include "plugins/devices/hd61830/main/hd61830.h"

namespace {
void cmd(Hd61830& c, uint8_t ir, uint8_t data) {
    c.write(1, ir);
    c.write(0, data);
}
// The Model 200's set-up: graphic mode, 6-dot bytes, 40 per line, 1/64 duty
void m200Setup(Hd61830& c) {
    cmd(c, 0x0, 0x32);      // display on, master, graphic
    cmd(c, 0x1, 0x75);      // vp 8, hp 6
    cmd(c, 0x2, 39);        // 40 bytes a line
    cmd(c, 0x3, 63);        // 64 rows per half
    cmd(c, 0x8, 0);
    cmd(c, 0x9, 0);
}
}  // namespace

TEST_CASE(hd61830_registers) {
    Hd61830 c;
    m200Setup(c);
    ASSERT_EQ(c.mode(), 0x32);
    ASSERT_EQ(c.horizontalPitch(), 6);
    ASSERT_EQ(c.verticalPitch(), 8);
    ASSERT_EQ(c.charactersPerLine(), 40);
    ASSERT_EQ(c.timeDivisions(), 64);
    ASSERT_EQ(c.width(), 240);
    ASSERT_EQ(c.height(), 128);
    cmd(c, 0x2, 38);                       // 39 is made even: 40
    ASSERT_EQ(c.charactersPerLine(), 40);
    cmd(c, 0x8, 0x34);
    cmd(c, 0x9, 0x12);
    ASSERT_EQ(c.displayStart(), 0x1234);
    ASSERT_EQ(c.status(), 0x00);
    c.reset();
    ASSERT_EQ(c.mode() & 0x30, 0);         // display off, slave
}

// Cursor address: the low-byte carry quirk; writes, bit set/clear step it
TEST_CASE(hd61830_cursor_and_ram) {
    Hd61830 c;
    cmd(c, 0xA, 0xFE);
    cmd(c, 0xB, 0x00);
    c.write(1, 0x0C);
    c.write(0, 0x11);
    c.write(0, 0x22);
    ASSERT_EQ(c.ram(0x00FE), 0x11);
    ASSERT_EQ(c.ram(0x00FF), 0x22);
    ASSERT_EQ(c.cursorAddress(), 0x0100);
    // Low byte with bit 7 set, then a low byte with bit 7 clear: carry into high
    cmd(c, 0xB, 0x02);
    cmd(c, 0xA, 0x90);
    cmd(c, 0xA, 0x10);
    ASSERT_EQ(c.cursorAddress(), 0x0310);
    // Set / clear bit
    cmd(c, 0xA, 0x00);
    cmd(c, 0xB, 0x00);
    c.write(1, 0x0F);
    c.write(0, 3);                         // set bit 3 at 0, step
    c.write(0, 7);                         // set bit 7 at 1
    ASSERT_EQ(c.ram(0) & 0x08, 0x08);
    ASSERT_EQ(c.ram(1) & 0x80, 0x80);
    cmd(c, 0xA, 0x00);
    c.write(1, 0x0E);
    c.write(0, 3);                         // clear bit 3 at 0
    ASSERT_EQ(c.ram(0) & 0x08, 0);
    // The RAM wraps at its size (the Model 200's 8 KB)
    c.setRam(0x2005, 0xAB);
    ASSERT_EQ(c.ram(0x0005), 0xAB);
}

// Reading display data: the first read after setting the address is a dummy
TEST_CASE(hd61830_dummy_read) {
    Hd61830 c;
    for (int i = 0; i < 4; ++i) c.setRam((uint16_t)(0x100 + i), (uint8_t)(0x50 + i));
    cmd(c, 0xA, 0x00);
    cmd(c, 0xB, 0x01);
    c.write(1, 0x0D);
    (void)c.read(0);
    ASSERT_EQ(c.read(0), 0x50);
    ASSERT_EQ(c.read(0), 0x51);
    ASSERT_EQ(c.cursorAddress(), 0x0103);
}

// Graphic mode: 40 bytes a line, 6 dots each (bit 0 leftmost), 128 lines
TEST_CASE(hd61830_graphic_render) {
    Hd61830 c;
    m200Setup(c);
    c.setRam(0, 0x01);                     // (0, 0)
    c.setRam(39, 0x20);                    // (239, 0)
    c.setRam(40, 0x02);                    // (1, 1)
    c.setRam(64 * 40 + 5, 0x04);           // lower half, row 64: x 32
    c.setRam(127 * 40 + 39, 0x3F);         // last row, x 234-239
    c.setRam(1, 0xC0);                     // bits 6, 7: beyond the pitch, unseen
    const auto img = c.render(240, 128);
    ASSERT_EQ(img[0], 1);
    ASSERT_EQ(img[239], 1);
    ASSERT_EQ(img[240 + 1], 1);
    ASSERT_EQ(img[64 * 240 + 32], 1);
    for (int x = 234; x < 240; ++x) ASSERT_EQ(img[127 * 240 + x], 1);
    int on = 0;
    for (uint8_t p : img) on += p;
    ASSERT_EQ(on, 4 + 6);
    // Scrolling by the display start address (one line = 40 bytes)
    cmd(c, 0x8, 40);
    const auto img2 = c.render(240, 128);
    ASSERT_EQ(img2[1], 1);                 // line 1 is now on top
    // Display off: blank
    cmd(c, 0x0, 0x12);
    const auto img3 = c.render(240, 128);
    for (uint8_t p : img3) ASSERT_EQ(p, 0);
}

// Character mode: the generated 5x7 font, the cursor and blink
TEST_CASE(hd61830_character_mode) {
    Hd61830 c;
    cmd(c, 0x0, 0x34);                     // on, master, character, cursor on
    cmd(c, 0x1, 0x75);                     // 6 x 8 cells
    cmd(c, 0x2, 3);                        // 4 characters a row
    cmd(c, 0x3, 15);                       // 16 rows a half: 2 character rows each
    cmd(c, 0x4, 7);                        // cursor on line 7
    cmd(c, 0x8, 0);
    cmd(c, 0x9, 0);
    c.setRam(0, 'A');
    c.setRam(1, ' ');
    cmd(c, 0xA, 1);                        // cursor on cell 1
    cmd(c, 0xB, 0);
    const auto img = c.render(24, 32);
    // 'A' row 0 is ..#.. (bit 2)
    ASSERT_EQ(img[0 * 24 + 2], 1);
    ASSERT_EQ(img[0 * 24 + 0], 0);
    // Cursor: line 7 of cell 1 solid
    for (int x = 6; x < 12; ++x) ASSERT_EQ(img[7 * 24 + x], 1);
    ASSERT_EQ(img[6 * 24 + 6], 0);
    // Blink with the cursor on: the cursor shows only in the blink's on phase
    cmd(c, 0x0, 0x3C);
    ASSERT(!c.blinkPhase());
    ASSERT_EQ(c.render(24, 32)[7 * 24 + 6], 0);
    for (int i = 0; i < 32; ++i) c.frame();
    ASSERT(c.blinkPhase());
    ASSERT_EQ(c.render(24, 32)[7 * 24 + 6], 1);
    // Character blink (cursor off, blink on): the whole cell goes solid
    cmd(c, 0x0, 0x38);
    const auto img2 = c.render(24, 32);
    ASSERT_EQ(img2[3 * 24 + 8], 1);
    // An external character generator
    cmd(c, 0x0, 0x31);
    c.externalCg = [](uint8_t code, int line) -> uint8_t { return code == 'A' && line == 0 ? 0x3F : 0; };
    const auto img3 = c.render(24, 32);
    for (int x = 0; x < 6; ++x) ASSERT_EQ(img3[x], 1);
    ASSERT_EQ(img3[24 + 2], 0);
}

TEST_CASE(hd61830_busy_and_state) {
    Hd61830 c;
    c.setBusyTime(5);
    cmd(c, 0xA, 0x10);
    ASSERT_EQ(c.status(), 0x80);
    c.write(0, 0x20);                      // ignored while busy
    ASSERT_EQ(c.cursorAddress(), 0x0010);
    c.tick(5);
    ASSERT_EQ(c.status(), 0x00);
    c.setBusyTime(0);
    m200Setup(c);
    c.tick(100);
    c.setRam(77, 0x15);
    Hd61830 d;
    d.loadState(c.saveState());
    ASSERT_EQ(d.ram(77), 0x15);
    ASSERT_EQ(d.width(), 240);
    ASSERT_EQ(d.mode(), 0x32);
}
