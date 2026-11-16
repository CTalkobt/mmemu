// Thomson EF9345 (#357): the commands through R0-R7, the busy flag and its
// time, the pointers, the 40- and 80-column displays, attributes, the
// service row, the status, snapshots. The character ROM is a stand-in
// (the chip's is the user's): every glyph's slices are 0x0F (the left
// half lit) unless set otherwise.

#include "test_harness.h"
#include "plugins/devices/ef9345/main/ef9345.h"
#include <vector>

namespace {

constexpr double HZ = 1e6;                // ticks are microseconds

struct Chip {
    Ef9345 ef;
    explicit Chip(uint8_t slice = 0x0F) {
        ef.setCharset(std::vector<uint8_t>(Ef9345::CHARSET_SIZE, slice));
        ef.setClock(HZ);
        ef.reset();
    }
    // A command with R1 (and the others first)
    void cmd(uint8_t c, uint8_t r1 = 0) {
        while (ef.busy()) ef.advance(1);
        ef.write(1, r1);
        ef.write(8, c);
    }
    void ind(uint8_t reg, uint8_t v) { cmd((uint8_t)(0x80 | reg), v); }
    void frame() { ef.advance(64 * Ef9345::LINES); }
    // 40 columns, long codes, every area on, the service row from Y=0, rows from Y=8
    void setup40() {
        ind(1, 0x00);                     // TGS
        ind(2, 0x00);                     // MAT: black margin, no cursor
        ind(3, 0x37);                     // PAT: service row, upper and lower bulk; active area mark
        ind(4, 0x00);                     // DOR
        ind(7, 0x08);                     // ROR: origin Y=8, page 0
    }
};

}  // namespace

// IND: TGS, MAT, PAT, DOR, ROR written and read back; the busy flag and its 2 us
TEST_CASE(ef9345_indirect_registers) {
    Chip c;
    c.ind(2, 0x28);
    EXPECT_TRUE(c.ef.busy());
    EXPECT_EQ(c.ef.read(0) & 0x80, 0x80);
    c.ef.advance(2);
    EXPECT_TRUE(!c.ef.busy());
    EXPECT_EQ(c.ef.read(0) & 0x80, 0);
    c.ind(1, 0x10); c.ind(3, 0x67); c.ind(4, 0x13); c.ind(7, 0x08);
    EXPECT_EQ(c.ef.mat(), 0x28);
    EXPECT_EQ(c.ef.tgs(), 0x10);
    EXPECT_EQ(c.ef.pat(), 0x67);
    EXPECT_EQ(c.ef.dor(), 0x13);
    EXPECT_EQ(c.ef.ror(), 0x08);
    c.cmd(0x8A);                          // MAT to R1
    while (c.ef.busy()) c.ef.advance(1);
    EXPECT_EQ(c.ef.reg(1), 0x28);
    // A register written without the command bit runs nothing
    c.ef.write(1, 0x55);
    c.ef.write(0, 0x81);
    EXPECT_EQ(c.ef.tgs(), 0x10);
    EXPECT_EQ(c.ef.peek(1), 0x55);
}

// KRF: R1-R3 to C, B, A at the main pointer (planes $800 apart), X
// incremented; read back; X wraps at 40 with Al set; KRG; OCT on both pointers
TEST_CASE(ef9345_krf_and_pointers) {
    Chip c;
    c.ef.write(6, 8);                     // MP: Y=8 (address $200), X=39
    c.ef.write(7, 39);
    c.ef.write(2, 0x01);
    c.ef.write(3, 0x70);
    c.cmd(0x01, 'A');                     // KRF + increment
    EXPECT_EQ(c.ef.vram(0x0200 + 39), 'A');
    EXPECT_EQ(c.ef.vram(0x0A00 + 39), 0x01);
    EXPECT_EQ(c.ef.vram(0x1200 + 39), 0x70);
    EXPECT_EQ(c.ef.reg(7), 0);            // wrapped
    while (c.ef.busy()) c.ef.advance(1);
    EXPECT_EQ(c.ef.read(0) & 0x40, 0x40); // Al
    // Y below 8 is 0 or 1 (Y=5 is 1: $40)
    c.ef.write(6, 5);
    c.ef.write(7, 2);
    c.cmd(0x00, 'Z');
    EXPECT_EQ(c.ef.vram(0x0042), 'Z');
    // Back again
    c.ef.write(6, 8);
    c.ef.write(7, 39);
    c.cmd(0x08);
    while (c.ef.busy()) c.ef.advance(1);
    EXPECT_EQ(c.ef.reg(1), 'A');
    EXPECT_EQ(c.ef.reg(2), 0x01);
    EXPECT_EQ(c.ef.reg(3), 0x70);
    // OCT on the auxiliary pointer (R4/R5), incremented
    c.ef.write(4, 9);
    c.ef.write(5, 3);
    c.cmd(0x35, 0x99);
    EXPECT_EQ(c.ef.vram(0x0243), 0x99);
    EXPECT_EQ(c.ef.reg(5), 4);
    // OCT read, main pointer, wrapping X carries into Y
    c.ef.write(6, 9);
    c.ef.write(7, 3);
    c.cmd(0x39);
    while (c.ef.busy()) c.ef.advance(1);
    EXPECT_EQ(c.ef.reg(1), 0x99);
}

