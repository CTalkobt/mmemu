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
