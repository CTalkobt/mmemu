#include "test_harness.h"
#include "plugins/devices/ted/main/ted.h"
#include <cmath>
#include <vector>

// TED (#9, Plus/4 / C16): registers, timers, raster IRQ, video modes,
// palette, sound, keyboard latch and ROM select.

namespace {
class MockSignal : public ISignalLine {
public:
    bool get() const override { return m_level; }
    void set(bool level) override { m_level = level; }
    void pulse() override {}
    bool m_level = false;
};

constexpr uint64_t LINE = Ted::CYCLES_PER_LINE;
constexpr uint64_t PAL_FRAME = LINE * Ted::PAL_LINES;

struct T {
    Ted t;
    MockSignal irq;
    std::vector<uint8_t> ram, rom;
    explicit T(Ted::Standard s = Ted::Standard::PAL) : t(s), ram(65536, 0), rom(65536, 0) {
        t.setIrqLine(&irq);
        t.setFetch([this](uint16_t a, bool fromRom) { return fromRom ? rom[a] : ram[a]; });
        t.reset();
    }
    void w(int reg, uint8_t v) { t.ioWrite(nullptr, 0xFF00 + reg, v); }
    uint8_t r(int reg) { uint8_t v = 0; t.ioRead(nullptr, 0xFF00 + reg, &v); return v; }
    uint8_t p(int reg) { uint8_t v = 0; t.ioPeek(nullptr, 0xFF00 + reg, &v); return v; }

    /** A KERNAL-like text screen: attributes $0800, screen $0C00, characters in RAM at $2000. */
    void textSetup() {
        w(0x06, 0x1B);   // DEN, 25 rows, Y scroll 3
        w(0x07, 0x08);   // 40 columns, X scroll 0
        w(0x12, 0x00);   // characters / bitmap from RAM
        w(0x13, 0x20);   // character base $2000
        w(0x14, 0x08);   // attributes $0800, screen $0C00
        w(0x15, 0x11);   // background
        w(0x19, 0x32);   // border
        w(0x0C, 0x03); w(0x0D, 0xFF);   // cursor off screen
    }
    std::vector<uint32_t> frame(int frames = 3) {
        t.tick(PAL_FRAME * (uint64_t)frames);
        std::vector<uint32_t> buf((size_t)Ted::FRAME_WIDTH * t.frameHeight());
        t.renderFrame(buf.data());
        return buf;
    }
    uint32_t px(const std::vector<uint32_t>& f, int x, int y) { return f[(size_t)y * Ted::FRAME_WIDTH + x]; }
    uint32_t col(uint8_t c) { return t.paletteColor(c); }
};

// Frame row of PAL raster line 4 (the first line of the 25-row display):
// tv line (4 - 257 + 312) % 312 = 59, minus the first displayed tv line 19.
constexpr int ROW0 = 40;
constexpr int COL0 = Ted::LEFT_BORDER;
}  // namespace

TEST_CASE(ted_unused_bits_read_back) {
    T t;
    t.w(0x0A, 0x00);
    ASSERT_EQ((int)t.r(0x0A), 0xA0);
    t.w(0x0A, 0xFF);
    ASSERT_EQ((int)t.r(0x0A), 0xFF);        // stored & 0x5F, | 0xA0
    t.w(0x0A, 0x00);
    ASSERT_EQ((int)t.r(0x09), 0x25);        // bits 0, 2, 5 read 1
    t.w(0x0C, 0x00);
    ASSERT_EQ((int)t.r(0x0C), 0xFC);
    t.w(0x14, 0x00);
    ASSERT_EQ((int)t.r(0x14), 0x07);
    for (int r = 0x15; r <= 0x19; ++r) {
        t.w(r, 0x00);
        ASSERT_EQ((int)t.r(r), 0x80);
        t.w(r, 0xFF);
        ASSERT_EQ((int)t.r(r), 0xFF);
    }
    t.w(0x12, 0x00);
    ASSERT_EQ((int)t.r(0x12), 0xC0);
    t.w(0x12, 0xFF);
    ASSERT_EQ((int)t.r(0x12), 0xFF);        // bits 2-5 stored, 0-1 voice 1 high, 6-7 read 1
    t.w(0x10, 0x00);
    ASSERT_EQ((int)t.r(0x10), 0x7C);
    t.w(0x10, 0xFF);
    ASSERT_EQ((int)t.r(0x10), 0x7F);
    ASSERT_EQ((int)(t.r(0x1C) & 0xFE), 0xFE);
    ASSERT_EQ((int)(t.r(0x1A) & 0xFC), 0xFC);
    ASSERT_EQ((int)(t.r(0x1F) & 0x80), 0x80);
    // $FF13 bit 0 is the ROM status: only $FF3E/$FF3F change it
    ASSERT_EQ((int)(t.r(0x13) & 1), 1);
    t.w(0x13, 0x00);
    ASSERT_EQ((int)(t.r(0x13) & 1), 1);
    // peek returns what a read does
    for (int r = 0; r < 0x20; ++r) ASSERT_EQ((int)t.p(r), (int)t.r(r));
}

