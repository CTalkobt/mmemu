#include "test_harness.h"
#include "plugins/devices/vic3/main/vic3.h"
#include "libmem/main/memory_bus.h"
#include <cstring>
#include <vector>

namespace {

struct Vic3Fixture {
    FlatMemoryBus bus{"system", 16};
    std::vector<uint8_t> charRom = std::vector<uint8_t>(4096, 0);
    std::vector<uint8_t> colorRam = std::vector<uint8_t>(2048, 0); // 2KB for 80-col
    VIC3 vic;

    Vic3Fixture() {
        vic.setDmaBus(&bus);
        vic.setCharRom(charRom.data(), charRom.size());
        vic.setColorRam(colorRam.data());
        vic.reset();
    }

    void unlock() { vic.setLocked(false); }
};

} // namespace

// --- Identity and defaults ---

TEST_CASE(vic3_identity) {
    VIC3 vic;
    ASSERT(std::string(vic.name()) == "VIC-III");
    ASSERT_EQ(vic.addrMask(), (uint32_t)0x03FF);
    ASSERT(vic.isLocked());
}

TEST_CASE(vic3_custom_name) {
    VIC3 vic("CustomVIC3", 0xD000);
    ASSERT(std::string(vic.name()) == "CustomVIC3");
}

TEST_CASE(vic3_reset_locked) {
    Vic3Fixture f;
    f.unlock();
    ASSERT(!f.vic.isLocked());
    f.vic.reset();
    ASSERT(f.vic.isLocked());
}

// --- Register access when locked ---

TEST_CASE(vic3_locked_reads_ff) {
    Vic3Fixture f;
    uint8_t val;

    // VIC-III regs return $FF when locked
    f.vic.ioRead(nullptr, 0xD030, &val);
    ASSERT_EQ(val, (uint8_t)0xFF);
    f.vic.ioRead(nullptr, 0xD031, &val);
    ASSERT_EQ(val, (uint8_t)0xFF);

    // Palette returns $FF when locked
    f.vic.ioRead(nullptr, 0xD100, &val);
    ASSERT_EQ(val, (uint8_t)0xFF);
    f.vic.ioRead(nullptr, 0xD200, &val);
    ASSERT_EQ(val, (uint8_t)0xFF);
    f.vic.ioRead(nullptr, 0xD300, &val);
    ASSERT_EQ(val, (uint8_t)0xFF);
}

TEST_CASE(vic3_locked_writes_ignored) {
    Vic3Fixture f;
    // Write to $D031 while locked — should be silently ignored
    f.vic.ioWrite(nullptr, 0xD031, 0xFF);
    f.unlock();
    uint8_t val;
    f.vic.ioRead(nullptr, 0xD031, &val);
    ASSERT_EQ(val, (uint8_t)0x00); // Still 0 from reset
}

// --- Register access when unlocked ---

TEST_CASE(vic3_unlocked_d030_d031) {
    Vic3Fixture f;
    f.unlock();

    f.vic.ioWrite(nullptr, 0xD030, 0xA5);
    uint8_t val;
    f.vic.ioRead(nullptr, 0xD030, &val);
    ASSERT_EQ(val, (uint8_t)0xA5);

    f.vic.ioWrite(nullptr, 0xD031, VIC3::D031_H640 | VIC3::D031_ATTR);
    f.vic.ioRead(nullptr, 0xD031, &val);
    ASSERT_EQ(val, (uint8_t)(VIC3::D031_H640 | VIC3::D031_ATTR));
}

