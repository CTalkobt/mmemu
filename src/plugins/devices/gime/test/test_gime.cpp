#include "test_harness.h"
#include "plugins/devices/gime/main/gime.h"
#include <vector>

namespace {

constexpr uint64_t FRAME = 263ull * Gime::LINE_TICKS;

struct Rig {
    std::vector<uint8_t> ram;
    Gime g;
    explicit Rig(int kb = 128, Gime::Revision rev = Gime::Revision::G1987) : ram((size_t)kb * 1024, 0), g(rev) {
        g.setRamKb(kb);
        g.setRam(ram.data());
    }
    uint8_t& at(uint32_t phys) { return ram[phys & g.ramMask()]; }
    /** Two frames: the mode is latched at a frame's start, then a whole frame drawn. */
    void frames(int n = 2) { g.advance(FRAME * (uint64_t)n); }
    void setY(uint32_t addr) { g.write(0xFF9D, (uint8_t)(addr >> 11)); g.write(0xFF9E, (uint8_t)(addr >> 3)); }
};

uint8_t rd(Gime& g, uint16_t a) {
    uint8_t v = 0;
    g.read(a, v);
    return v;
}

}  // namespace

// MMU off: the CPU's 64K is pages $38-$3F; on, the task's page registers
TEST_CASE(gime_mmu_mapping) {
    Rig r;
    ASSERT_EQ(r.g.decode(0x0000).s, Gime::S_RAM);
    ASSERT_EQ(r.g.decode(0x0000).phys, (0x38u << 13) & 0x1FFFF);
    ASSERT_EQ(r.g.decode(0x2345).phys, ((0x39u << 13) | 0x0345) & 0x1FFFF);
    r.g.write(0xFFA0, 0x05);
    ASSERT_EQ(r.g.decode(0x0123).phys, ((0x38u << 13) | 0x123) & 0x1FFFF);   // MMU still off
    r.g.write(0xFF90, 0x40);                                                // MMUEN
    ASSERT_EQ(r.g.decode(0x0123).phys, (0x05u << 13) | 0x123);
    // Task 1
    r.g.write(0xFFA8, 0x07);
    r.g.write(0xFF91, 0x01);
    ASSERT_EQ(r.g.task(), 1);
    ASSERT_EQ(r.g.decode(0x0010).phys, (0x07u << 13) | 0x10);
    r.g.write(0xFF91, 0x00);
    ASSERT_EQ(r.g.decode(0x0010).phys, (0x05u << 13) | 0x10);
    // The page registers keep six bits and read back
    r.g.write(0xFFA3, 0xFF);
    ASSERT_EQ(rd(r.g, 0xFFA3), 0x3F);
    ASSERT_EQ(r.g.mmu(3), 0x3F);
}

// 128K: pages $00 and $30 are the same RAM; 512K: not
TEST_CASE(gime_ram_size_masking) {
    Rig small(128);
    small.g.write(0xFF90, 0x40);
    small.g.write(0xFFA0, 0x02);
    small.g.write(0xFFA1, 0x32);
    ASSERT_EQ(small.g.decode(0x0000).phys, small.g.decode(0x2000).phys);
    Rig big(512);
    big.g.write(0xFF90, 0x40);
    big.g.write(0xFFA0, 0x02);
    big.g.write(0xFFA1, 0x32);
    ASSERT_EQ(big.g.decode(0x0000).phys, 0x02u << 13);
    ASSERT_EQ(big.g.decode(0x2000).phys, 0x32u << 13);
}