TEST_CASE(ted_ff20_ff3f_not_registers) {
    T t;
    uint8_t v = 0;
    ASSERT(!t.t.ioRead(nullptr, 0xFF20, &v));
    ASSERT(!t.t.ioRead(nullptr, 0xFF3D, &v));
    ASSERT(!t.t.ioWrite(nullptr, 0xFF30, 0x12));
    // ROM selected: $FF3E/$FF3F read the ROM underneath
    ASSERT(!t.t.ioRead(nullptr, 0xFF3E, &v));
    ASSERT(t.t.ioWrite(nullptr, 0xFF3F, 0));      // RAM on
    ASSERT(t.t.ioRead(nullptr, 0xFF3E, &v));
    ASSERT_EQ((int)v, 0xFF);
    ASSERT(t.t.ioRead(nullptr, 0xFF3F, &v));
    ASSERT_EQ((int)v, 0xFF);
}

TEST_CASE(ted_rom_select_callback) {
    T t;
    std::vector<int> calls;
    t.t.setRomSelect([&](bool on) { calls.push_back(on ? 1 : 0); });
    ASSERT(t.t.romSelected());
    t.w(0x3F, 0x55);
    ASSERT(!t.t.romSelected());
    ASSERT_EQ((int)(t.r(0x13) & 1), 0);
    t.w(0x3E, 0x00);
    ASSERT(t.t.romSelected());
    ASSERT_EQ((int)(t.r(0x13) & 1), 1);
    ASSERT_EQ((int)calls.size(), 2);
    ASSERT_EQ(calls[0], 0);
    ASSERT_EQ(calls[1], 1);
    t.t.reset();                                 // reset writes $FF3E: ROM on
    ASSERT_EQ((int)calls.size(), 3);
    ASSERT_EQ(calls[2], 1);
}

TEST_CASE(ted_keyboard_latch) {
    T t;
    std::vector<int> args;
    t.t.setKeyboardRead([&](uint8_t v) { args.push_back(v); return (uint8_t)(0xA0 | (v & 0x0F)); });
    t.w(0x08, 0xFB);                             // joystick 1 selected (bit 2 low)
    ASSERT_EQ((int)args.size(), 1);
    ASSERT_EQ(args[0], 0xFB);
    ASSERT_EQ((int)t.r(0x08), 0xAB);
    ASSERT_EQ((int)t.r(0x08), 0xAB);             // reads return the latch
    ASSERT_EQ((int)args.size(), 1);
    t.w(0x08, 0xFF);
    ASSERT_EQ(args[1], 0xFF);
    ASSERT_EQ((int)t.r(0x08), 0xAF);
}