TEST_CASE(vic3_bitplane_regs) {
    Vic3Fixture f;
    f.unlock();

    // Bitplane enable ($D032)
    f.vic.ioWrite(nullptr, 0xD032, 0x0F); // enable planes 0-3
    uint8_t val;
    f.vic.ioRead(nullptr, 0xD032, &val);
    ASSERT_EQ(val, (uint8_t)0x0F);

    // Bitplane address ($D033)
    f.vic.ioWrite(nullptr, 0xD033, 0x12);
    f.vic.ioRead(nullptr, 0xD033, &val);
    ASSERT_EQ(val, (uint8_t)0x12);

    // Complement ($D03B)
    f.vic.ioWrite(nullptr, 0xD03B, 0xFF);
    f.vic.ioRead(nullptr, 0xD03B, &val);
    ASSERT_EQ(val, (uint8_t)0xFF);

    // BPX ($D03C), BPY ($D03D)
    f.vic.ioWrite(nullptr, 0xD03C, 10);
    f.vic.ioWrite(nullptr, 0xD03D, 20);
    f.vic.ioRead(nullptr, 0xD03C, &val);
    ASSERT_EQ(val, (uint8_t)10);
    f.vic.ioRead(nullptr, 0xD03D, &val);
    ASSERT_EQ(val, (uint8_t)20);
}

TEST_CASE(vic3_dat_ports) {
    Vic3Fixture f;
    f.unlock();

    for (int i = 0; i < 8; ++i) {
        f.vic.ioWrite(nullptr, 0xD040 + i, (uint8_t)(0x10 + i));
        uint8_t val;
        f.vic.ioRead(nullptr, 0xD040 + i, &val);
        ASSERT_EQ(val, (uint8_t)(0x10 + i));
    }
}

// --- Palette ---

TEST_CASE(vic3_palette_readwrite) {
    Vic3Fixture f;
    f.unlock();

    // Write palette entry 5: R=0xAA, G=0xBB, B=0xCC
    f.vic.ioWrite(nullptr, 0xD105, 0xAA);
    f.vic.ioWrite(nullptr, 0xD205, 0xBB);
    f.vic.ioWrite(nullptr, 0xD305, 0xCC);

    uint8_t r, g, b;
    f.vic.ioRead(nullptr, 0xD105, &r);
    f.vic.ioRead(nullptr, 0xD205, &g);
    f.vic.ioRead(nullptr, 0xD305, &b);
    ASSERT_EQ(r, (uint8_t)0xAA);
    ASSERT_EQ(g, (uint8_t)0xBB);
    ASSERT_EQ(b, (uint8_t)0xCC);

    // Verify RGBA conversion
    uint32_t rgba = f.vic.getPaletteRGBA(5);
    ASSERT_EQ(rgba, (uint32_t)(0xFF000000u | (0xCC << 16) | (0xBB << 8) | 0xAA));
}

TEST_CASE(vic3_palette_default_c64) {
    Vic3Fixture f;
    f.unlock();

    // Entry 0 = black
    ASSERT_EQ(f.vic.getPaletteRGBA(0), (uint32_t)0xFF000000);
    // Entry 1 = white
    ASSERT_EQ(f.vic.getPaletteRGBA(1), (uint32_t)0xFFFFFFFF);
    // Entry 16+ = black (uninitialized)
    ASSERT_EQ(f.vic.getPaletteRGBA(16), (uint32_t)0xFF000000);
}

// --- Rendering: locked delegates to VIC2 ---

TEST_CASE(vic3_render_locked_delegates_vic2) {
    Vic3Fixture f;
    // Locked mode should render exactly like VIC2
    f.vic.ioWrite(nullptr, 0xD020, 0x01); // white border

    uint32_t buf[VIC2::FRAME_W * VIC2::FRAME_H];
    f.vic.renderFrame(buf);

    // Border pixel should be white (VIC2 palette, not VIC3)
    ASSERT_EQ(buf[0], (uint32_t)0xFFFFFFFF);
}

// --- 80-column mode ---