// MC3: $FE00-$FEFF is page $3F whatever slot 7 maps; $FF00 up is never RAM
TEST_CASE(gime_constant_page) {
    Rig r(512);
    r.g.write(0xFF90, 0x40);
    r.g.write(0xFFA7, 0x10);
    ASSERT_EQ(r.g.decode(0xFE10).phys, (0x10u << 13) | 0x1E10);
    ASSERT_EQ(r.g.decode(0xE000).phys, 0x10u << 13);
    r.g.write(0xFF90, 0x48);                   // + MC3
    ASSERT_EQ(r.g.decode(0xFE10).phys, (0x3Fu << 13) | 0x1E10);
    ASSERT_EQ(r.g.decode(0xFDFF).phys, (0x10u << 13) | 0x1DFF);
    // Constant even with the ROM mapped there (page $3F)
    r.g.write(0xFFA7, 0x3F);
    ASSERT_EQ(r.g.decode(0xFE10).s, Gime::S_RAM);
    ASSERT_EQ(r.g.decode(0xFD00).s, Gime::S_CTS);
    ASSERT_EQ(r.g.decode(0xFF00).s, Gime::S_PIA0);
}

// MC1/MC0 and TY: 16K internal + 16K external, 32K internal, 32K external;
// the vectors always the internal ROM
TEST_CASE(gime_rom_modes) {
    Rig r;
    ASSERT_EQ(r.g.decode(0x8000).s, Gime::S_ROM);
    ASSERT_EQ(r.g.decode(0xBFFF).s, Gime::S_ROM);
    ASSERT_EQ(r.g.decode(0xC000).s, Gime::S_CTS);
    ASSERT_EQ(r.g.decode(0xFEFF).s, Gime::S_CTS);
    ASSERT_EQ(r.g.decode(0x7FFF).s, Gime::S_RAM);
    r.g.write(0xFF90, 0x02);                   // 32K internal
    ASSERT_EQ(r.g.decode(0xC000).s, Gime::S_ROM);
    r.g.write(0xFF90, 0x03);                   // 32K external
    ASSERT_EQ(r.g.decode(0x8000).s, Gime::S_CTS);
    ASSERT_EQ(r.g.decode(0xFFFE).s, Gime::S_ROM);
    // TY ($FFDF): all RAM
    r.g.write(0xFFDF, 0);
    ASSERT_EQ(r.g.decode(0x8000).s, Gime::S_RAM);
    ASSERT_EQ(r.g.decode(0xC000).s, Gime::S_RAM);
    ASSERT_EQ(r.g.decode(0xFFFE).s, Gime::S_ROM);
    r.g.write(0xFFDE, 0);
    ASSERT_EQ(r.g.decode(0x8000).s, Gime::S_CTS);
    // The ROM pages mapped low by the MMU are ROM there too
    r.g.write(0xFF90, 0x40);
    r.g.write(0xFFA0, 0x3C);
    ASSERT_EQ(r.g.decode(0x0000).s, Gime::S_ROM);
    // I/O: the PIAs mirror in $FF00-$FF0F / $FF20-$FF2F; SCS needs MC2 for $FF40-$FF4F
    ASSERT_EQ(r.g.decode(0xFF0E).s, Gime::S_PIA0);
    ASSERT_EQ(r.g.decode(0xFF23).s, Gime::S_PIA1);
    ASSERT_EQ(r.g.decode(0xFF13).s, Gime::S_NONE);
    ASSERT_EQ(r.g.decode(0xFF48).s, Gime::S_NONE);
    ASSERT_EQ(r.g.decode(0xFF50).s, Gime::S_SCS);
    r.g.write(0xFF90, 0x44);
    ASSERT_EQ(r.g.decode(0xFF48).s, Gime::S_SCS);
    ASSERT_EQ(r.g.decode(0xFF92).s, Gime::S_GIME);
    ASSERT_EQ(r.g.decode(0xFF7F).s, Gime::S_NONE);
}

// The SAM's R1 ($FFD8/$FFD9): 1.79 MHz
TEST_CASE(gime_speed_switch) {
    Rig r;
    ASSERT_EQ(r.g.cycleTicks(), 16);
    r.g.write(0xFFD9, 0);
    ASSERT_EQ(r.g.cycleTicks(), 8);
    ASSERT(r.g.fast());
    r.g.write(0xFFD7, 0);                      // R0 is ignored by the GIME
    ASSERT_EQ(r.g.cycleTicks(), 8);
    r.g.write(0xFFD8, 0);
    ASSERT_EQ(r.g.cycleTicks(), 16);
}

