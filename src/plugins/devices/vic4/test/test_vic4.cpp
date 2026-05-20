#include "test_harness.h"
#include "plugins/devices/vic4/main/vic4.h"
#include "plugins/devices/map_mmu/main/key_register.h"
#include "libmem/main/memory_bus.h"
#include <vector>

TEST_CASE(vic4_personality_lock) {
    VIC4 vic;
    uint8_t val = 0;

    // Initially locked
    ASSERT(vic.isLocked());

    // Extended registers should read $FF when locked
    ASSERT(vic.ioRead(nullptr, 0xD040, &val));
    ASSERT_EQ(val, 0xFF);

    // Unlock
    vic.setLocked(false);
    ASSERT(!vic.isLocked());

    // Now should read internal state (default 0)
    ASSERT(vic.ioRead(nullptr, 0xD040, &val));
    ASSERT_EQ(val, 0x00);
}

TEST_CASE(vic4_extended_registers) {
    VIC4 vic;
    vic.setLocked(false);
    uint8_t val = 0;

    // Write to $D040
    vic.ioWrite(nullptr, 0xD040, 0x42);
    ASSERT(vic.ioRead(nullptr, 0xD040, &val));
    ASSERT_EQ(val, 0x42);

    // Write to $D07F
    vic.ioWrite(nullptr, 0xD07F, 0x7F);
    ASSERT(vic.ioRead(nullptr, 0xD07F, &val));
    ASSERT_EQ(val, 0x7F);
}

TEST_CASE(vic4_palette_access) {
    VIC4 vic;
    vic.setLocked(false);
    uint8_t val = 0;

    // $D101: Palette 1 Red
    vic.ioWrite(nullptr, 0xD101, 0xAA);
    ASSERT(vic.ioRead(nullptr, 0xD101, &val));
    ASSERT_EQ(val, 0xAA);

    // $D201: Palette 1 Green
    vic.ioWrite(nullptr, 0xD201, 0xBB);
    ASSERT(vic.ioRead(nullptr, 0xD201, &val));
    ASSERT_EQ(val, 0xBB);

    // $D301: Palette 1 Blue
    vic.ioWrite(nullptr, 0xD301, 0xCC);
    ASSERT(vic.ioRead(nullptr, 0xD301, &val));
    ASSERT_EQ(val, 0xCC);
}

TEST_CASE(vic4_key_integration) {
    VIC4 vic;
    KeyRegister keyReg;

    // Wire them via callback as in factory
    keyReg.setPersonalityChangeCallback([&vic](IopersonalityMode mode) {
        vic.setLocked(mode != IopersonalityMode::MEGA65);
    });

    ASSERT(vic.isLocked());

    // Switch to MEGA65 personality
    keyReg.ioWrite(nullptr, 0xD02F, 0x47);
    keyReg.ioWrite(nullptr, 0xD02F, 0x53);
    
    ASSERT(!vic.isLocked());

    // Switch back to C64
    keyReg.ioWrite(nullptr, 0xD02F, 0x00);
    keyReg.ioWrite(nullptr, 0xD02F, 0x00);

    ASSERT(vic.isLocked());
}

// --- Rendering tests ---

namespace {

struct Vic4Fixture {
    FlatMemoryBus bus{"system", 16};
    std::vector<uint8_t> charRom = std::vector<uint8_t>(4096, 0);
    std::vector<uint8_t> colorRam = std::vector<uint8_t>(1024, 0);
    VIC4 vic;

    Vic4Fixture() {
        vic.setDmaBus(&bus);
        vic.setCharRom(charRom.data(), charRom.size());
        vic.setColorRam(colorRam.data());
        vic.reset();
    }

    void unlock() { vic.setLocked(false); }
};

} // namespace

TEST_CASE(vic4_render_locked) {
    Vic4Fixture f;
    f.vic.ioWrite(nullptr, 0xD020, 0x01); // white border

    uint32_t buf[VIC2::FRAME_W * VIC2::FRAME_H];
    f.vic.renderFrame(buf);
    ASSERT_EQ(buf[0], (uint32_t)0xFFFFFFFF); // VIC2 rendering
}

TEST_CASE(vic4_render_unlocked_vic3_fallback) {
    Vic4Fixture f;
    f.unlock();

    // No FCM bits → falls through to VIC3
    f.vic.ioWrite(nullptr, 0xD054, 0x00);
    f.vic.ioWrite(nullptr, 0xD020, 0x01);

    uint32_t buf[VIC2::FRAME_W * VIC2::FRAME_H];
    f.vic.renderFrame(buf);
    ASSERT_EQ(buf[0], f.vic.getPaletteRGBA(1));
}

