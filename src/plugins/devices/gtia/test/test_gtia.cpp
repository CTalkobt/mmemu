#include "test_harness.h"
#include "plugins/devices/gtia/main/gtia.h"
#include <vector>

TEST_CASE(gtia_registers) {
    GTIA gtia;
    uint8_t val;
    
    // Test register write/read
    // HPOSP0 is write-only, M0PF is read-only at same address
    gtia.ioWrite(nullptr, 0xD000, 0x40); // HPOSP0 = 0x40
    gtia.ioRead(nullptr, 0xD000, &val);  // Read M0PF
    ASSERT(val == 0); // No collisions yet
    
    // COLPM0 is R/W (well, it's write-only but I implemented it as stored in m_regs for simplicity in this stub)
    // Wait, my implementation returns 0x00 for default in ioRead if not hit/trig/pal/consol.
    // Let's check a readable register like CONSOL
    gtia.ioRead(nullptr, 0xD01F, &val);
    ASSERT(val == 0x07); // Default switches
}

TEST_CASE(gtia_palette) {
    // Frames are 0xAARRGGBB: every entry is opaque.
    for (int i = 0; i < 256; i++) ASSERT((GTIA::getARGB((uint8_t)i) >> 24) == 0xFF);

    auto r = [](uint32_t c) { return (int)((c >> 16) & 0xFF); };
    auto g = [](uint32_t c) { return (int)((c >> 8) & 0xFF); };
    auto b = [](uint32_t c) { return (int)(c & 0xFF); };

    // Hue 0 is grey, from near black ($00) to near white ($0F).
    ASSERT(GTIA::getARGB(0x00) == 0xFF040404);
    ASSERT(GTIA::getARGB(0x0F) == 0xFFEAEAEA);
    for (int l = 0; l < 16; l++) {
        uint32_t c = GTIA::getARGB((uint8_t)l);
        ASSERT(r(c) == g(c) && g(c) == b(c));
        if (l) ASSERT(r(c) > r(GTIA::getARGB((uint8_t)(l - 1))));  // luminance rises
    }

    // Every hue uses all 16 luminance steps (no black filler entries).
    for (int h = 1; h < 16; h++)
        ASSERT(GTIA::getARGB((uint8_t)(h << 4 | 0x0E)) != 0xFF000000);

    // Known hues: $4x red/pink, $94 (Atari BASIC background) blue, $Cx green.
    uint32_t red = GTIA::getARGB(0x46), blue = GTIA::getARGB(0x94), green = GTIA::getARGB(0xC6);
    ASSERT(r(red) > g(red) && r(red) > b(red));
    ASSERT(blue == 0xFF143382);
    ASSERT(b(blue) > r(blue) && b(blue) > g(blue));
    ASSERT(g(green) > r(green) && g(green) > b(green));
}

TEST_CASE(gtia_collisions) {
    GTIA gtia;
    uint8_t val;
    
    // Mock a collision (internal state for now)
    // In real use, collisions would be set by the renderer/tick.
    // Since tick is a stub, we'll verify HITCLR works.
    
    // We can't easily set collisions from outside without adding a test-only method
    // or implementing enough of tick.
    // For now, just test HITCLR resets registers to 0.
    
    gtia.ioWrite(nullptr, 0xD01E, 0x01); // HITCLR
    for (int i=0; i<16; ++i) {
        gtia.ioRead(nullptr, 0xD000 + i, &val);
        ASSERT(val == 0);
    }
}

TEST_CASE(gtia_console) {
    GTIA gtia;
    uint8_t val;
    
    gtia.setConsoleSwitches(0x02); // Select only
    gtia.ioRead(nullptr, 0xD01F, &val);
    ASSERT(val == 0x02);
}

// ---------------------------------------------------------------------------
// GTIA modes, VDELAY, multicolour players, PAL (#10)
// ---------------------------------------------------------------------------
namespace {
struct Line {
    GTIA g;
    uint8_t pf[GTIA::LINE_PIXELS];
    uint32_t out[GTIA::LINE_PIXELS];
    Line() { std::fill(pf, pf + GTIA::LINE_PIXELS, (uint8_t)GTIA::PF_BK); }
    void w(int r, uint8_t v) { g.ioWrite(nullptr, 0xD000 + r, v); }
    /** A hires (mode F) playfield over pixels [x0, x1): `bits` repeats per 4 pixels. */
    void hires(int x0, int x1, uint8_t nibbleBits) {
        for (int x = x0; x < x1; ++x) {
            bool lit = nibbleBits & (8 >> (x & 3));
            pf[x] = (uint8_t)(GTIA::PF_2 | GTIA::PF_HIRES_MODE | GTIA::PF_WINDOW | (lit ? GTIA::PF_HIRES : 0));
        }
    }
};
}  // namespace