// Sixteen palette registers of six bits; RGB and composite colours
TEST_CASE(gime_palette) {
    Rig r;
    r.g.write(0xFFB3, 0xFF);
    ASSERT_EQ(rd(r.g, 0xFFB3), 0x3F);
    r.g.write(0xFFBF, 0x24);
    ASSERT_EQ(r.g.palette(15), 0x24);
    ASSERT_EQ(Gime::rgb(0x00), 0xFF000000u);
    ASSERT_EQ(Gime::rgb(0x3F), 0xFFFFFFFFu);
    ASSERT_EQ(Gime::rgb(0x20), 0xFFAA0000u);   // R1
    ASSERT_EQ(Gime::rgb(0x04), 0xFF550000u);   // R0
    ASSERT_EQ(Gime::rgb(0x24), 0xFFFF0000u);
    ASSERT_EQ(Gime::rgb(0x12), 0xFF00FF00u);   // G1 G0
    ASSERT_EQ(Gime::rgb(0x09), 0xFF0000FFu);   // B1 B0
    // Composite: hue 0 is grey, 0 black, 63 white; the intensities brighten
    ASSERT_EQ(Gime::composite(0x00), 0xFF000000u);
    ASSERT_EQ(Gime::composite(0x3F), 0xFFFFFFFFu);
    const uint32_t g1 = Gime::composite(0x10), g2 = Gime::composite(0x20);
    ASSERT((g1 & 0xFF) == ((g1 >> 8) & 0xFF) && (g1 & 0xFF) == ((g1 >> 16) & 0xFF));
    ASSERT((g2 & 0xFF) > (g1 & 0xFF));
    ASSERT_NE(Gime::composite(0x14), Gime::composite(0x18));     // hues differ
    r.g.setMonitor(Gime::Monitor::Rgb);
    ASSERT_EQ(r.g.colour(0x24), Gime::rgb(0x24));
    r.g.setMonitor(Gime::Monitor::Composite);
    ASSERT_EQ(r.g.colour(0x24), Gime::composite(0x24));
}

// The timer at the line rate: value + 1 lines on the 1987 GIME, + 2 on the 1986
TEST_CASE(gime_timer_line_rate) {
    for (int rev = 0; rev < 2; ++rev) {
        Rig r(128, rev ? Gime::Revision::G1987 : Gime::Revision::G1986);
        r.g.write(0xFF90, 0x20);               // IEN
        r.g.write(0xFF92, Gime::INT_TMR);
        r.g.advance(5);                        // just past an HSYNC
        r.g.write(0xFF95, 10);
        ASSERT_EQ(r.g.timerCount(), 0);        // $FF95 alone doesn't start it
        r.g.write(0xFF94, 0);
        rd(r.g, 0xFF92);                       // enabled while stopped: raised at once; acknowledge
        const int lines = rev ? 11 : 12;
        ASSERT_EQ(r.g.timerCount(), lines);
        // The 1986 GIME's (and the 1987's) count passes at each HSYNC fall
        r.g.advance((uint64_t)Gime::LINE_TICKS * (uint64_t)(lines - 1));
        ASSERT(!(r.g.irqStatus() & Gime::INT_TMR));
        ASSERT(!r.g.irq());
        r.g.advance(Gime::LINE_TICKS);
        ASSERT(r.g.irqStatus() & Gime::INT_TMR);
        ASSERT(r.g.irq());
        // Reloaded: it goes again
        ASSERT_EQ(r.g.timerCount(), lines);
        ASSERT_EQ(rd(r.g, 0xFF92) & Gime::INT_TMR, Gime::INT_TMR);
        ASSERT(!r.g.irq());
        r.g.advance((uint64_t)Gime::LINE_TICKS * (uint64_t)lines);
        ASSERT(r.g.irq());
    }
}