TEST_CASE(vic4_tick) {
    Vic4Fixture f;
    // Tick should advance raster via VIC3→VIC2
    f.vic.tick(VIC2::CYCLES_PER_LINE);
    uint8_t val;
    f.vic.ioRead(nullptr, 0xD012, &val);
    ASSERT_EQ(val, (uint8_t)1);
}

TEST_CASE(vic4_screen_base_register) {
    Vic4Fixture f;
    f.unlock();

    // $D060-$D063: SCRNPTR
    f.vic.ioWrite(nullptr, 0xD060, 0x00);
    f.vic.ioWrite(nullptr, 0xD061, 0x04);
    f.vic.ioWrite(nullptr, 0xD062, 0x00);
    f.vic.ioWrite(nullptr, 0xD063, 0x00);
    ASSERT_EQ(f.vic.getScreenBase(), (uint32_t)0x0400);

    // 28-bit address
    f.vic.ioWrite(nullptr, 0xD063, 0x01);
    ASSERT_EQ(f.vic.getScreenBase(), (uint32_t)0x01000400);
}

TEST_CASE(vic4_char_base_register) {
    Vic4Fixture f;
    f.unlock();

    // $D068-$D06A: CHARPTR
    f.vic.ioWrite(nullptr, 0xD068, 0x00);
    f.vic.ioWrite(nullptr, 0xD069, 0x10);
    f.vic.ioWrite(nullptr, 0xD06A, 0x00);
    ASSERT_EQ(f.vic.getCharBase(), (uint32_t)0x1000);
}

TEST_CASE(vic4_col_base_register) {
    Vic4Fixture f;
    f.unlock();

    f.vic.ioWrite(nullptr, 0xD064, 0x00);
    f.vic.ioWrite(nullptr, 0xD065, 0x08);
    ASSERT_EQ(f.vic.getColBase(), (uint16_t)0x0800);
}

TEST_CASE(vic4_d054_bits) {
    Vic4Fixture f;
    f.unlock();

    f.vic.ioWrite(nullptr, 0xD054, VIC4::D054_CHR16 | VIC4::D054_FCLRLO);
    ASSERT_EQ(f.vic.d054(), (uint8_t)(VIC4::D054_CHR16 | VIC4::D054_FCLRLO));
}

// --- FCM tests ---

TEST_CASE(vic4_fcm_basic) {
    Vic4Fixture f;
    f.unlock();

    // Enable FCLRLO (FCM for char ≤ $FF)
    f.vic.ioWrite(nullptr, 0xD054, VIC4::D054_FCLRLO);

    // Set border/bg
    f.vic.ioWrite(nullptr, 0xD020, 0x00); // black border
    f.vic.ioWrite(nullptr, 0xD021, 0x00); // black bg

    // SCRNPTR at $0400 (default VIC-II screen area)
    f.vic.ioWrite(nullptr, 0xD060, 0x00);
    f.vic.ioWrite(nullptr, 0xD061, 0x04);
    f.vic.ioWrite(nullptr, 0xD062, 0x00);
    f.vic.ioWrite(nullptr, 0xD063, 0x00);

    // CHARPTR at $2000
    f.vic.ioWrite(nullptr, 0xD068, 0x00);
    f.vic.ioWrite(nullptr, 0xD069, 0x20);
    f.vic.ioWrite(nullptr, 0xD06A, 0x00);

    // Screen RAM: char 1 at position (0,0)
    f.bus.write8(0x0400, 1);

    // FCM char data: char 1 at $2000 + 1*64 = $2040
    // Row 0, pixel 0 = palette index 5 (green)
    f.bus.write8(0x2040, 5);
    // Row 0, pixel 1 = palette index 0 (bg)
    f.bus.write8(0x2041, 0);

    // Color RAM: fg color for $FF substitution (not used here)
    f.colorRam[0] = 0x01;

    uint32_t buf[VIC2::FRAME_W * VIC2::FRAME_H];
    f.vic.renderFrame(buf);

    int px = VIC2::DISPLAY_X;
    int py = VIC2::DISPLAY_Y;

    // Pixel 0 = palette 5 (green)
    ASSERT_EQ(buf[py * VIC2::FRAME_W + px], f.vic.getPaletteRGBA(5));
    // Pixel 1 = palette 0 (bg = black)
    ASSERT_EQ(buf[py * VIC2::FRAME_W + px + 1], f.vic.getPaletteRGBA(0));
}