TEST_CASE(vic3_render_80col_text) {
    Vic3Fixture f;
    f.unlock();

    // Enable H640 mode
    f.vic.ioWrite(nullptr, 0xD031, VIC3::D031_H640);

    // Set border and background
    f.vic.ioWrite(nullptr, 0xD020, 0x00); // black border
    f.vic.ioWrite(nullptr, 0xD021, 0x06); // blue background

    // Place char code 1 at screen position (0,0) — default screen=$0400
    f.bus.write8(0x0400, 1);

    // Char ROM: char 1, row 0 = 0xFF (all set)
    f.charRom[1 * 8 + 0] = 0xFF;

    // Color RAM: fg = white (1)
    f.colorRam[0] = 0x01;

    uint32_t buf[VIC2::FRAME_W * VIC2::FRAME_H];
    f.vic.renderFrame(buf);

    // First pixel of display area should be foreground (white)
    int px = VIC2::DISPLAY_X;
    int py = VIC2::DISPLAY_Y;
    ASSERT_EQ(buf[py * VIC2::FRAME_W + px], f.vic.getPaletteRGBA(1));
}

TEST_CASE(vic3_render_80col_background) {
    Vic3Fixture f;
    f.unlock();

    f.vic.ioWrite(nullptr, 0xD031, VIC3::D031_H640);
    f.vic.ioWrite(nullptr, 0xD020, 0x00);
    f.vic.ioWrite(nullptr, 0xD021, 0x06); // blue bg

    // All char data = 0 → all background
    uint32_t buf[VIC2::FRAME_W * VIC2::FRAME_H];
    f.vic.renderFrame(buf);

    int px = VIC2::DISPLAY_X;
    int py = VIC2::DISPLAY_Y;
    ASSERT_EQ(buf[py * VIC2::FRAME_W + px], f.vic.getPaletteRGBA(6));
}

// --- Bitplane mode ---

TEST_CASE(vic3_render_bitplane_single) {
    Vic3Fixture f;
    f.unlock();

    // Enable BPM, 320px wide
    f.vic.ioWrite(nullptr, 0xD031, VIC3::D031_BPM);
    f.vic.ioWrite(nullptr, 0xD020, 0x00); // black border

    // Enable only bitplane 0
    f.vic.ioWrite(nullptr, 0xD032, 0x01);

    // Bitplane 0 address: even=$0000 (nybble 0), odd=$0000
    f.vic.ioWrite(nullptr, 0xD033, 0x00);

    // Zero offsets
    f.vic.ioWrite(nullptr, 0xD03C, 0);
    f.vic.ioWrite(nullptr, 0xD03D, 0);
    f.vic.ioWrite(nullptr, 0xD03B, 0); // no complement

    // First byte of plane 0 data: 0xFF (8 set pixels)
    f.bus.write8(0x0000, 0xFF);

    uint32_t buf[VIC2::FRAME_W * VIC2::FRAME_H];
    f.vic.renderFrame(buf);

    // First display pixel: plane 0 bit set → palette index 1 (white)
    int px = VIC2::DISPLAY_X;
    int py = VIC2::DISPLAY_Y;
    ASSERT_EQ(buf[py * VIC2::FRAME_W + px], f.vic.getPaletteRGBA(1));
}

TEST_CASE(vic3_render_bitplane_complement) {
    Vic3Fixture f;
    f.unlock();

    f.vic.ioWrite(nullptr, 0xD031, VIC3::D031_BPM);
    f.vic.ioWrite(nullptr, 0xD020, 0x00);

    // Enable planes 0 and 1
    f.vic.ioWrite(nullptr, 0xD032, 0x03);
    f.vic.ioWrite(nullptr, 0xD033, 0x00); // plane 0 at $0000
    f.vic.ioWrite(nullptr, 0xD034, 0x01); // plane 1 at $2000 even

    // Complement plane 1 (bit 1 of $D03B)
    f.vic.ioWrite(nullptr, 0xD03B, 0x02);
    f.vic.ioWrite(nullptr, 0xD03C, 0);
    f.vic.ioWrite(nullptr, 0xD03D, 0);

    // Plane 0 data: 0x00 (all clear), Plane 1 data at $2000: 0x00 (all clear)
    // With complement on plane 1: plane 1 bits inverted → all 1s
    // Result: palette index = 0b10 = 2

    uint32_t buf[VIC2::FRAME_W * VIC2::FRAME_H];
    f.vic.renderFrame(buf);

    int px = VIC2::DISPLAY_X;
    int py = VIC2::DISPLAY_Y;
    ASSERT_EQ(buf[py * VIC2::FRAME_W + px], f.vic.getPaletteRGBA(2));
}