// TINS: 3.58 MHz, a count each 4 pixel ticks; a value of 1 counts 3 / 2
TEST_CASE(gime_timer_fast_clock) {
    for (int rev = 0; rev < 2; ++rev) {
        Rig r(128, rev ? Gime::Revision::G1987 : Gime::Revision::G1986);
        r.g.write(0xFF90, 0x10);               // FEN
        r.g.write(0xFF93, Gime::INT_TMR);
        r.g.write(0xFF91, 0x20);
        r.g.write(0xFF95, 100);
        r.g.write(0xFF94, 0);
        rd(r.g, 0xFF93);
        const uint64_t ticks = (uint64_t)(100 + (rev ? 1 : 2)) * 4;
        r.g.advance(ticks - 1);
        ASSERT(!r.g.firq());
        r.g.advance(1);
        ASSERT(r.g.firq());
        ASSERT(!r.g.irq());
        ASSERT(r.g.blink());                   // the timer drives the text blink
        // 1: 3 counts (1986), 2 (1987)
        r.g.write(0xFF95, 1);
        r.g.write(0xFF94, 0);
        ASSERT_EQ(r.g.timerCount(), rev ? 2 : 3);
        // 0 stops it; a newly enabled TMR with the timer stopped is raised at once
        r.g.write(0xFF95, 0);
        r.g.write(0xFF94, 0);
        ASSERT_EQ(r.g.timerCount(), 0);
        rd(r.g, 0xFF93);
        r.g.write(0xFF93, 0);
        r.g.write(0xFF93, Gime::INT_TMR);
        ASSERT(r.g.firqStatus() & Gime::INT_TMR);
    }
}

// Enables, IEN/FEN, reading clears, disabling clears; inputs as edges and levels
TEST_CASE(gime_interrupt_semantics) {
    Rig r;
    r.g.write(0xFF92, Gime::INT_VBORD);        // enabled, but IEN off
    r.frames(1);
    ASSERT(r.g.irqStatus() & Gime::INT_VBORD);
    ASSERT(!r.g.irq());
    r.g.write(0xFF90, 0x20);
    ASSERT(r.g.irq());                         // IEN lets the held status out
    ASSERT_EQ(rd(r.g, 0xFF92), Gime::INT_VBORD);
    ASSERT_EQ(r.g.irqStatus(), 0);
    ASSERT(!r.g.irq());
    // VBORD once a frame, HBORD every line
    r.frames(1);
    ASSERT(r.g.irqStatus() & Gime::INT_VBORD);
    ASSERT(!(r.g.irqStatus() & Gime::INT_HBORD));
    r.g.write(0xFF92, Gime::INT_HBORD);         // disabling VBORD clears it
    ASSERT_EQ(r.g.irqStatus(), 0);
    r.g.advance(Gime::LINE_TICKS);
    ASSERT_EQ(r.g.irqStatus(), Gime::INT_HBORD);
    // FIRQ: its own status
    r.g.write(0xFF92, 0);
    r.g.write(0xFF90, 0x10);
    r.g.write(0xFF93, Gime::INT_EI0);
    r.g.setInterruptInput(Gime::INT_EI0, true);  // the cartridge's falling edge
    ASSERT(r.g.firq());
    ASSERT(!r.g.irq());
    ASSERT_EQ(rd(r.g, 0xFF93), Gime::INT_EI0);
    ASSERT(!r.g.firq());
    r.g.setInterruptInput(Gime::INT_EI0, true);  // held: no new edge
    ASSERT(!r.g.firq());
    // The keyboard held when its enable is set: raised at once
    r.g.setInterruptInput(Gime::INT_EI1, true);
    ASSERT(!(r.g.firqStatus() & Gime::INT_EI1));
    r.g.write(0xFF93, Gime::INT_EI0 | Gime::INT_EI1);
    ASSERT(r.g.firqStatus() & Gime::INT_EI1);
    ASSERT(r.g.firq());
    // Both registers at once: an event goes to both
    r.g.write(0xFF90, 0x30);
    r.g.write(0xFF92, Gime::INT_EI2);
    r.g.write(0xFF93, Gime::INT_EI2);
    r.g.setInterruptInput(Gime::INT_EI2, true);
    ASSERT(r.g.irq() && r.g.firq());
}