TEST_CASE(vic4_fcm_ff_uses_color_ram) {
    Vic4Fixture f;
    f.unlock();

    f.vic.ioWrite(nullptr, 0xD054, VIC4::D054_FCLRLO);
    f.vic.ioWrite(nullptr, 0xD020, 0x00);
    f.vic.ioWrite(nullptr, 0xD021, 0x00);

    f.vic.ioWrite(nullptr, 0xD060, 0x00);
    f.vic.ioWrite(nullptr, 0xD061, 0x04);
    f.vic.ioWrite(nullptr, 0xD062, 0x00);
    f.vic.ioWrite(nullptr, 0xD063, 0x00);
    f.vic.ioWrite(nullptr, 0xD068, 0x00);
    f.vic.ioWrite(nullptr, 0xD069, 0x20);
    f.vic.ioWrite(nullptr, 0xD06A, 0x00);

    // Screen: char 0
    f.bus.write8(0x0400, 0);

    // FCM char data for char 0: row 0, pixel 0 = $FF
    f.bus.write8(0x2000, 0xFF);

    // Color RAM: fg = palette index 7 (yellow)
    f.colorRam[0] = 7;

    uint32_t buf[VIC2::FRAME_W * VIC2::FRAME_H];
    f.vic.renderFrame(buf);

    int px = VIC2::DISPLAY_X;
    int py = VIC2::DISPLAY_Y;

    // $FF pixel → uses colour RAM fg (index 7)
    ASSERT_EQ(buf[py * VIC2::FRAME_W + px], f.vic.getPaletteRGBA(7));
}

TEST_CASE(vic4_fcm_chr16) {
    Vic4Fixture f;
    f.unlock();

    // CHR16 + FCLRHI (FCM for char > $FF)
    f.vic.ioWrite(nullptr, 0xD054, VIC4::D054_CHR16 | VIC4::D054_FCLRHI);
    f.vic.ioWrite(nullptr, 0xD020, 0x00);
    f.vic.ioWrite(nullptr, 0xD021, 0x00);

    f.vic.ioWrite(nullptr, 0xD060, 0x00);
    f.vic.ioWrite(nullptr, 0xD061, 0x04);
    f.vic.ioWrite(nullptr, 0xD062, 0x00);
    f.vic.ioWrite(nullptr, 0xD063, 0x00);
    f.vic.ioWrite(nullptr, 0xD068, 0x00);
    f.vic.ioWrite(nullptr, 0xD069, 0x00);
    f.vic.ioWrite(nullptr, 0xD06A, 0x00);

    // 16-bit char number: $0100 (> $FF → FCLRHI applies)
    f.bus.write8(0x0400, 0x00); // lo byte
    f.bus.write8(0x0401, 0x01); // hi byte → char 256

    // FCM data for char 256: at charBase + 256*64 = $0000 + $4000 = $4000
    f.bus.write8(0x4000, 3); // pixel 0 = palette 3 (cyan)

    f.colorRam[0] = 0x01;

    uint32_t buf[VIC2::FRAME_W * VIC2::FRAME_H];
    f.vic.renderFrame(buf);

    int px = VIC2::DISPLAY_X;
    int py = VIC2::DISPLAY_Y;
    ASSERT_EQ(buf[py * VIC2::FRAME_W + px], f.vic.getPaletteRGBA(3));
}

TEST_CASE(vic4_fcm_fclrlo_only) {
    Vic4Fixture f;
    f.unlock();

    // FCLRLO only — char > $FF should NOT be rendered as FCM
    f.vic.ioWrite(nullptr, 0xD054, VIC4::D054_CHR16 | VIC4::D054_FCLRLO);
    f.vic.ioWrite(nullptr, 0xD020, 0x00);
    f.vic.ioWrite(nullptr, 0xD021, 0x00);

    f.vic.ioWrite(nullptr, 0xD060, 0x00);
    f.vic.ioWrite(nullptr, 0xD061, 0x04);
    f.vic.ioWrite(nullptr, 0xD068, 0x00);
    f.vic.ioWrite(nullptr, 0xD069, 0x20);
    f.vic.ioWrite(nullptr, 0xD06A, 0x00);

    // 16-bit char $0100 (> $FF, FCLRHI not set → not FCM)
    f.bus.write8(0x0400, 0x00);
    f.bus.write8(0x0401, 0x01);

    // Put something visible in FCM data
    f.bus.write8(0x2000 + 256 * 64, 5);

    uint32_t buf[VIC2::FRAME_W * VIC2::FRAME_H];
    f.vic.renderFrame(buf);

    int px = VIC2::DISPLAY_X;
    int py = VIC2::DISPLAY_Y;
    // Should be border (black), not palette 5 — FCM skipped this char
    ASSERT_EQ(buf[py * VIC2::FRAME_W + px], f.vic.getPaletteRGBA(0));
}