TEST_CASE(ted_timer1_reload_and_irq) {
    T t;
    t.w(0x00, 0x10);                             // low: stops, value = latch
    ASSERT(!t.t.timerRunning(0));
    ASSERT_EQ((int)t.r(0x00), 0x10);
    t.t.tick(100);
    ASSERT_EQ((int)t.r(0x00), 0x10);             // stopped
    t.w(0x01, 0x00);                             // high: starts
    ASSERT(t.t.timerRunning(0));
    t.t.tick(2 * 0x10 - 2);                      // one count per two double-clock cycles
    ASSERT_EQ((int)t.r(0x00), 0x01);
    ASSERT_EQ((int)(t.r(0x09) & 0x08), 0);
    t.t.tick(2);
    ASSERT_EQ((int)(t.r(0x09) & 0x08), 0x08);   // underflow flag
    ASSERT_EQ((int)t.r(0x00), 0x10);             // reloaded from the latch
    ASSERT_EQ((int)t.r(0x01), 0x00);
    ASSERT(!t.irq.get());                        // not enabled
    ASSERT_EQ((int)(t.r(0x09) & 0x80), 0);
    t.w(0x0A, 0x08);
    ASSERT(t.irq.get());
    ASSERT_EQ((int)(t.r(0x09) & 0x88), 0x88);
    t.w(0x09, 0x08);                             // acknowledge
    ASSERT(!t.irq.get());
    ASSERT_EQ((int)(t.r(0x09) & 0x88), 0);
    t.t.tick(2 * 0x10);
    ASSERT(t.irq.get());                         // fires again every $10 cycles
}

TEST_CASE(ted_timer2_timer3_free_run) {
    T t;
    t.w(0x0A, 0x50);                             // T2 + T3 IRQs
    t.w(0x02, 0x05); t.w(0x03, 0x00);
    t.w(0x04, 0x00); t.w(0x05, 0x01);            // T3 = $0100
    ASSERT_EQ((int)t.r(0x02), 0x05);
    t.t.tick(10);
    ASSERT_EQ((int)(t.r(0x09) & 0x10), 0x10);
    ASSERT(t.irq.get());
    ASSERT_EQ((int)t.r(0x02), 0x00);             // runs on from $10000
    ASSERT_EQ((int)t.r(0x03), 0x00);
    t.t.tick(2);
    ASSERT_EQ((int)t.r(0x02), 0xFF);
    ASSERT_EQ((int)t.r(0x03), 0xFF);
    t.w(0x09, 0x10);
    ASSERT(!t.irq.get());
    ASSERT_EQ((int)(t.r(0x09) & 0x40), 0);
    t.t.tick(2 * 0x100 - 12);
    ASSERT_EQ((int)(t.r(0x09) & 0x40), 0x40);   // timer 3
    ASSERT(t.irq.get());
    t.w(0x09, 0x40);
    ASSERT(!t.irq.get());
    // timer 2 next underflows after a full $10000 count
    t.t.tick(2 * 0x10000 - 2 * 0x100 - 2);
    ASSERT_EQ((int)(t.r(0x09) & 0x10), 0);
    t.t.tick(2 * 0x100 + 4);
    ASSERT_EQ((int)(t.r(0x09) & 0x10), 0x10);
}

TEST_CASE(ted_raster_compare_irq) {
    T t;
    ASSERT_EQ((int)t.t.rasterLine(), 0);         // reset writes $FF1C/$FF1D = 0
    t.w(0x0B, 100);
    t.w(0x0A, 0x02);
    t.t.tick(LINE * 100 - 1);
    ASSERT_EQ((int)t.r(0x1D), 99);
    ASSERT(!t.irq.get());
    ASSERT_EQ((int)(t.r(0x09) & 0x02), 0);
    t.t.tick(1);
    ASSERT_EQ((int)t.r(0x1D), 100);
    ASSERT(t.irq.get());
    ASSERT_EQ((int)(t.r(0x09) & 0x82), 0x82);
    t.w(0x09, 0x02);
    ASSERT(!t.irq.get());
    // line 261 needs bit 8 in $FF0A bit 0
    t.w(0x0B, 0x05);
    t.w(0x0A, 0x03);
    ASSERT_EQ((int)t.t.rasterCompare(), 0x105);
    t.t.tick(LINE * (0x105 - 100) - 1);
    ASSERT(!t.irq.get());
    t.t.tick(1);
    ASSERT_EQ((int)t.r(0x1C) & 1, 1);
    ASSERT_EQ((int)t.r(0x1D), 0x05);
    ASSERT(t.irq.get());
}

