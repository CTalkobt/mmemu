#include "test_harness.h"
#include "plugins/devices/hd44102/main/hd44102.h"
#include "plugins/devices/hd44102/main/m100_lcd.h"

// Power-on: display off, count up; the on/off and up/down instructions
TEST_CASE(hd44102_status_and_modes) {
    Hd44102 lcd;
    ASSERT_EQ(lcd.read(0), 0x60);          // up, off
    lcd.write(0, 0x39);
    ASSERT_EQ(lcd.read(0), 0x40);
    lcd.write(0, 0x3A);
    ASSERT_EQ(lcd.read(0), 0x00);          // down
    lcd.write(0, 0x3B);
    lcd.write(0, 0x38);
    ASSERT_EQ(lcd.read(0), 0x60);
    lcd.setResetPin(true);
    ASSERT_EQ(lcd.read(0) & 0x10, 0x10);
    lcd.write(0, 0x39);                    // ignored while /RST is held
    ASSERT(!lcd.displayOn());
    lcd.setResetPin(false);
    ASSERT_EQ(lcd.read(0), 0x60);
}

// The address: page in bits 7-6, column 0-49; writes step and wrap the column
TEST_CASE(hd44102_address_counter) {
    Hd44102 lcd;
    lcd.write(0, 0x80 | 48);               // page 2, column 48
    lcd.write(1, 0x11);
    lcd.write(1, 0x22);
    lcd.write(1, 0x33);                    // wrapped to column 0
    ASSERT_EQ(lcd.ram(2, 48), 0x11);
    ASSERT_EQ(lcd.ram(2, 49), 0x22);
    ASSERT_EQ(lcd.ram(2, 0), 0x33);
    ASSERT_EQ(lcd.page(), 2);              // the page never steps
    ASSERT_EQ(lcd.column(), 1);
    // Count down wraps 0 -> 49
    lcd.write(0, 0x3A);
    lcd.write(0, 0x40 | 1);
    lcd.write(1, 0xA1);
    lcd.write(1, 0xA0);
    lcd.write(1, 0xAF);
    ASSERT_EQ(lcd.ram(1, 1), 0xA1);
    ASSERT_EQ(lcd.ram(1, 0), 0xA0);
    ASSERT_EQ(lcd.ram(1, 49), 0xAF);
    // Y 50-63 that isn't an instruction is ignored
    lcd.write(0, 0x3B);
    lcd.write(0, 0xC0 | 5);
    lcd.write(0, 0x00 | 55);
    ASSERT_EQ(lcd.page(), 3);
    ASSERT_EQ(lcd.column(), 5);
}

// Reads go through the output latch: the first after an address is a dummy
TEST_CASE(hd44102_dummy_read) {
    Hd44102 lcd;
    for (int i = 0; i < 4; ++i) lcd.setRam(0, 10 + i, (uint8_t)(0xC0 + i));
    lcd.write(0, 10);
    (void)lcd.read(1);                     // dummy
    ASSERT_EQ(lcd.read(1), 0xC0);
    ASSERT_EQ(lcd.read(1), 0xC1);
    ASSERT_EQ(lcd.peek(1), 0xC2);          // the latch, no step
    ASSERT_EQ(lcd.read(1), 0xC2);
    ASSERT_EQ(lcd.column(), 14);
}

// The start page rolls RAM pages up the panel; display off blanks it
TEST_CASE(hd44102_start_page_and_pixels) {
    Hd44102 lcd;
    lcd.setRam(0, 3, 0x01);                // RAM row 0
    lcd.setRam(1, 3, 0x80);                // RAM row 15
    ASSERT(!lcd.pixel(3, 0));              // off
    lcd.write(0, 0x39);
    ASSERT(lcd.pixel(3, 0));
    ASSERT(lcd.pixel(3, 15));
    lcd.write(0, 0x40 | 0x3E);             // start page 1
    ASSERT_EQ(lcd.startPage(), 1);
    ASSERT(lcd.pixel(3, 7));               // RAM row 15 on display row 7
    ASSERT(lcd.pixel(3, 24));              // RAM row 0 wrapped to row 24
    ASSERT(!lcd.pixel(3, 0));
    uint8_t img[50 * 32];
    lcd.render(img);
    ASSERT_EQ(img[7 * 50 + 3], 1);
    ASSERT_EQ(img[24 * 50 + 3], 1);
    ASSERT_EQ(img[0 * 50 + 3], 0);
}