// 60 Hz: 263 lines; H50: 314; FSYNC and HSYNC edges
TEST_CASE(gime_field_rate_and_sync) {
    Rig r;
    int hs = 0, fsFalls = 0;
    r.g.onHs = [&](bool level, uint64_t) { if (!level) ++hs; };
    r.g.onFs = [&](bool level, uint64_t) { if (!level) ++fsFalls; };
    r.frames(10);
    ASSERT_EQ(hs, 2630);
    ASSERT(fsFalls == 10 || fsFalls == 9);
    r.g.write(0xFF98, 0x08);
    r.frames(1);
    fsFalls = 0;
    r.g.advance(314ull * Gime::LINE_TICKS * 5);
    ASSERT_EQ(r.g.linesPerField(), 314);
    ASSERT(fsFalls == 5 || fsFalls == 4);
}

// CoCo 3 text, 40 columns with attributes
TEST_CASE(gime_text40_attributes) {
    Rig r;
    r.g.write(0xFF98, 0x03);                   // text, 8 lines a row
    r.g.write(0xFF99, 0x05);                   // 192 lines, 40 columns, attributes
    r.setY(0x60000);                           // physical 0 on 128K
    r.g.write(0xFFB1, 0x01);                   // background 1
    r.g.write(0xFFBA, 0x3F);                   // foreground 8 + 2
    r.g.write(0xFFBB, 0x24);
    r.g.write(0xFF9A, 0x09);                   // border
    r.at(0) = 'A';
    r.at(1) = 0x11;                            // fg 2, bg 1
    r.at(2) = 'B';
    r.at(3) = 0x51;                            // underlined
    r.at(80) = 'A';
    r.at(81) = 0x19;                           // fg 3 on bg 1, the second row
    r.frames();
    const int x0 = r.g.activeX(), y0 = r.g.activeY();
    ASSERT_EQ(x0, 32);
    ASSERT_EQ(y0, 24);
    // The first line shows font row 1 of 'A': ...X.... (two ticks a dot)
    const uint8_t aRow1 = gime_font::GLYPHS['A'][1];
    for (int d = 0; d < 8; ++d) {
        const uint8_t want = (aRow1 & (0x80 >> d)) ? 0x3F : 0x01;
        ASSERT_EQ(r.g.pixel(x0 + d * 2, y0), want);
        ASSERT_EQ(r.g.pixel(x0 + d * 2 + 1, y0), want);
    }
    // Underline: the row's last line all foreground
    for (int d = 0; d < 16; ++d) ASSERT_EQ(r.g.pixel(x0 + 16 + d, y0 + 7), 0x3F);
    // The next row of characters 8 lines down, 80 bytes on
    ASSERT_EQ(r.g.pixel(x0 + 6, y0 + 8), 0x24);
    // Border
    ASSERT_EQ(r.g.pixel(0, y0), 0x09);
    ASSERT_EQ(r.g.pixel(x0 + 640, y0), 0x09);
    ASSERT_EQ(r.g.pixel(x0, y0 - 1), 0x09);
    ASSERT_EQ(r.g.pixel(x0, y0 + 192), 0x09);
    // Blink: hidden while the timer's blink flag is set
    r.at(1) = 0x91;
    r.g.write(0xFF91, 0x20);
    r.g.write(0xFF95, 1);
    r.g.write(0xFF94, 0);
    r.g.advance(8);                            // one expiry: blink on
    r.g.write(0xFF95, 0);
    r.g.write(0xFF94, 0);
    ASSERT(r.g.blink());
    r.frames();
    ASSERT_EQ(r.g.pixel(x0 + 6, y0), 0x01);
}