TEST_CASE(ted_raster_compare_written_on_current_line) {
    T t;
    t.w(0x0A, 0x02);
    t.t.tick(LINE * 50 + 20);
    ASSERT_EQ((int)t.t.rasterLine(), 50);
    ASSERT(!t.irq.get());
    t.w(0x0B, 50);                               // compare = the current line: immediate
    ASSERT(t.irq.get());
}

TEST_CASE(ted_lines_per_frame_pal_ntsc) {
    T pal;
    pal.t.tick(LINE * 311);
    ASSERT_EQ((int)pal.t.rasterLine(), 311);
    pal.t.tick(LINE);
    ASSERT_EQ((int)pal.t.rasterLine(), 0);
    T ntsc(Ted::Standard::NTSC);
    ntsc.t.tick(LINE * 261);
    ASSERT_EQ((int)ntsc.t.rasterLine(), 261);
    ntsc.t.tick(LINE);
    ASSERT_EQ((int)ntsc.t.rasterLine(), 0);
    ASSERT_EQ(ntsc.t.getDimensions().height, Ted::NTSC_FRAME_HEIGHT);
    ASSERT_EQ(pal.t.getDimensions().height, 288);
    ASSERT_EQ(pal.t.getDimensions().width, 384);
    ASSERT_EQ(Ted::clockHz(Ted::Standard::PAL), 1773447u);
    ASSERT_EQ(Ted::clockHz(Ted::Standard::NTSC), 1789772u);
}

TEST_CASE(ted_horizontal_position_read) {
    T t;
    t.t.tick(16);
    ASSERT_EQ((int)t.r(0x1E), 0);                // VICE: ((cycle - 16) * 4 / 2) & $FE
    t.t.tick(10);
    ASSERT_EQ((int)t.r(0x1E), 20);
}

TEST_CASE(ted_palette_matches_vice) {
    T t;
    ASSERT_EQ(t.col(0x00), 0xFF000000u);         // black
    ASSERT_EQ(t.col(0x70), 0xFF000000u);         // hue 0 is black at any luminance
    ASSERT_EQ(t.col(0x71), 0xFFFFFFFFu);         // white
    ASSERT_EQ(t.col(0x11), 0xFF303030u);         // grey, luminance 1 (48)
    ASSERT_EQ(t.col(0x32), 0xFF8E3730u);         // red, luminance 3
    ASSERT_EQ(t.col(0x46), 0xFF6D62FFu);         // blue, luminance 4
    ASSERT_EQ(t.col(0x57), 0xFF9BA602u);         // yellow, luminance 5
    ASSERT_EQ(t.col(0xF2), t.col(0x72));         // bit 7 ignored
}

TEST_CASE(ted_text_mode_renders_character) {
    T t;
    t.textSetup();
    t.ram[0x0C00] = 0x01;                        // char 1 at row 0, column 0
    t.ram[0x0800] = 0x71;                        // white
    for (int y = 0; y < 8; ++y) t.ram[0x2008 + y] = 0x81;
    t.ram[0x0C01] = 0x81;                        // reversed char 1 at column 1
    t.ram[0x0801] = 0x32;
    auto f = t.frame();
    const uint32_t bg = t.col(0x11), border = t.col(0x32), fg = t.col(0x71);
    ASSERT_EQ(t.px(f, COL0, ROW0), fg);
    ASSERT_EQ(t.px(f, COL0 + 1, ROW0), bg);
    ASSERT_EQ(t.px(f, COL0 + 7, ROW0 + 7), fg);
    ASSERT_EQ(t.px(f, COL0 - 1, ROW0), border);  // left border
    ASSERT_EQ(t.px(f, COL0, ROW0 - 1), border);  // top border
    ASSERT_EQ(t.px(f, COL0 + 8, ROW0), bg);      // reversed: pixels 0/7 background, the rest red
    ASSERT_EQ(t.px(f, COL0 + 9, ROW0), t.col(0x32));
    ASSERT_EQ(t.px(f, COL0 + 16, ROW0), bg);     // char 0 (blank) elsewhere
    ASSERT_EQ(t.px(f, COL0, ROW0 + 8), bg);      // row 1
    ASSERT_EQ(t.px(f, COL0, ROW0 + 199), bg);    // last display line
    ASSERT_EQ(t.px(f, COL0, ROW0 + 200), border);
    ASSERT_EQ(t.px(f, COL0 + 320, ROW0), border);
}