// CLF clears the 32 x 40 page (Y from 8 to 31) with R1-R3; MVB copies a row
TEST_CASE(ef9345_clear_and_move) {
    Chip c;
    c.ef.write(6, 8);
    c.ef.write(7, 0);
    c.ef.write(2, 0x00);
    c.ef.write(3, 0x20);
    c.cmd(0x05, ' ');
    EXPECT_TRUE(c.ef.busy());
    EXPECT_EQ(c.ef.vram(0x0200), ' ');
    EXPECT_EQ(c.ef.vram(0x07E7), ' ');    // Y=31, X=39
    EXPECT_EQ(c.ef.vram(0x17E7), 0x20);
    c.ef.advance(8000);                   // 5.12 ms, and 104 us for each row drawn meanwhile
    EXPECT_TRUE(!c.ef.busy());
    // MVB, main to auxiliary, stopping at the line's end: row Y=8 to Y=9
    for (int x = 0; x < 40; ++x) c.ef.setVram((uint16_t)(0x0200 + x), (uint8_t)('0' + x % 10));
    c.ef.write(6, 8); c.ef.write(7, 0);
    c.ef.write(4, 9); c.ef.write(5, 0);
    c.cmd(0xD5);
    EXPECT_EQ(c.ef.vram(0x0240), '0');
    EXPECT_EQ(c.ef.vram(0x0240 + 39), '9');
}

// The display: 40 columns, the margin, a character's lit half, colours from
// A, the service row, rows from ROR's origin; negative; the status's VSYNC
TEST_CASE(ef9345_display_40) {
    Chip c;
    c.setup40();
    c.ind(2, 0x04);                       // MAT: a blue margin
    // Row 1 (Y=8), column 0: red on black; service row (Y=0), column 1: green on black, negative
    c.ef.write(6, 8); c.ef.write(7, 0);
    c.ef.write(2, 0x00); c.ef.write(3, 0x10);
    c.cmd(0x00, 'A');
    c.ef.write(6, 0); c.ef.write(7, 1);
    c.ef.write(2, 0x00); c.ef.write(3, 0xA0);   // A7 negative, fg 2
    c.cmd(0x00, 'B');
    c.frame();
    EXPECT_EQ(c.ef.width(), Ef9345::W40);
    EXPECT_EQ(c.ef.pixel(0, 0), 4);       // the margin
    EXPECT_EQ(c.ef.pixel(4, 25), 4);
    // Row 1 is drawn at cell row 2 (the margin's row first), column 0 at cell 1
    EXPECT_EQ(c.ef.pixel(8, 20), 1);      // lit: red
    EXPECT_EQ(c.ef.pixel(8 + 4, 20), 0);  // unlit: black
    // The service row at cell row 1; negative: the lit half black, the rest green
    EXPECT_EQ(c.ef.pixel(16, 10), 0);
    EXPECT_EQ(c.ef.pixel(16 + 4, 10), 2);
    EXPECT_EQ(c.ef.rowText(1).substr(0, 1), "A");
    EXPECT_EQ(c.ef.rowText(0).substr(1, 1), "B");
    // VSYNC: set from line 0 to line 250
    c.ef.advance(64 * 5);
    EXPECT_EQ(c.ef.read(0) & 0x04, 0x04);
    c.ef.advance(64 * 250);
    EXPECT_EQ(c.ef.read(0) & 0x04, 0);
    // The upper bulk off (PAT b1): row 1 is margin
    c.ind(3, 0x35);
    c.frame();
    EXPECT_EQ(c.ef.pixel(8, 20), 4);
}