TEST_CASE(gtia_mode9_luminances) {
    Line l;
    l.w(GTIA::COLBK, 0x40);
    l.w(GTIA::PRIOR, 0x40);
    l.hires(32, 352, 0x0B);                    // each 4-pixel group: luminance 11
    l.g.renderLine(l.pf, l.out);
    ASSERT_EQ(l.out[64], GTIA::getARGB(0x4B));
    ASSERT_EQ(l.out[67], GTIA::getARGB(0x4B));
    ASSERT_EQ(l.out[8], GTIA::getARGB(0x40));  // border: COLBK
    // An odd luminance, which normal modes cannot show
    l.hires(32, 352, 0x0F);
    l.g.renderLine(l.pf, l.out);
    ASSERT_EQ(l.out[100], GTIA::getARGB(0x4F));
}

TEST_CASE(gtia_mode11_hues_and_mode10_registers) {
    Line l;
    l.w(GTIA::COLBK, 0x06);
    l.w(GTIA::PRIOR, 0xC0);
    l.hires(32, 352, 0x05);
    l.g.renderLine(l.pf, l.out);
    ASSERT_EQ(l.out[64], GTIA::getARGB(0x56));   // hue 5 at COLBK's luminance
    l.hires(32, 352, 0x00);
    l.g.renderLine(l.pf, l.out);
    ASSERT_EQ(l.out[64], GTIA::getARGB(0x00));   // 0: luminance 0

    // Mode 10: 9 colour registers; pixel 0-3 are COLPM0-3, 4-7 COLPF0-3, 8 COLBK.
    // Its pixels start a colour clock later: the nibble at pixels 64-67 shows at 66-69.
    Line m;
    m.w(GTIA::PRIOR, 0x80);
    m.w(GTIA::COLPM2, 0x26); m.w(GTIA::COLPF1, 0x88); m.w(GTIA::COLBK, 0x02);
    m.hires(32, 352, 0x02);
    m.g.renderLine(m.pf, m.out);
    ASSERT_EQ(m.out[68], GTIA::getARGB(0x26));
    m.hires(32, 352, 0x05);
    m.g.renderLine(m.pf, m.out);
    ASSERT_EQ(m.out[68], GTIA::getARGB(0x88));
    ASSERT_EQ(m.out[8], GTIA::getARGB(0x02));    // border
}

TEST_CASE(gtia_vdelay_and_multicolour_players) {
    GTIA g;
    g.ioWrite(nullptr, 0xD000 + GTIA::GRACTL, 0x03);
    g.ioWrite(nullptr, 0xD000 + GTIA::VDELAY, 0x10 | 0x01);   // player 0, missile 0
    g.dmaPlayer(0, 0xAA, false);
    g.dmaPlayer(0, 0x55, true);                                // even line: ignored
    ASSERT_EQ((int)g.reg(GTIA::GRAFP0), 0xAA);
    g.dmaPlayer(1, 0x55, true);                                // no VDELAY: taken
    ASSERT_EQ((int)g.reg(GTIA::GRAFP1), 0x55);
    g.dmaMissiles(0xFF, false);
    g.dmaMissiles(0x00, true);                                 // missile 0 keeps its bits
    ASSERT_EQ((int)g.reg(GTIA::GRAFM), 0x03);

    Line l;
    l.w(GTIA::GRAFP0, 0xFF); l.w(GTIA::GRAFP1, 0xFF);
    l.w(GTIA::HPOSP0, 100); l.w(GTIA::HPOSP1, 104);
    l.w(GTIA::COLPM0, 0x42); l.w(GTIA::COLPM1, 0x18);
    l.g.renderLine(l.pf, l.out);
    int overlap = (106 - GTIA::FIRST_COLOUR_CLOCK) * 2;
    ASSERT_EQ(l.out[overlap], GTIA::getARGB(0x42));            // P0 wins
    l.w(GTIA::PRIOR, 0x20);
    l.g.renderLine(l.pf, l.out);
    ASSERT_EQ(l.out[overlap], GTIA::getARGB(0x42 | 0x18));     // OR of both
    ASSERT_EQ(l.out[(101 - GTIA::FIRST_COLOUR_CLOCK) * 2], GTIA::getARGB(0x42));
}

TEST_CASE(gtia_pal_palette_and_register) {
    // Greys match NTSC; hues differ (PAL's own delays)
    for (int lum = 0; lum < 16; ++lum)
        ASSERT_EQ(GTIA::getARGB((uint8_t)lum, true), GTIA::getARGB((uint8_t)lum, false));
    ASSERT(GTIA::getARGB(0x46, true) != GTIA::getARGB(0x46, false));
    for (int i = 0; i < 256; ++i) ASSERT((GTIA::getARGB((uint8_t)i, true) >> 24) == 0xFF);

    Line l;
    l.g.setPal(true);
    uint8_t v = 0;
    l.g.ioRead(nullptr, 0xD014, &v);
    ASSERT_EQ((int)(v & 0x0E), 0);
    l.w(GTIA::COLBK, 0x46);
    l.g.renderLine(l.pf, l.out);
    ASSERT_EQ(l.out[0], GTIA::getARGB(0x46, true));
}