TEST_CASE(ted_text_mode_rows_columns_and_scroll) {
    T t;
    t.textSetup();
    t.ram[0x0C00 + 40] = 0x01;                   // row 1, column 0
    t.ram[0x0800 + 40] = 0x71;
    for (int y = 0; y < 8; ++y) t.ram[0x2008 + y] = 0x80;
    auto f = t.frame();
    ASSERT_EQ(t.px(f, COL0, ROW0 + 8), t.col(0x71));
    ASSERT_EQ(t.px(f, COL0, ROW0 + 7), t.col(0x11));
    // X scroll 2: the pixel moves right, the gap shows the background
    t.w(0x07, 0x0A);
    f = t.frame();
    ASSERT_EQ(t.px(f, COL0 + 2, ROW0 + 8), t.col(0x71));
    ASSERT_EQ(t.px(f, COL0, ROW0 + 8), t.col(0x11));
    // 38 columns: 7 more border pixels on the left
    t.w(0x07, 0x00);
    f = t.frame();
    ASSERT_EQ(t.px(f, COL0 + 6, ROW0 + 8), t.col(0x32));
    ASSERT_EQ(t.px(f, COL0 + 7, ROW0 + 8), t.col(0x11));
    ASSERT_EQ(t.px(f, COL0 + 311, ROW0 + 8), t.col(0x32));
    // 24 rows: 4 border lines more at the top and bottom
    t.w(0x07, 0x08);
    t.w(0x06, 0x13);
    f = t.frame();
    ASSERT_EQ(t.px(f, COL0, ROW0 + 3), t.col(0x32));
    ASSERT_EQ(t.px(f, COL0, ROW0 + 4), t.col(0x11));
    ASSERT_EQ(t.px(f, COL0, ROW0 + 195), t.col(0x11));
    ASSERT_EQ(t.px(f, COL0, ROW0 + 196), t.col(0x32));
    // display off: all border
    t.w(0x06, 0x0B);
    f = t.frame();
    ASSERT_EQ(t.px(f, COL0, ROW0 + 8), t.col(0x32));
}

TEST_CASE(ted_text_mode_flash_and_cursor) {
    T t;
    t.textSetup();
    t.ram[0x0C00] = 0x01;
    t.ram[0x0800] = 0xF1;                        // flashing white
    for (int y = 0; y < 8; ++y) t.ram[0x2008 + y] = 0xFF;
    // the flash counter: visible 16 frames, hidden 16
    int shown = 0, hidden = 0;
    for (int i = 0; i < 32; ++i) {
        auto f = t.frame(1);
        if (t.px(f, COL0, ROW0) == t.col(0x71)) ++shown; else ++hidden;
    }
    ASSERT_EQ(shown, 16);
    ASSERT_EQ(hidden, 16);
    // hardware cursor at position 2 inverts that cell while visible
    t.ram[0x0800] = 0x71;
    t.ram[0x0802] = 0x46;
    t.w(0x0C, 0x00); t.w(0x0D, 0x02);
    int inverted = 0;
    for (int i = 0; i < 32; ++i) {
        auto f = t.frame(1);
        if (t.px(f, COL0 + 16, ROW0) == t.col(0x46)) ++inverted;
    }
    ASSERT_EQ(inverted, 16);
}