// --- NCM tests ---

TEST_CASE(vic4_ncm_basic) {
    Vic4Fixture f;
    f.unlock();

    // CHR16 + FCLRLO → enables per-char NCM via colour RAM bit 3
    f.vic.ioWrite(nullptr, 0xD054, VIC4::D054_CHR16 | VIC4::D054_FCLRLO);
    f.vic.ioWrite(nullptr, 0xD020, 0x00);
    f.vic.ioWrite(nullptr, 0xD021, 0x00);

    f.vic.ioWrite(nullptr, 0xD060, 0x00);
    f.vic.ioWrite(nullptr, 0xD061, 0x04);
    f.vic.ioWrite(nullptr, 0xD062, 0x00);
    f.vic.ioWrite(nullptr, 0xD063, 0x00);
    f.vic.ioWrite(nullptr, 0xD068, 0x00);
    f.vic.ioWrite(nullptr, 0xD069, 0x20);
    f.vic.ioWrite(nullptr, 0xD06A, 0x00);

    // 16-bit screen RAM: char 0 (lo=0, hi=0)
    f.bus.write8(0x0400, 0x00);
    f.bus.write8(0x0401, 0x00);

    // Colour RAM: bit 3 set = NCM flag, upper nibble = $A0
    // fgColor = 0xA8 (bit 3 set → NCM, upper nibble $A0)
    f.colorRam[0] = 0xA8;

    // NCM char data at $2000 + 0*64 = $2000
    // Row 0, byte 0: high nibble=3, low nibble=5 → pixels: $A3, $A5
    f.bus.write8(0x2000, 0x35);

    uint32_t buf[VIC2::FRAME_W * VIC2::FRAME_H];
    f.vic.renderFrame(buf);

    int px = VIC2::DISPLAY_X;
    int py = VIC2::DISPLAY_Y;

    // Left pixel: upper 4 bits from colour RAM ($A0) | high nibble (3) = $A3
    ASSERT_EQ(buf[py * VIC2::FRAME_W + px], f.vic.getPaletteRGBA(0xA3));
    // Right pixel: $A0 | low nibble (5) = $A5
    ASSERT_EQ(buf[py * VIC2::FRAME_W + px + 1], f.vic.getPaletteRGBA(0xA5));
}

TEST_CASE(vic4_ncm_nibble_f_uses_color_ram) {
    Vic4Fixture f;
    f.unlock();

    f.vic.ioWrite(nullptr, 0xD054, VIC4::D054_CHR16 | VIC4::D054_FCLRLO);
    f.vic.ioWrite(nullptr, 0xD020, 0x00);
    f.vic.ioWrite(nullptr, 0xD021, 0x00);

    f.vic.ioWrite(nullptr, 0xD060, 0x00);
    f.vic.ioWrite(nullptr, 0xD061, 0x04);
    f.vic.ioWrite(nullptr, 0xD068, 0x00);
    f.vic.ioWrite(nullptr, 0xD069, 0x20);
    f.vic.ioWrite(nullptr, 0xD06A, 0x00);

    f.bus.write8(0x0400, 0x00);
    f.bus.write8(0x0401, 0x00);

    // Colour RAM: NCM flag set (bit 3), full value = 0x78
    f.colorRam[0] = 0x78;

    // NCM data: byte = $FF → high nibble=$F, low nibble=$F
    f.bus.write8(0x2000, 0xFF);

    uint32_t buf[VIC2::FRAME_W * VIC2::FRAME_H];
    f.vic.renderFrame(buf);

    int px = VIC2::DISPLAY_X;
    int py = VIC2::DISPLAY_Y;

    // Nibble $F → use full colour RAM value (0x78) as palette index
    ASSERT_EQ(buf[py * VIC2::FRAME_W + px], f.vic.getPaletteRGBA(0x78));
    ASSERT_EQ(buf[py * VIC2::FRAME_W + px + 1], f.vic.getPaletteRGBA(0x78));
}