// 80 and 32 columns without attributes: palette 1 on palette 0
TEST_CASE(gime_text80_and_32) {
    Rig r;
    r.g.write(0xFF98, 0x03);
    r.g.write(0xFF99, 0x14);                   // 80 columns (HRES 101), no attributes
    r.setY(0x60000);
    r.g.write(0xFFB0, 0x02);
    r.g.write(0xFFB1, 0x3F);
    r.at(0) = 'A';
    r.at(1) = 'A';
    r.frames();
    const int x0 = r.g.activeX(), y0 = r.g.activeY();
    const uint8_t aRow1 = gime_font::GLYPHS['A'][1];
    for (int d = 0; d < 16; ++d) {
        const uint8_t want = (aRow1 & (0x80 >> (d & 7))) ? 0x3F : 0x02;
        ASSERT_EQ(r.g.pixel(x0 + d, y0), want);
    }
    // 32 columns (HRES 000): 512 ticks, further right; two ticks a dot
    r.g.write(0xFF99, 0x00);
    r.frames();
    const int x32 = r.g.activeX();
    ASSERT(x32 > x0 + 60 && x32 < x0 + 68);
    for (int d = 0; d < 8; ++d) {
        const uint8_t want = (aRow1 & (0x80 >> d)) ? 0x3F : 0x02;
        ASSERT_EQ(r.g.pixel(x32 + d * 2, y0), want);
    }
    ASSERT_EQ(r.g.pixel(x32 - 1, y0), 0x00);    // border (BRDR 0)
}

// Graphics: 320x225x16, 640x200x4, 256x192x2, 160x192x16
TEST_CASE(gime_graphics_modes) {
    Rig r;
    for (int i = 0; i < 16; ++i) r.g.write((uint16_t)(0xFFB0 + i), (uint8_t)(0x30 + i));
    r.setY(0x60000);
    r.at(0) = 0x12;
    r.at(1) = 0xF3;
    r.at(160) = 0x45;
    // 320 x 225, 16 colours (HRES 111, CRES 10, LPF 11)
    r.g.write(0xFF98, 0x80);
    r.g.write(0xFF99, 0x7E);
    r.frames();
    int x0 = r.g.activeX(), y0 = r.g.activeY();
    ASSERT_EQ(y0, 7);
    ASSERT_EQ(r.g.pixel(x0, y0), 0x31);
    ASSERT_EQ(r.g.pixel(x0 + 1, y0), 0x31);
    ASSERT_EQ(r.g.pixel(x0 + 2, y0), 0x32);
    ASSERT_EQ(r.g.pixel(x0 + 4, y0), 0x3F);
    ASSERT_EQ(r.g.pixel(x0 + 6, y0), 0x33);
    ASSERT_EQ(r.g.pixel(x0, y0 + 1), 0x34);     // the next line: 160 bytes on
    ASSERT_EQ(r.g.pixel(x0, y0 + 224), 0x30);
    ASSERT_EQ(r.g.pixel(x0, y0 + 225), 0x00);   // the border
    // 640 x 200, 4 colours (HRES 111, CRES 01, LPF 01)
    r.g.write(0xFF99, 0x3D);
    r.at(0) = 0x1B;                             // 0 1 2 3
    r.frames();
    x0 = r.g.activeX();
    y0 = r.g.activeY();
    ASSERT_EQ(y0, 22);
    for (int p = 0; p < 4; ++p) ASSERT_EQ(r.g.pixel(x0 + p, y0), 0x30 + p);
    // 256 x 192, 2 colours (HRES 010, CRES 00): two ticks a pixel, a 512-tick area
    r.g.write(0xFF99, 0x08);
    r.at(0) = 0x80;
    r.frames();
    x0 = r.g.activeX();
    y0 = r.g.activeY();
    ASSERT_EQ(y0, 24);
    ASSERT_EQ(r.g.pixel(x0, y0), 0x31);
    ASSERT_EQ(r.g.pixel(x0 + 1, y0), 0x31);
    ASSERT_EQ(r.g.pixel(x0 + 2, y0), 0x30);
    // 160 x 192, 16 colours (HRES 101, CRES 10): four ticks a pixel
    r.g.write(0xFF99, 0x16);
    r.at(0) = 0x5A;
    r.frames();
    x0 = r.g.activeX();
    for (int t = 0; t < 4; ++t) ASSERT_EQ(r.g.pixel(x0 + t, y0), 0x35);
    for (int t = 4; t < 8; ++t) ASSERT_EQ(r.g.pixel(x0 + t, y0), 0x3A);
    // MOCH on composite: the intensity only
    r.g.write(0xFF98, 0x90);
    r.frames();
    ASSERT_EQ(r.g.pixel(x0, y0), 0x30);
    r.g.setMonitor(Gime::Monitor::Rgb);
    r.frames();
    ASSERT_EQ(r.g.pixel(x0, y0), 0x35);
}