TEST_CASE(ted_multicolour_text) {
    T t;
    t.textSetup();
    t.w(0x07, 0x18);                             // MCM
    t.w(0x16, 0x23);
    t.w(0x17, 0x45);
    t.ram[0x0C00] = 0x01;
    t.ram[0x0800] = 0x5E;                        // bit 3: multicolour; colour = $5E & $77 = $56
    t.ram[0x0C01] = 0x01;
    t.ram[0x0801] = 0x57;                        // bit 3 clear: hires in colour $57
    for (int y = 0; y < 8; ++y) t.ram[0x2008 + y] = 0x1B;   // 00 01 10 11
    auto f = t.frame();
    ASSERT_EQ(t.px(f, COL0 + 0, ROW0), t.col(0x11));
    ASSERT_EQ(t.px(f, COL0 + 1, ROW0), t.col(0x11));
    ASSERT_EQ(t.px(f, COL0 + 2, ROW0), t.col(0x23));
    ASSERT_EQ(t.px(f, COL0 + 3, ROW0), t.col(0x23));
    ASSERT_EQ(t.px(f, COL0 + 4, ROW0), t.col(0x45));
    ASSERT_EQ(t.px(f, COL0 + 6, ROW0), t.col(0x56));
    ASSERT_EQ(t.px(f, COL0 + 7, ROW0), t.col(0x56));
    // hires cell: $1B = 00011011
    ASSERT_EQ(t.px(f, COL0 + 8 + 2, ROW0), t.col(0x11));
    ASSERT_EQ(t.px(f, COL0 + 8 + 3, ROW0), t.col(0x57));
    ASSERT_EQ(t.px(f, COL0 + 8 + 5, ROW0), t.col(0x11));
    ASSERT_EQ(t.px(f, COL0 + 8 + 7, ROW0), t.col(0x57));
}

TEST_CASE(ted_extended_colour_text) {
    T t;
    t.textSetup();
    t.w(0x06, 0x5B);                             // ECM
    t.w(0x16, 0x23); t.w(0x17, 0x45); t.w(0x18, 0x67);
    t.ram[0x0C00] = 0x81;                        // char 1, background 2 ($FF17)
    t.ram[0x0800] = 0x71;
    t.ram[0x0C01] = 0xC1;                        // char 1, background 3 ($FF18)
    t.ram[0x0801] = 0x71;
    for (int y = 0; y < 8; ++y) t.ram[0x2008 + y] = 0x80;
    auto f = t.frame();
    ASSERT_EQ(t.px(f, COL0, ROW0), t.col(0x71));
    ASSERT_EQ(t.px(f, COL0 + 1, ROW0), t.col(0x45));
    ASSERT_EQ(t.px(f, COL0 + 9, ROW0), t.col(0x67));
    ASSERT_EQ(t.px(f, COL0 + 17, ROW0), t.col(0x11));   // char 0, background 0
}

TEST_CASE(ted_hires_bitmap) {
    T t;
    t.textSetup();
    t.w(0x06, 0x3B);                             // BMM
    t.w(0x12, 0x08);                             // bitmap at $2000
    t.ram[0x2000] = 0xF0;                        // row 0, column 0, line 0
    t.ram[0x2001] = 0x0F;                        // line 1
    t.ram[0x2000 + 320] = 0x80;                  // row 1, column 0
    t.ram[0x0C00] = 0x52;                        // foreground hue 5, background hue 2
    t.ram[0x0800] = 0x63;                        // foreground luminance 3, background luminance 6
    auto f = t.frame();
    const uint32_t fg = t.col(0x35), bg = t.col(0x62);
    ASSERT_EQ(t.px(f, COL0 + 0, ROW0), fg);
    ASSERT_EQ(t.px(f, COL0 + 3, ROW0), fg);
    ASSERT_EQ(t.px(f, COL0 + 4, ROW0), bg);
    ASSERT_EQ(t.px(f, COL0 + 0, ROW0 + 1), bg);
    ASSERT_EQ(t.px(f, COL0 + 7, ROW0 + 1), fg);
    // row 1 uses screen/attribute 40: both 0 -> black
    ASSERT_EQ(t.px(f, COL0, ROW0 + 8), 0xFF000000u);
}