TEST_CASE(vic4_ncm_zero_nibble_is_bg) {
    Vic4Fixture f;
    f.unlock();

    f.vic.ioWrite(nullptr, 0xD054, VIC4::D054_CHR16 | VIC4::D054_FCLRLO);
    f.vic.ioWrite(nullptr, 0xD020, 0x00);
    f.vic.ioWrite(nullptr, 0xD021, 0x02); // bg = red (palette 2)

    f.vic.ioWrite(nullptr, 0xD060, 0x00);
    f.vic.ioWrite(nullptr, 0xD061, 0x04);
    f.vic.ioWrite(nullptr, 0xD068, 0x00);
    f.vic.ioWrite(nullptr, 0xD069, 0x20);
    f.vic.ioWrite(nullptr, 0xD06A, 0x00);

    f.bus.write8(0x0400, 0x00);
    f.bus.write8(0x0401, 0x00);

    f.colorRam[0] = 0xA8; // NCM flag set

    // NCM data: $00 → both nibbles = 0 → background
    f.bus.write8(0x2000, 0x00);

    uint32_t buf[VIC2::FRAME_W * VIC2::FRAME_H];
    f.vic.renderFrame(buf);

    int px = VIC2::DISPLAY_X;
    int py = VIC2::DISPLAY_Y;
    ASSERT_EQ(buf[py * VIC2::FRAME_W + px], f.vic.getPaletteRGBA(2));
}

// --- Variable columns/rows and line step ---

TEST_CASE(vic4_chrcount_default) {
    Vic4Fixture f;
    f.unlock();
    // Default CHRCOUNT = 0 → getChrCount returns -1 (use default)
    ASSERT_EQ(f.vic.getChrCount(), -1);
}

TEST_CASE(vic4_chrcount_custom) {
    Vic4Fixture f;
    f.unlock();
    f.vic.ioWrite(nullptr, 0xD05E, 60); // 60 columns
    ASSERT_EQ(f.vic.getChrCount(), 60);
}

TEST_CASE(vic4_disprows_default) {
    Vic4Fixture f;
    f.unlock();
    // Default DISPROWS = 0 → 25
    ASSERT_EQ(f.vic.getDispRows(), 25);
}

TEST_CASE(vic4_disprows_custom) {
    Vic4Fixture f;
    f.unlock();
    f.vic.ioWrite(nullptr, 0xD07B, 50); // 50 rows
    ASSERT_EQ(f.vic.getDispRows(), 50);
}

TEST_CASE(vic4_linestep_default) {
    Vic4Fixture f;
    f.unlock();
    // Default = 0 → auto
    ASSERT_EQ(f.vic.getLineStep(), (uint16_t)0);
}

TEST_CASE(vic4_linestep_custom) {
    Vic4Fixture f;
    f.unlock();
    f.vic.ioWrite(nullptr, 0xD058, 0x80); // LSB
    f.vic.ioWrite(nullptr, 0xD059, 0x00); // MSB → 128 bytes per row
    ASSERT_EQ(f.vic.getLineStep(), (uint16_t)128);
}

TEST_CASE(vic4_fcm_variable_rows) {
    Vic4Fixture f;
    f.unlock();

    // FCM with only 2 rows
    f.vic.ioWrite(nullptr, 0xD054, VIC4::D054_FCLRLO);
    f.vic.ioWrite(nullptr, 0xD07B, 2); // 2 rows
    f.vic.ioWrite(nullptr, 0xD020, 0x00);
    f.vic.ioWrite(nullptr, 0xD021, 0x00);

    f.vic.ioWrite(nullptr, 0xD060, 0x00);
    f.vic.ioWrite(nullptr, 0xD061, 0x04);
    f.vic.ioWrite(nullptr, 0xD068, 0x00);
    f.vic.ioWrite(nullptr, 0xD069, 0x20);
    f.vic.ioWrite(nullptr, 0xD06A, 0x00);

    // Char 1 at row 0 col 0
    f.bus.write8(0x0400, 1);
    // Char 1 FCM data: pixel 0 = palette 5
    f.bus.write8(0x2040, 5);
    f.colorRam[0] = 0x01;

    // Char 2 at row 1 col 0
    f.bus.write8(0x0400 + 40, 2);
    f.bus.write8(0x2080, 7); // char 2 pixel 0 = palette 7
    f.colorRam[40] = 0x01;

    uint32_t buf[VIC2::FRAME_W * VIC2::FRAME_H];
    f.vic.renderFrame(buf);

    int px = VIC2::DISPLAY_X;
    // Row 0
    ASSERT_EQ(buf[VIC2::DISPLAY_Y * VIC2::FRAME_W + px], f.vic.getPaletteRGBA(5));
    // Row 1 (8 pixels down)
    ASSERT_EQ(buf[(VIC2::DISPLAY_Y + 8) * VIC2::FRAME_W + px], f.vic.getPaletteRGBA(7));
    // Row 2 should NOT have been rendered (only 2 rows) — stays as border (black)
    ASSERT_EQ(buf[(VIC2::DISPLAY_Y + 16) * VIC2::FRAME_W + px], f.vic.getPaletteRGBA(0));
}