// Lines per row in graphics: LPR 2 shows each row twice
TEST_CASE(gime_lines_per_row) {
    Rig r;
    r.g.write(0xFFB1, 0x3F);
    r.setY(0x60000);
    r.at(0) = 0x80;                             // row 0
    r.at(32) = 0x00;                            // row 1
    r.g.write(0xFF98, 0x82);                    // graphics, LPR 2
    r.g.write(0xFF99, 0x08);                    // 32 bytes, 2 colours
    r.frames();
    const int x0 = r.g.activeX(), y0 = r.g.activeY();
    ASSERT_EQ(r.g.pixel(x0, y0), 0x3F);
    ASSERT_EQ(r.g.pixel(x0, y0 + 1), 0x3F);
    ASSERT_EQ(r.g.pixel(x0, y0 + 2), 0x00);
}

// The vertical offset, the horizontal offset and the virtual 256-byte row
TEST_CASE(gime_offsets) {
    Rig r(512);
    r.g.write(0xFFB1, 0x3F);
    r.g.write(0xFF98, 0x80);
    r.g.write(0xFF99, 0x08);                    // 256 x 192 x 2, 32 bytes a row
    r.at(0x40000) = 0x80;
    r.at(0x40008) = 0x40;
    r.at(0x40000 + 256) = 0xC0;
    r.at(0x40000 + 32) = 0x20;
    r.setY(0x40000);
    r.frames();
    int x0 = r.g.activeX(), y0 = r.g.activeY();
    ASSERT_EQ(r.g.pixel(x0, y0), 0x3F);
    ASSERT_EQ(r.g.pixel(x0 + 4, y0 + 1), 0x3F);  // the second line: 32 bytes on
    // Eight bytes further ($FF9E counts 8 bytes)
    r.setY(0x40008);
    r.frames();
    ASSERT_EQ(r.g.pixel(x0, y0), 0x00);
    ASSERT_EQ(r.g.pixel(x0 + 2, y0), 0x3F);
    // The horizontal offset: X in 2-byte steps
    r.setY(0x40000);
    r.g.write(0xFF9F, 0x04);
    r.frames();
    ASSERT_EQ(r.g.pixel(x0 + 2, y0), 0x3F);
    // HVEN: rows of 256 bytes
    r.g.write(0xFF9F, 0x80);
    r.frames();
    ASSERT_EQ(r.g.pixel(x0 + 2, y0 + 1), 0x3F);
    ASSERT_EQ(r.g.pixel(x0, y0 + 1), 0x3F);
    // VSC: the first text row starts part way
    r.g.write(0xFF9F, 0x00);
    r.g.write(0xFF98, 0x03);
    r.g.write(0xFF99, 0x00);
    r.g.write(0xFF9C, 0x03);
    r.g.write(0xFFB0, 0x00);
    r.at(0x40000) = 'A';
    r.frames();
    x0 = r.g.activeX();
    const uint8_t aRow4 = gime_font::GLYPHS['A'][4];
    for (int d = 0; d < 8; ++d) ASSERT_EQ(r.g.pixel(x0 + d * 2, y0), (aRow4 & (0x80 >> d)) ? 0x3F : 0x00);
}