TEST_CASE(ted_multicolour_bitmap) {
    T t;
    t.textSetup();
    t.w(0x06, 0x3B);
    t.w(0x07, 0x18);
    t.w(0x12, 0x08);
    t.w(0x16, 0x4D);
    t.ram[0x2000] = 0x1B;                        // 00 01 10 11
    t.ram[0x0C00] = 0x52;
    t.ram[0x0800] = 0x63;
    auto f = t.frame();
    ASSERT_EQ(t.px(f, COL0 + 0, ROW0), t.col(0x11));                 // 00: $FF15
    ASSERT_EQ(t.px(f, COL0 + 2, ROW0), t.col((0x52 >> 4) + (3 << 4))); // 01: hue = screen hi, lum = attr lo
    ASSERT_EQ(t.px(f, COL0 + 4, ROW0), t.col(0x02 + 0x60));          // 10: hue = screen lo, lum = attr hi
    ASSERT_EQ(t.px(f, COL0 + 6, ROW0), t.col(0x4D));                 // 11: $FF16
    ASSERT_EQ(t.px(f, COL0 + 7, ROW0), t.col(0x4D));
}

TEST_CASE(ted_character_rom_fetch) {
    T t;
    t.textSetup();
    std::vector<int> romAddrs;
    t.t.setFetch([&](uint16_t a, bool fromRom) {
        if (fromRom) { romAddrs.push_back(a); return t.rom[a]; }
        return t.ram[a];
    });
    t.w(0x12, 0x04);                             // characters from ROM
    t.w(0x13, 0xD0);                             // $D000
    t.ram[0x0C00] = 0x01;
    t.ram[0x0800] = 0x71;
    t.rom[0xD008] = 0x80;
    t.ram[0xD008] = 0x00;
    auto f = t.frame(1);
    f = t.frame(1);
    ASSERT_EQ(t.px(f, COL0, ROW0), t.col(0x71));
    ASSERT(!romAddrs.empty());
    for (int a : romAddrs) ASSERT(a >= 0xD000 && a < 0xD400);
    // ROM below $8000 is open space: nothing drawn, fetch not asked
    romAddrs.clear();
    t.w(0x13, 0x20);
    f = t.frame(2);
    ASSERT(romAddrs.empty());
    ASSERT_EQ(t.px(f, COL0, ROW0), t.col(0x11));
}

TEST_CASE(ted_single_clock_and_dma) {
    T t;
    t.textSetup();
    t.t.tick(PAL_FRAME);                         // display on from line 0
    while (t.t.rasterLine() != 10) t.t.tick(1);
    ASSERT(t.t.fastMode());
    ASSERT(!t.t.cpuSingleClock());               // cycle 0
    t.t.tick(50);
    ASSERT(t.t.cpuSingleClock());                // character fetch window
    while (t.t.rasterLine() != 0xD0) t.t.tick(1);
    t.t.tick(50);
    ASSERT(!t.t.cpuSingleClock());               // below the display
    t.t.tick(45);
    ASSERT(t.t.cpuSingleClock());                // refresh cycles 91-100
    t.w(0x13, 0x22);                             // force single clock
    ASSERT(!t.t.fastMode());
    t.t.tick(10);
    ASSERT(t.t.cpuSingleClock());
    t.w(0x13, 0x20);
    // the scheduler pays DMA cycles back as it goes
    auto run = [&](uint64_t n) {
        for (uint64_t i = 0; i < n; ++i) { if (t.t.cpuBlocked()) t.t.consumeBlockedCycle(); t.t.tick(1); }
    };
    auto stolenFrom = [&](int line) {
        while (t.t.rasterLine() != line) run(1);
        while (t.t.cpuBlocked()) run(1);
        int before = t.t.cpuBlocked() ? 1 : 0;
        t.t.tick(4);
        int n = 0;
        while (t.t.cpuBlocked()) { t.t.consumeBlockedCycle(); t.t.tick(1); ++n; }
        return before ? -1 : n;
    };
    // attribute line (11 & 7 == Y scroll 3) and bad line (12 & 7 == Y scroll + 1):
    // TED takes (40 + 3) * 2 double-clock cycles from cycle 4 of each
    ASSERT_EQ(stolenFrom(11), Ted::DMA_STEAL);
    ASSERT_EQ(stolenFrom(12), Ted::DMA_STEAL);
    // an ordinary display line: no DMA
    ASSERT_EQ(stolenFrom(13), 0);
    // display off: no DMA in the next frame
    t.w(0x06, 0x0B);
    run(PAL_FRAME);
    ASSERT_EQ(stolenFrom(12), 0);
}