TEST_CASE(vic4_fcm_linestep) {
    Vic4Fixture f;
    f.unlock();

    // FCM with custom line step (64 bytes between rows instead of default 40)
    f.vic.ioWrite(nullptr, 0xD054, VIC4::D054_FCLRLO);
    f.vic.ioWrite(nullptr, 0xD058, 64); // LINESTEP LSB = 64
    f.vic.ioWrite(nullptr, 0xD059, 0);  // LINESTEP MSB = 0
    f.vic.ioWrite(nullptr, 0xD020, 0x00);
    f.vic.ioWrite(nullptr, 0xD021, 0x00);

    f.vic.ioWrite(nullptr, 0xD060, 0x00);
    f.vic.ioWrite(nullptr, 0xD061, 0x04);
    f.vic.ioWrite(nullptr, 0xD068, 0x00);
    f.vic.ioWrite(nullptr, 0xD069, 0x20);
    f.vic.ioWrite(nullptr, 0xD06A, 0x00);

    // Row 0, col 0: char 1
    f.bus.write8(0x0400, 1);
    f.bus.write8(0x2040, 3); // char 1 pixel 0 = palette 3
    f.colorRam[0] = 0x01;

    // Row 1 starts at scrBase + lineStep = $0400 + 64 = $0440
    f.bus.write8(0x0440, 2);
    f.bus.write8(0x2080, 9); // char 2 pixel 0 = palette 9
    f.colorRam[40] = 0x01; // colour RAM still uses cellIdx = row*cols+col = 1*40+0 = 40

    uint32_t buf[VIC2::FRAME_W * VIC2::FRAME_H];
    f.vic.renderFrame(buf);

    int px = VIC2::DISPLAY_X;
    ASSERT_EQ(buf[VIC2::DISPLAY_Y * VIC2::FRAME_W + px], f.vic.getPaletteRGBA(3));
    ASSERT_EQ(buf[(VIC2::DISPLAY_Y + 8) * VIC2::FRAME_W + px], f.vic.getPaletteRGBA(9));
}

TEST_CASE(vic4_ncm_without_chr16_is_fcm) {
    Vic4Fixture f;
    f.unlock();

    // FCLRLO but no CHR16 → NCM flag in colour RAM is ignored, all chars are FCM
    f.vic.ioWrite(nullptr, 0xD054, VIC4::D054_FCLRLO);
    f.vic.ioWrite(nullptr, 0xD020, 0x00);
    f.vic.ioWrite(nullptr, 0xD021, 0x00);

    f.vic.ioWrite(nullptr, 0xD060, 0x00);
    f.vic.ioWrite(nullptr, 0xD061, 0x04);
    f.vic.ioWrite(nullptr, 0xD068, 0x00);
    f.vic.ioWrite(nullptr, 0xD069, 0x20);
    f.vic.ioWrite(nullptr, 0xD06A, 0x00);

    f.bus.write8(0x0400, 0x00); // char 0

    // Colour RAM has bit 3 set, but CHR16 is off → treated as FCM
    f.colorRam[0] = 0x08;

    // FCM data: pixel 0 = palette 5
    f.bus.write8(0x2000, 5);

    uint32_t buf[VIC2::FRAME_W * VIC2::FRAME_H];
    f.vic.renderFrame(buf);

    int px = VIC2::DISPLAY_X;
    int py = VIC2::DISPLAY_Y;
    // Should render as FCM (byte=5 → palette 5), not NCM
    ASSERT_EQ(buf[py * VIC2::FRAME_W + px], f.vic.getPaletteRGBA(5));
}

// --- Sprite extension tests ---