// Busy: set by each access for busyTime, then cleared by tick(); writes ignored meanwhile
TEST_CASE(hd44102_busy) {
    Hd44102 lcd;
    lcd.setBusyTime(10);
    lcd.write(0, 0x39);
    ASSERT_EQ(lcd.read(0) & 0x80, 0x80);
    lcd.write(0, 0x38);                    // ignored: busy
    ASSERT(lcd.displayOn());
    lcd.tick(9);
    ASSERT_EQ(lcd.read(0) & 0x80, 0x80);
    lcd.tick(1);
    ASSERT_EQ(lcd.read(0), 0x40);
    lcd.write(1, 0x55);
    lcd.write(1, 0x66);                    // ignored
    lcd.tick(10);
    ASSERT_EQ(lcd.ram(0, 0), 0x55);
    ASSERT_EQ(lcd.ram(0, 1), 0x00);
}

TEST_CASE(hd44102_save_state) {
    Hd44102 a;
    a.write(0, 0x39);
    a.write(0, 0x80 | 0x3E);
    a.write(0, 0x40 | 7);
    a.write(1, 0x5A);
    Hd44102 b;
    b.loadState(a.saveState());
    ASSERT_EQ(b.ram(1, 7), 0x5A);
    ASSERT_EQ(b.startPage(), 2);
    ASSERT_EQ(b.column(), 8);
    ASSERT(b.displayOn());
}

// The Model 100 panel: ten chips, 5 per row, 50 columns each (40 on the last)
TEST_CASE(m100_lcd_geometry) {
    M100Lcd lcd;
    lcd.setChipSelect(0x3FF);
    lcd.write(0, 0x39);                    // all on
    lcd.write(0, 0x3E);                    // all start page 0
    // Chip 3 (top row, x 150-199), page 1, column 5: rows 8-15 at x 155
    lcd.setChipSelect(1 << 3);
    lcd.write(0, 0x40 | 5);
    lcd.write(1, 0x81);
    ASSERT(lcd.pixel(155, 8));
    ASSERT(lcd.pixel(155, 15));
    ASSERT(!lcd.pixel(155, 9));
    // Chip 7 (bottom row, x 100-149), page 0, column 0: row 32 at x 100
    lcd.setSelectFromPorts(0x80, 0x00);
    lcd.write(0, 0x00);
    lcd.write(1, 0x01);
    ASSERT(lcd.pixel(100, 32));
    // Chip 9 via port B bit 1: the last 40 columns of the bottom row
    lcd.setSelectFromPorts(0x00, 0x02);
    ASSERT_EQ(lcd.chipSelect(), 1 << 9);
    lcd.write(0, 0xC0 | 39);
    lcd.write(1, 0x80);                    // column 39: x 239, row 63
    lcd.write(1, 0xFF);                    // column 40: not connected
    ASSERT(lcd.pixel(239, 63));
    ASSERT_EQ(M100Lcd::chipColumns(9), 40);
    ASSERT_EQ(M100Lcd::chipX(9), 200);
    ASSERT_EQ(M100Lcd::chipY(9), 32);
    const auto img = lcd.render();
    ASSERT_EQ(img.size(), (size_t)(240 * 64));
    int on = 0;
    for (uint8_t p : img) on += p;
    ASSERT_EQ(on, 2 + 1 + 1);
    ASSERT_EQ(img[63 * 240 + 239], 1);
    ASSERT_EQ(img[32 * 240 + 100], 1);
}

// Broadcast writes and OR'd reads on the shared bus; nothing selected reads 0
TEST_CASE(m100_lcd_bus) {
    M100Lcd lcd;
    ASSERT_EQ(lcd.read(0), 0);
    lcd.setChipSelect(0x003);
    lcd.write(0, 0x39);
    lcd.write(0, 0x00);
    lcd.write(1, 0xF0);
    ASSERT_EQ(lcd.chip(0).ram(0, 0), 0xF0);
    ASSERT_EQ(lcd.chip(1).ram(0, 0), 0xF0);
    ASSERT_EQ(lcd.chip(2).ram(0, 0), 0x00);
    lcd.chip(1).setRam(0, 1, 0x0F);
    lcd.setChipSelect(0x003);
    lcd.write(0, 0x01);
    (void)lcd.read(1);
    ASSERT_EQ(lcd.read(1), 0x0F);          // chip 0's 0 | chip 1's $0F
    ASSERT_EQ(lcd.read(0), 0x40);          // both on, up
    M100Lcd copy;
    copy.loadState(lcd.saveState());
    ASSERT_EQ(copy.chip(1).ram(0, 1), 0x0F);
    ASSERT_EQ(copy.chipSelect(), 0x003);
}