// The VDG-compatible modes: the mode from PIA1 port B, the address from the SAM
TEST_CASE(gime_vdg_compatible) {
    Rig r;
    r.g.write(0xFF90, 0x80);                    // COCO
    r.g.write(0xFF9C, 0x0F);                    // 12 lines a text row
    r.setY(0x70000);
    r.g.write(0xFFC9, 0);                       // SAM F1: $0400
    for (int i = 0; i < 16; ++i) r.g.write((uint16_t)(0xFFB0 + i), (uint8_t)(0x30 + i));
    const uint32_t screen = 0x70000 + 0x400;
    r.at(screen) = 0x01;                        // 'A'
    r.at(screen + 1) = 0x41;                    // inverse 'A'
    r.at(screen + 2) = 0x8F;                    // SG4 all set, colour 0 (green)
    r.g.snoopPia1(0xFF23, 0x34);
    r.g.snoopPia1(0xFF22, 0x00);                // alphanumerics
    r.frames();
    const int x0 = r.g.activeX(), y0 = r.g.activeY();
    ASSERT_EQ(x0, 32 + 64);
    ASSERT_EQ(y0, 24);
    const uint8_t aRow1 = gime_font::GLYPHS['A'][1];
    for (int d = 0; d < 8; ++d) {
        ASSERT_EQ(r.g.pixel(x0 + d * 2, y0 + 1), (aRow1 & (0x80 >> d)) ? 0x3D : 0x3C);
        ASSERT_EQ(r.g.pixel(x0 + 16 + d * 2, y0 + 1), (aRow1 & (0x80 >> d)) ? 0x3C : 0x3D);
    }
    ASSERT_EQ(r.g.pixel(x0 + 32, y0), 0x30);
    ASSERT_EQ(r.g.pixel(x0 + 32, y0 + 11), 0x30);
    ASSERT_EQ(r.g.pixel(x0 - 1, y0), 0x00);     // a text border is black
    // GM0: lower case ($01 is 'a', inverted as an MC6847T1)
    r.g.snoopPia1(0xFF22, 0x10);
    r.frames();
    const uint8_t lRow4 = gime_font::GLYPHS['a'][4];
    for (int d = 0; d < 8; ++d) ASSERT_EQ(r.g.pixel(x0 + d * 2, y0 + 4), (lRow4 & (0x80 >> d)) ? 0x3C : 0x3D);
    // PMODE 4 (G6R: SAM V 6, PB 11110xxx): palette 8/9
    r.g.write(0xFFC3, 0);
    r.g.write(0xFFC5, 0);
    r.g.snoopPia1(0xFF22, 0xF0);
    r.at(screen) = 0x80;
    r.frames();
    ASSERT_EQ(r.g.pixel(x0, y0), 0x39);
    ASSERT_EQ(r.g.pixel(x0 + 2, y0), 0x38);
    ASSERT_EQ(r.g.pixel(x0 - 1, y0), 0x39);     // a graphics border is the foreground
    ASSERT_EQ(r.g.pixel(x0 + 2, y0 + 1), r.g.pixel(x0 + 2, y0));
    // CSS: palette 10/11
    r.g.snoopPia1(0xFF22, 0xF8);
    r.frames();
    ASSERT_EQ(r.g.pixel(x0, y0), 0x3B);
    // The data register only: writes with CRB bit 2 clear are the DDR
    r.g.snoopPia1(0xFF23, 0x30);
    r.g.snoopPia1(0xFF22, 0x00);
    r.frames();
    ASSERT_EQ(r.g.pixel(x0, y0), 0x3B);
}

// Snapshots keep the registers, MMU and palette
TEST_CASE(gime_state_roundtrip) {
    Rig r;
    r.g.write(0xFF90, 0x6C);
    r.g.write(0xFFA2, 0x31);
    r.g.write(0xFFB4, 0x15);
    r.g.write(0xFFD9, 0);
    r.frames(1);
    const auto s = r.g.saveState();
    Rig q;
    q.g.loadState(s);
    ASSERT_EQ(q.g.reg(0), 0x6C);
    ASSERT_EQ(q.g.mmu(2), 0x31);
    ASSERT_EQ(q.g.palette(4), 0x15);
    ASSERT(q.g.fast());
    ASSERT_EQ(q.g.now(), r.g.now());
}