TEST_CASE(vic4_sprite_extended_height) {
    Vic4Fixture f;
    f.unlock();

    // FCM mode so renderFCM calls renderSpritesV4
    f.vic.ioWrite(nullptr, 0xD054, VIC4::D054_FCLRLO);
    f.vic.ioWrite(nullptr, 0xD020, 0x00);
    f.vic.ioWrite(nullptr, 0xD021, 0x00);
    f.vic.ioWrite(nullptr, 0xD060, 0x00);
    f.vic.ioWrite(nullptr, 0xD061, 0x04);
    f.vic.ioWrite(nullptr, 0xD068, 0x00);
    f.vic.ioWrite(nullptr, 0xD069, 0x20);
    f.vic.ioWrite(nullptr, 0xD06A, 0x00);

    // Enable sprite 0
    f.vic.ioWrite(nullptr, 0xD015, 0x01);
    f.vic.ioWrite(nullptr, 0xD000, 50); // X
    f.vic.ioWrite(nullptr, 0xD001, 50); // Y
    f.vic.ioWrite(nullptr, 0xD027, 0x01); // color = white

    // Default pointer: screen=$0400, ptr at $07F8
    f.bus.write8(0x07F8, 0x0D); // data at $0340

    // Enable extended height for sprite 0: 30 pixels tall
    f.vic.ioWrite(nullptr, 0xD055, 0x01); // SPRHGTEN sprite 0
    f.vic.ioWrite(nullptr, 0xD056, 30);   // SPRHGHT = 30

    // Fill all 30 rows of sprite data (3 bytes/row) with $FF
    for (int r = 0; r < 30; r++) {
        f.bus.write8(0x0340 + r * 3, 0xFF);
        f.bus.write8(0x0340 + r * 3 + 1, 0xFF);
        f.bus.write8(0x0340 + r * 3 + 2, 0xFF);
    }

    uint32_t buf[VIC2::FRAME_W * VIC2::FRAME_H];
    f.vic.renderFrame(buf);

    // Row 0 should be white
    ASSERT_EQ(buf[50 * VIC2::FRAME_W + 50], f.vic.getPaletteRGBA(1));
    // Row 21 (beyond VIC-II limit) should also be white
    ASSERT_EQ(buf[71 * VIC2::FRAME_W + 50], f.vic.getPaletteRGBA(1));
    // Row 29 (last) should be white
    ASSERT_EQ(buf[79 * VIC2::FRAME_W + 50], f.vic.getPaletteRGBA(1));
    // Row 30 should NOT be sprite (border = black)
    ASSERT_EQ(buf[80 * VIC2::FRAME_W + 50], f.vic.getPaletteRGBA(0));
}

TEST_CASE(vic4_sprite_16colour) {
    Vic4Fixture f;
    f.unlock();

    f.vic.ioWrite(nullptr, 0xD054, VIC4::D054_FCLRLO);
    f.vic.ioWrite(nullptr, 0xD020, 0x00);
    f.vic.ioWrite(nullptr, 0xD021, 0x00);
    f.vic.ioWrite(nullptr, 0xD060, 0x00);
    f.vic.ioWrite(nullptr, 0xD061, 0x04);
    f.vic.ioWrite(nullptr, 0xD068, 0x00);
    f.vic.ioWrite(nullptr, 0xD069, 0x20);
    f.vic.ioWrite(nullptr, 0xD06A, 0x00);

    // Enable sprite 0
    f.vic.ioWrite(nullptr, 0xD015, 0x01);
    f.vic.ioWrite(nullptr, 0xD000, 50);
    f.vic.ioWrite(nullptr, 0xD001, 50);

    f.bus.write8(0x07F8, 0x0D); // data at $0340

    // Enable 16-colour for sprite 0
    f.vic.ioWrite(nullptr, 0xD06B, 0x01);

    // 16-colour: 4 bits/pixel, 3 bytes/row = 6 pixels per row
    // Byte 0 of row 0: high nibble=5, low nibble=3
    f.bus.write8(0x0340, 0x53);

    uint32_t buf[VIC2::FRAME_W * VIC2::FRAME_H];
    f.vic.renderFrame(buf);

    // Color = sprite# * 16 + nibble. Sprite 0 → 0*16+5=5, 0*16+3=3
    ASSERT_EQ(buf[50 * VIC2::FRAME_W + 50], f.vic.getPaletteRGBA(5));
    ASSERT_EQ(buf[50 * VIC2::FRAME_W + 51], f.vic.getPaletteRGBA(3));
}

TEST_CASE(vic4_sprite_16colour_transparent) {
    Vic4Fixture f;
    f.unlock();

    f.vic.ioWrite(nullptr, 0xD054, VIC4::D054_FCLRLO);
    f.vic.ioWrite(nullptr, 0xD020, 0x06); // blue border/bg
    f.vic.ioWrite(nullptr, 0xD021, 0x06);
    f.vic.ioWrite(nullptr, 0xD060, 0x00);
    f.vic.ioWrite(nullptr, 0xD061, 0x04);
    f.vic.ioWrite(nullptr, 0xD068, 0x00);
    f.vic.ioWrite(nullptr, 0xD069, 0x20);
    f.vic.ioWrite(nullptr, 0xD06A, 0x00);

    f.vic.ioWrite(nullptr, 0xD015, 0x01);
    f.vic.ioWrite(nullptr, 0xD000, 50);
    f.vic.ioWrite(nullptr, 0xD001, 50);
    f.bus.write8(0x07F8, 0x0D);

    f.vic.ioWrite(nullptr, 0xD06B, 0x01); // 16-colour sprite 0

    // Nibble 0 = transparent
    f.bus.write8(0x0340, 0x05); // high=0 (transparent), low=5

    uint32_t buf[VIC2::FRAME_W * VIC2::FRAME_H];
    f.vic.renderFrame(buf);

    // First pixel: transparent → should be border color (blue)
    ASSERT_EQ(buf[50 * VIC2::FRAME_W + 50], f.vic.getPaletteRGBA(6));
    // Second pixel: nibble 5 → palette 0*16+5=5
    ASSERT_EQ(buf[50 * VIC2::FRAME_W + 51], f.vic.getPaletteRGBA(5));
}