// The cursor (MAT b6-4 = 100, fixed complemented) on the main pointer;
// double height (B1) shows a character's top half over two rows
TEST_CASE(ef9345_cursor_and_double_height) {
    Chip c(0xFF);
    c.setup40();
    c.ef.write(6, 8); c.ef.write(7, 0);
    c.ef.write(2, 0x00); c.ef.write(3, 0x70);
    c.cmd(0x00, ' ');                      // white "full" glyph
    c.ind(2, 0x40);                        // the cursor: complemented
    c.ef.write(6, 8); c.ef.write(7, 0);   // on that character
    c.frame();
    EXPECT_EQ(c.ef.pixel(8, 20), 0);      // 7 ^ 7
    c.ef.write(7, 5);                     // the cursor away
    c.frame();
    EXPECT_EQ(c.ef.pixel(8, 20), 7);
    // Double height (B1) on row 2, column 3, its lower half on row 3: 'A'
    // is lit in its top five slices only
    Chip d;
    std::vector<uint8_t> rom(Ef9345::CHARSET_SIZE, 0);
    for (int i = 0; i < 5; ++i) rom[0x401 + 4 * i] = 0xFF;   // 'A': ($41 >> 2) * $40 + ($41 & 3)
    d.ef.setCharset(rom);
    d.setup40();
    d.ef.write(6, 9); d.ef.write(7, 3);
    d.ef.write(2, 0x02); d.ef.write(3, 0x70);
    d.cmd(0x00, 'A');
    d.ef.write(6, 10); d.ef.write(7, 3);
    d.ef.write(2, 0x00); d.ef.write(3, 0x70);
    d.cmd(0x00, 'A');
    d.frame();
    EXPECT_EQ(d.ef.pixel(32, 30), 7);     // the top half, stretched: lit to the cell's last line
    EXPECT_EQ(d.ef.pixel(32, 39), 7);
    EXPECT_EQ(d.ef.pixel(32, 40), 7);     // the lower half: slice 4, then unlit
    EXPECT_EQ(d.ef.pixel(32, 49), 0);
    // The same without B1: lit in lines 0-4 only
    d.ef.write(6, 9); d.ef.write(7, 3);
    d.cmd(0x00, 'A');
    d.frame();
    d.frame();
    EXPECT_EQ(d.ef.pixel(32, 34), 7);
    EXPECT_EQ(d.ef.pixel(32, 35), 0);
}

// 80 columns (TGS b7-6 = 11: 12-bit codes): 492 wide, two characters a
// cell, colours from DOR (foreground) and MAT (background)
TEST_CASE(ef9345_display_80) {
    Chip c(0x07);
    c.setup40();
    c.ind(1, 0xC0);
    c.ind(2, 0x01);                       // MAT: red background (and margin)
    c.ind(4, 0x04);                       // DOR: blue foreground, set 0
    c.ind(3, 0x37);
    c.ef.write(6, 8); c.ef.write(7, 0);
    c.ef.write(3, 0x00);
    c.cmd(0x50, 'A');                     // the left (even) character
    c.frame();
    EXPECT_EQ(c.ef.width(), Ef9345::W80);
    EXPECT_EQ(c.ef.pixel(6, 20), 4);      // cell 1, slice bits 0-2 lit: blue
    EXPECT_EQ(c.ef.pixel(6 + 4, 20), 1);  // unlit: red
    EXPECT_EQ(c.ef.pixel(0, 0), 1);       // the margin
    std::vector<uint32_t> buf((size_t)Ef9345::W80 * Ef9345::OUT_H);
    c.ef.render(buf.data());
    EXPECT_EQ(buf[(size_t)20 * Ef9345::W80 + 6], 0xFF0000FFu);
}

// Snapshots: the video RAM, the registers and the time
TEST_CASE(ef9345_snapshot) {
    Chip c;
    c.setup40();
    c.ef.write(6, 8); c.ef.write(7, 0);
    c.cmd(0x01, 'Q');
    const auto s = c.ef.saveState();
    c.cmd(0x01, 'R');
    c.ind(7, 0x10);
    c.ef.loadState(s);
    EXPECT_EQ(c.ef.vram(0x0200), 'Q');
    EXPECT_EQ(c.ef.vram(0x0201), 0);
    EXPECT_EQ(c.ef.ror(), 0x08);
    EXPECT_EQ(c.ef.reg(7), 1);
}