TEST_CASE(ted_sound_voice_and_volume) {
    T t;
    t.t.pullSamples(nullptr, 0);
    t.w(0x0E, 0x00);
    t.w(0x12, 0x03);                             // voice 1 frequency $300
    t.w(0x11, 0x18);                             // voice 1 on, volume 8
    t.t.tick(Ted::clockHz(Ted::Standard::PAL) / 10);
    std::vector<float> buf(8192);
    int n = t.t.pullSamples(buf.data(), (int)buf.size());
    ASSERT(n > 4000 && n < 4500);                // ~4410 samples at 44100 Hz
    int nonzero = 0, zero = 0;
    for (int i = 0; i < n; ++i) (buf[i] != 0.0f ? nonzero : zero)++;
    ASSERT(nonzero > n / 4);
    ASSERT(zero > n / 4);                         // a square wave
    float peak = 0;
    for (int i = 0; i < n; ++i) peak = std::max(peak, buf[i]);
    ASSERT(std::fabs(peak - 0x1E4A / 32768.0f) < 1e-6f);

    t.w(0x11, 0x10);                             // volume 0
    t.t.tick(Ted::clockHz(Ted::Standard::PAL) / 20);
    n = t.t.pullSamples(buf.data(), (int)buf.size());
    ASSERT(n > 0);
    for (int i = 1; i < n; ++i) ASSERT_EQ(buf[i], 0.0f);

    t.w(0x11, 0x08);                             // both voices off
    t.t.tick(Ted::clockHz(Ted::Standard::PAL) / 20);
    n = t.t.pullSamples(buf.data(), (int)buf.size());
    for (int i = 1; i < n; ++i) ASSERT_EQ(buf[i], 0.0f);
}

TEST_CASE(ted_sound_noise_and_da) {
    T t;
    t.w(0x0F, 0x80);
    t.w(0x10, 0x03);                             // voice 2 frequency $380
    t.w(0x11, 0x48);                             // noise on voice 2, volume 8
    t.t.tick(Ted::clockHz(Ted::Standard::PAL) / 10);
    std::vector<float> buf(8192);
    int n = t.t.pullSamples(buf.data(), (int)buf.size());
    int nonzero = 0;
    for (int i = 0; i < n; ++i) if (buf[i] != 0.0f) ++nonzero;
    ASSERT(nonzero > 0 && nonzero < n);
    ASSERT_EQ((int)t.r(0x0F), 0x80);
    ASSERT_EQ((int)t.r(0x11), 0x48);
    ASSERT_EQ((int)t.t.voiceFreq(1), 0x380);

    t.w(0x11, 0xA8);                             // DA mode, voice 2 on: constant level
    t.t.tick(Ted::clockHz(Ted::Standard::PAL) / 50);
    n = t.t.pullSamples(buf.data(), (int)buf.size());
    ASSERT(n > 0);
    for (int i = 0; i < n; ++i) ASSERT(std::fabs(buf[i] - 0x1E4A / 32768.0f) < 1e-6f);
}

TEST_CASE(ted_device_info) {
    T t;
    DeviceInfo info;
    t.t.getDeviceInfo(info);
    ASSERT_EQ(info.baseAddr, 0xFF00u);
    ASSERT_EQ(info.addrMask, 0x3Fu);
    ASSERT((int)info.registers.size() >= 0x20);
    ASSERT(!info.state.empty());
    ASSERT_EQ(t.t.deviceAliases()[0], std::string("TED"));
}