TEST_CASE(vic4_sprite_y_msb) {
    Vic4Fixture f;
    f.unlock();

    f.vic.ioWrite(nullptr, 0xD054, VIC4::D054_FCLRLO);
    f.vic.ioWrite(nullptr, 0xD020, 0x00);
    f.vic.ioWrite(nullptr, 0xD021, 0x00);
    f.vic.ioWrite(nullptr, 0xD060, 0x00);
    f.vic.ioWrite(nullptr, 0xD061, 0x04);
    f.vic.ioWrite(nullptr, 0xD068, 0x00);
    f.vic.ioWrite(nullptr, 0xD069, 0x20);
    f.vic.ioWrite(nullptr, 0xD06A, 0x00);

    f.vic.ioWrite(nullptr, 0xD015, 0x01);
    f.vic.ioWrite(nullptr, 0xD000, 10);
    f.vic.ioWrite(nullptr, 0xD001, 0x05); // Y low = 5
    f.vic.ioWrite(nullptr, 0xD077, 0x01); // SPRYMSBS: sprite 0 bit 8 set → Y = 256+5 = 261

    f.bus.write8(0x07F8, 0x0D);
    f.vic.ioWrite(nullptr, 0xD027, 0x01);

    f.bus.write8(0x0340, 0xFF);
    f.bus.write8(0x0341, 0xFF);
    f.bus.write8(0x0342, 0xFF);

    uint32_t buf[VIC2::FRAME_W * VIC2::FRAME_H];
    f.vic.renderFrame(buf);

    // Sprite at Y=261 — should be visible near bottom of frame
    ASSERT_EQ(buf[261 * VIC2::FRAME_W + 10], f.vic.getPaletteRGBA(1));
    // Y=260 should be border
    ASSERT_EQ(buf[260 * VIC2::FRAME_W + 10], f.vic.getPaletteRGBA(0));
}

TEST_CASE(vic4_sprite_pointer_relocation) {
    Vic4Fixture f;
    f.unlock();

    f.vic.ioWrite(nullptr, 0xD054, VIC4::D054_FCLRLO);
    f.vic.ioWrite(nullptr, 0xD020, 0x00);
    f.vic.ioWrite(nullptr, 0xD021, 0x00);
    f.vic.ioWrite(nullptr, 0xD060, 0x00);
    f.vic.ioWrite(nullptr, 0xD061, 0x04);
    f.vic.ioWrite(nullptr, 0xD068, 0x00);
    f.vic.ioWrite(nullptr, 0xD069, 0x20);
    f.vic.ioWrite(nullptr, 0xD06A, 0x00);

    f.vic.ioWrite(nullptr, 0xD015, 0x01);
    f.vic.ioWrite(nullptr, 0xD000, 50);
    f.vic.ioWrite(nullptr, 0xD001, 50);
    f.vic.ioWrite(nullptr, 0xD027, 0x01);

    // Relocate sprite pointers to $0800
    f.vic.ioWrite(nullptr, 0xD06C, 0x00); // SPRPTRADRLSB
    f.vic.ioWrite(nullptr, 0xD06D, 0x08); // SPRPTRADRMSB
    f.vic.ioWrite(nullptr, 0xD06E, 0x00); // SPRPTRBNK (no SPRPTR16)

    // Sprite 0 pointer at $0800: value $0E → data at $0E*64 = $0380
    f.bus.write8(0x0800, 0x0E);

    // Sprite data at $0380
    f.bus.write8(0x0380, 0xFF);
    f.bus.write8(0x0381, 0xFF);
    f.bus.write8(0x0382, 0xFF);

    uint32_t buf[VIC2::FRAME_W * VIC2::FRAME_H];
    f.vic.renderFrame(buf);

    ASSERT_EQ(buf[50 * VIC2::FRAME_W + 50], f.vic.getPaletteRGBA(1));
}