TEST_CASE(vic3_render_bitplane_disabled_complement) {
    Vic3Fixture f;
    f.unlock();

    f.vic.ioWrite(nullptr, 0xD031, VIC3::D031_BPM);
    f.vic.ioWrite(nullptr, 0xD020, 0x00);

    // All planes disabled
    f.vic.ioWrite(nullptr, 0xD032, 0x00);
    // Complement plane 0
    f.vic.ioWrite(nullptr, 0xD03B, 0x01);
    f.vic.ioWrite(nullptr, 0xD03C, 0);
    f.vic.ioWrite(nullptr, 0xD03D, 0);

    // Disabled plane with complement → contributes 1
    // Only plane 0 complement → index = 1
    uint32_t buf[VIC2::FRAME_W * VIC2::FRAME_H];
    f.vic.renderFrame(buf);

    int px = VIC2::DISPLAY_X;
    int py = VIC2::DISPLAY_Y;
    ASSERT_EQ(buf[py * VIC2::FRAME_W + px], f.vic.getPaletteRGBA(1));
}

// --- Unused register ranges ---

TEST_CASE(vic3_unused_ranges) {
    Vic3Fixture f;
    uint8_t val;

    // $D048-$D07F returns $FF (reserved for VIC-IV)
    f.vic.ioRead(nullptr, 0xD048, &val);
    ASSERT_EQ(val, (uint8_t)0xFF);
    f.vic.ioRead(nullptr, 0xD07F, &val);
    ASSERT_EQ(val, (uint8_t)0xFF);

    // $D080-$D0FF returns $FF
    f.vic.ioRead(nullptr, 0xD080, &val);
    ASSERT_EQ(val, (uint8_t)0xFF);
}

// --- VIC-II registers still work ---

TEST_CASE(vic3_vic2_compat) {
    Vic3Fixture f;

    // Standard VIC-II registers should still work through VIC3
    f.vic.ioWrite(nullptr, 0xD020, 0x0E); // border = light blue
    uint8_t val;
    f.vic.ioRead(nullptr, 0xD020, &val);
    ASSERT_EQ(val & 0x0F, (uint8_t)0x0E);

    // Sprite position
    f.vic.ioWrite(nullptr, 0xD000, 100);
    f.vic.ioRead(nullptr, 0xD000, &val);
    ASSERT_EQ(val, (uint8_t)100);
}

// --- Color register behavior in ATTR mode ---

TEST_CASE(vic3_attr_color_regs_8bit) {
    Vic3Fixture f;
    f.unlock();

    // Enable ATTR mode
    f.vic.ioWrite(nullptr, 0xD031, VIC3::D031_ATTR);

    // Write full 8-bit value to $D020 (border)
    f.vic.ioWrite(nullptr, 0xD020, 0xAB);
    uint8_t val;
    f.vic.ioRead(nullptr, 0xD020, &val);
    ASSERT_EQ(val, (uint8_t)0xAB); // Full 8-bit, not masked

    // Without ATTR mode, VIC-II masks to 4 bits
    f.vic.ioWrite(nullptr, 0xD031, 0x00); // ATTR off
    f.vic.ioRead(nullptr, 0xD020, &val);
    // VIC2 returns high nibble as 0xF
    ASSERT_EQ(val & 0xF0, (uint8_t)0xF0);
}
