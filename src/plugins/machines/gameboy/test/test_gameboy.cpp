// The Game Boy core (gb::Gameboy, after Mesen2) with small hand-assembled cartridges: the state the
// boot ROM leaves, instruction timing, the timer and its interrupt, HALT, OAM DMA, the PPU's lines
// and modes, MBC1/MBC5 banking with cartridge RAM, the joypad, the serial port and the APU.

#include "test_harness.h"
#include "plugins/machines/gameboy/main/gameboy.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

/** A ROM-only (or `type`) cartridge of `size` bytes with `prog` at $0150 (the entry jumps there). */
std::vector<uint8_t> cart(const std::vector<uint8_t>& prog, uint8_t type = 0, size_t size = 0x8000, uint8_t ram = 0) {
    std::vector<uint8_t> r(size, 0);
    const uint8_t entry[] = {0x00, 0xC3, 0x50, 0x01};
    std::memcpy(&r[0x100], entry, 4);
    std::memcpy(&r[0x134], "GBTEST", 6);
    r[0x147] = type;
    r[0x149] = ram;
    std::memcpy(&r[0x150], prog.data(), prog.size());
    return r;
}

bool boot(gb::Gameboy& g, const std::vector<uint8_t>& rom) {
    std::string err;
    if (!g.load(rom, err)) return false;
    g.powerOn();
    return true;
}

}  // namespace

TEST_CASE(gb_post_boot_state) {
    gb::Gameboy g;
    ASSERT(boot(g, cart({0x18, 0xFE})));
    auto& s = g.cpu().state();
    EXPECT_EQ(s.pc, 0x0100);
    EXPECT_EQ(s.sp, 0xFFFE);
    EXPECT_EQ(s.a, 0x01);
    EXPECT_EQ(s.f, 0xB0);
    EXPECT_EQ(s.c, 0x13);
    EXPECT_EQ(s.e, 0xD8);
    EXPECT_EQ(s.h, 0x01);
    EXPECT_EQ(s.l, 0x4D);
    EXPECT_EQ(g.peek(0xFF40), 0x91);
    EXPECT_EQ(g.peek(0xFF47), 0xFC);
    EXPECT_EQ(g.peek(0xFF26), 0xF1);           // sound on, square 1 playing
    EXPECT_EQ(g.peek(0xFF04), 0xAB);
    EXPECT_EQ(g.peek(0xFF00), 0xCF);           // both select lines low (boot_hwio-dmgABCmgb)
    EXPECT_EQ(g.peek(0xFF02), 0x7E);
    EXPECT_EQ(g.peek(0xFF0F), 0xE1);
    EXPECT_TRUE(!g.isSgb());
    EXPECT_EQ(std::string(g.mapperName()), std::string("ROM"));
    EXPECT_EQ(g.header().title, std::string("GBTEST"));
}

TEST_CASE(gb_cpu_timing_and_alu) {
    gb::Gameboy g;
    // LD A,$3C; ADD A,$C4 (zero, half carry, carry); PUSH AF; POP BC; LD SP,$D000; LD HL,$C000; LD (HL),$99;
    // INC (HL); DAA-free: LD A,$15; ADD A,$27; DAA (-> $42); LD ($C001),A; CB SWAP A; then loop
    ASSERT(boot(g, cart({0x3E, 0x3C, 0xC6, 0xC4, 0xF5, 0xC1, 0x31, 0x00, 0xD0, 0x21, 0x00, 0xC0, 0x36, 0x99, 0x34,
                         0x3E, 0x15, 0xC6, 0x27, 0x27, 0xEA, 0x01, 0xC0, 0xCB, 0x37, 0x18, 0xFE})));
    const uint64_t start = g.cycles();
    // 4 + 2+2+4(jp) for the entry
    while (g.cpu().state().pc != 0x0150) g.cpu().exec();
    EXPECT_EQ(g.cycles() - start, 4u * 4 + 4);   // NOP (1) + JP (4) machine cycles, from the first's start
    const uint64_t at = g.cycles();
    g.cpu().exec();                               // LD A,n: 2 machine cycles
    EXPECT_EQ(g.cycles() - at, 8u);
    while (g.cpu().state().pc != 0x0169) g.cpu().exec();
    auto& s = g.cpu().state();
    EXPECT_EQ(s.b, 0x00);
    EXPECT_EQ(s.c, 0xB0);                         // Z, H, C
    EXPECT_EQ(g.peek(0xC000), 0x9A);
    EXPECT_EQ(g.peek(0xC001), 0x42);
    EXPECT_EQ(s.a, 0x24);
}

TEST_CASE(gb_timer_interrupt_and_halt) {
    gb::Gameboy g;
    // $0050 (the timer's vector): INC ($C000) via HL, RETI. Main: HL=$C000, TMA=$F0, TAC=5 (16 clocks), IE=4, EI, HALT, JR -3
    auto rom = cart({0x21, 0x00, 0xC0, 0xAF, 0x77, 0x3E, 0xF0, 0xE0, 0x06, 0xE0, 0x05, 0x3E, 0x05, 0xE0, 0x07,
                     0x3E, 0x04, 0xE0, 0xFF, 0xAF, 0xE0, 0x0F, 0xFB, 0x76, 0x18, 0xFD});
    const uint8_t isr[] = {0x34, 0xD9};
    std::memcpy(&rom[0x50], isr, 2);
    ASSERT(boot(g, rom));
    g.runTo(g.cycles() + 4 * 100);
    const uint8_t a = g.peek(0xC000);
    g.runTo(g.cycles() + 16 * 16 * 10);               // 10 overflows: every 16 ticks of 16 clocks
    const int n = (uint8_t)(g.peek(0xC000) - a);
    EXPECT_TRUE(n >= 9 && n <= 11);
    EXPECT_EQ(g.peek(0xFF07), 0xFD);
}

TEST_CASE(gb_oam_dma_and_ppu_lines) {
    gb::Gameboy g;
    // Fill $C100-$C19F with i; DMA from $C1 by a routine in HRAM (the CPU can't read ROM during it)
    auto rom = cart({
        0x21, 0x00, 0xC1, 0xAF, 0x22, 0x3C, 0xFE, 0xA0, 0x20, 0xFA,          // fill
        0x21, 0x80, 0xFF,                                                    // $FF80: LDH ($46),A; wait; RET
        0x3E, 0xE0, 0x22, 0x3E, 0x46, 0x22, 0x3E, 0x3E, 0x22, 0x3E, 0x28, 0x22,
        0x3E, 0x3D, 0x22, 0x3E, 0x20, 0x22, 0x3E, 0xFD, 0x22, 0x3E, 0xC9, 0x22,
        0x3E, 0xC1, 0xCD, 0x80, 0xFF,                                        // CALL $FF80 with A=$C1
        0x18, 0xFE});
    ASSERT(boot(g, rom));
    g.runTo(g.cycles() + 20000);
    bool ok = true;
    for (int i = 0; i < 0xA0; ++i) if (g.ppu().oam()[i] != i) ok = false;
    EXPECT_TRUE(ok);
    EXPECT_EQ(g.peek(0xFF46), 0xC1);
    // The LCD: 154 lines of 456 clocks, LY and the modes
    const uint32_t f0 = g.frameCount();
    g.runFrame();
    g.runFrame();
    EXPECT_EQ(g.frameCount(), f0 + 2);
    const uint64_t c0 = g.cycles();
    g.runFrame();
    EXPECT_NEAR((double)(g.cycles() - c0), 70224.0, 24.0);
    while (g.peek(0xFF44) != 1) g.cpu().exec();      // (LY reads 0 on line 153 already)
    int lines = 0, modes = 0;
    for (int i = 0; i < 500; ++i) {
        g.cpu().exec();
        const int ly = g.peek(0xFF44), mode = g.peek(0xFF41) & 3;
        lines |= ly < 144 ? 1 : 2;
        modes |= 1 << mode;
    }
    EXPECT_EQ(lines, 1);
    EXPECT_EQ(modes, 0x0D);                            // HBlank, OAM search, drawing (not VBlank yet)
}

TEST_CASE(gb_mbc_banking_and_ram) {
    // MBC1, 128 KB, 8 KB RAM: bank n's first byte is n; the program reads banks 3 and 7 and keeps them in RAM
    {
        gb::Gameboy g;
        auto rom = cart({0x3E, 0x03, 0xEA, 0x00, 0x20, 0xFA, 0x00, 0x40, 0xEA, 0x00, 0xC0,     // bank 3
                         0x3E, 0x07, 0xEA, 0x00, 0x20, 0xFA, 0x00, 0x40, 0xEA, 0x01, 0xC0,     // bank 7
                         0x3E, 0x00, 0xEA, 0x00, 0x20, 0xFA, 0x00, 0x40, 0xEA, 0x02, 0xC0,     // 0 is 1
                         0xFA, 0x00, 0xA0, 0xEA, 0x03, 0xC0,                                   // RAM off: $FF
                         0x3E, 0x0A, 0xEA, 0x00, 0x00, 0x3E, 0x77, 0xEA, 0x34, 0xA1,           // RAM on, write
                         0x18, 0xFE},
                        0x03, 0x20000, 0x02);
        for (int b = 1; b < 8; ++b) rom[b * 0x4000] = (uint8_t)b;
        ASSERT(boot(g, rom));
        EXPECT_EQ(std::string(g.mapperName()), std::string("MBC1"));
        EXPECT_TRUE(g.hasBattery());
        g.runTo(g.cycles() + 4000);
        EXPECT_EQ(g.peek(0xC000), 3);
        EXPECT_EQ(g.peek(0xC001), 7);
        EXPECT_EQ(g.peek(0xC002), 1);
        EXPECT_EQ(g.peek(0xC003), 0xFF);
        EXPECT_EQ(g.cartRam()[0x134], 0x77);
    }
    // MBC5: 9-bit ROM bank (bank $101 of 512 KB wraps to $01... use bank $12), RAM banks
    {
        gb::Gameboy g;
        auto rom = cart({0x3E, 0x12, 0xEA, 0x00, 0x20, 0xAF, 0xEA, 0x00, 0x30, 0xFA, 0x00, 0x40, 0xEA, 0x00, 0xC0,
                         0x3E, 0x0A, 0xEA, 0x00, 0x00, 0x3E, 0x02, 0xEA, 0x00, 0x40, 0x3E, 0x55, 0xEA, 0x00, 0xA0,
                         0x18, 0xFE},
                        0x1B, 0x80000, 0x03);
        for (int b = 1; b < 32; ++b) rom[b * 0x4000] = (uint8_t)(0x80 + b);
        ASSERT(boot(g, rom));
        g.runTo(g.cycles() + 4000);
        EXPECT_EQ(g.peek(0xC000), 0x92);
        EXPECT_EQ(g.cartRam()[0x4000], 0x55);          // RAM bank 2
    }
    // An unsupported mapper is refused
    {
        gb::Gameboy g;
        std::string err;
        EXPECT_TRUE(!g.load(cart({}, 0x22), err));
        EXPECT_TRUE(err.find("$22") != std::string::npos);
    }
}

TEST_CASE(gb_joypad_serial_apu) {
    gb::Gameboy g;
    // Select the buttons (P15 low), read $FF00 into $C000; send $42 over the serial port; loop
    ASSERT(boot(g, cart({0x3E, 0x10, 0xE0, 0x00, 0xF0, 0x00, 0xEA, 0x00, 0xC0,
                         0x3E, 0x42, 0xE0, 0x01, 0x3E, 0x81, 0xE0, 0x02,
                         0x3E, 0x10, 0xE0, 0x00, 0xF0, 0x00, 0xEA, 0x00, 0xC0, 0x18, 0xF5})));
    g.runTo(g.cycles() + 400);
    EXPECT_EQ(g.peek(0xC000), 0xDF);
    EXPECT_EQ(g.peek(0xFF0F) & gb::IRQ_JOYPAD, 0);      // a select line changing with nothing pressed: no interrupt
    g.setButtons(gb::BTN_START | gb::BTN_A);
    g.runTo(g.cycles() + 400);
    EXPECT_EQ(g.peek(0xC000), 0xD6);
    EXPECT_TRUE((g.peek(0xFF0F) & gb::IRQ_JOYPAD) != 0);
    EXPECT_EQ(g.serialOutput(), std::string("B"));
    g.runTo(g.cycles() + 8 * 512 + 64);
    EXPECT_EQ(g.peek(0xFF02) & 0x80, 0);                // sent: SC's bit 7 cleared
    EXPECT_EQ(g.peek(0xFF01), 0xFF);                    // and $FF shifted in
    // The APU: the boot chime's square 1 fades; NR52 off clears the channels
    g.apu().setSampleRate(32000, gb::Gameboy::CLOCK_HZ);
    g.apu().samples().clear();
    g.runFrame();
    g.apu().run();
    EXPECT_TRUE(g.apu().samples().size() >= 2 * 500);   // ~16.7 ms at 32 kHz
    EXPECT_EQ(g.peek(0xFF26) & 0x80, 0x80);
}

// ---------------------------------------------------------------------------
// The Game Boy Color
// ---------------------------------------------------------------------------

namespace {

/** A CGB cartridge (header $0143: `cgbFlag`, $80 for CGB mode, $00 for a DMG one). */
std::vector<uint8_t> cgbCart(const std::vector<uint8_t>& prog, uint8_t cgbFlag = 0x80) {
    auto r = cart(prog);
    r[0x143] = cgbFlag;
    return r;
}

/** A Game Boy Color with `rom` in it, started without the boot ROM (in the state it leaves). */
bool bootCgb(gb::Gameboy& g, const std::vector<uint8_t>& rom) {
    g.setModel(gb::Gameboy::Model::Cgb);
    g.setSkipBootRom(true);
    return boot(g, rom);
}

}  // namespace

TEST_CASE(gb_cgb_post_boot_and_registers) {
    gb::Gameboy g;
    ASSERT(bootCgb(g, cgbCart({0x18, 0xFE})));
    EXPECT_TRUE(g.isCgb());
    EXPECT_TRUE(!g.isSgb());
    auto& s = g.cpu().state();
    EXPECT_EQ(s.pc, 0x0100);
    EXPECT_EQ(s.a, 0x11);                              // the CGB's
    EXPECT_EQ(s.f, 0x80);
    EXPECT_EQ(s.d, 0xFF);
    EXPECT_EQ(s.e, 0x56);
    EXPECT_EQ(s.l, 0x0D);
    EXPECT_EQ(g.peek(0xFF4D), 0x7E);                   // KEY1: normal speed, no switch armed
    EXPECT_EQ(g.peek(0xFF4F), 0xFE);                   // VBK: bank 0
    EXPECT_EQ(g.peek(0xFF70), 0xF9);                   // SVBK: bank 1
    EXPECT_EQ(g.peek(0xFF55), 0x80);                   // HDMA5: none running (Mesen's $80 | length)
    EXPECT_EQ(g.peek(0xFF02), 0x7C);                   // SC: the fast clock bit readable
    g.write(0xFF40, 0x11);                             // the LCD off: VRAM and the palettes free
    // VRAM banks
    g.write(0xFF4F, 0x01);
    EXPECT_EQ(g.peek(0xFF4F), 0xFF);
    g.write(0x8000, 0xAA);
    g.write(0xFF4F, 0x00);
    g.write(0x8000, 0x55);
    EXPECT_EQ(g.ppu().vram()[0x0000], 0x55);
    EXPECT_EQ(g.ppu().vram()[0x2000], 0xAA);
    EXPECT_EQ(g.peek(0x8000), 0x55);
    // Work RAM banks: 0 selects 1; $C000-$CFFF is always bank 0
    g.write(0xFF70, 0x02);
    g.write(0xD000, 0x22);
    g.write(0xFF70, 0x00);
    EXPECT_EQ(g.peek(0xFF70), 0xF8);
    EXPECT_EQ(g.wramBank(), 1);
    g.write(0xD000, 0x11);
    g.write(0xFF70, 0x07);
    EXPECT_EQ(g.peek(0xD000), 0x00);
    EXPECT_EQ(g.peek(0xF000), 0x00);                   // the echo follows
    g.write(0xFF70, 0x02);
    EXPECT_EQ(g.peek(0xD000), 0x22);
    EXPECT_EQ(g.workRam()[0x2000], 0x22);
    EXPECT_EQ(g.workRam()[0x1000], 0x11);
    EXPECT_EQ(g.workRam().size(), 0x8000u);
    // Palettes: BCPS with auto-increment, BCPD little-endian colours
    g.write(0xFF68, 0x80 | 0x02);
    g.write(0xFF69, 0x1F);
    g.write(0xFF69, 0x7C);
    EXPECT_EQ(g.peek(0xFF68), 0xC4);                   // index 4, auto-increment, bit 6 set
    EXPECT_EQ(g.ppu().bgPalettes()[1], 0x7C1F);
    g.write(0xFF68, 0x03);
    EXPECT_EQ(g.peek(0xFF69), 0x7C);
    g.write(0xFF6A, 0x80 | 0x3F);
    g.write(0xFF6B, 0x12);
    EXPECT_EQ(g.peek(0xFF6A), 0xC0);                   // wraps to 0
    EXPECT_EQ(g.ppu().objPalettes()[31] >> 8, 0x12);
    // The undocumented registers and RP
    g.write(0xFF72, 0x5A); g.write(0xFF73, 0xA5); g.write(0xFF74, 0x3C); g.write(0xFF75, 0xFF);
    EXPECT_EQ(g.peek(0xFF72), 0x5A);
    EXPECT_EQ(g.peek(0xFF73), 0xA5);
    EXPECT_EQ(g.peek(0xFF74), 0x3C);
    EXPECT_EQ(g.peek(0xFF75), 0xFF);
    g.write(0xFF75, 0x00);
    EXPECT_EQ(g.peek(0xFF75), 0x8F);                   // bits 4-6 only
    g.write(0xFF56, 0xFF);
    EXPECT_EQ(g.peek(0xFF56), 0xFF);
    g.write(0xFF56, 0x00);
    EXPECT_EQ(g.peek(0xFF56), 0x3E);
    // KEY0 is the boot ROM's: ignored after it
    g.write(0xFF4C, 0x04);
    EXPECT_TRUE(g.ppu().cgbEnabled());
}

TEST_CASE(gb_cgb_dmg_compatibility_mode) {
    // A DMG cartridge on the CGB: the colour features off, the boot ROM's default colours
    gb::Gameboy g;
    ASSERT(bootCgb(g, cgbCart({0x18, 0xFE}, 0x00)));
    EXPECT_TRUE(!g.ppu().cgbEnabled());
    EXPECT_EQ(g.cpu().state().e, 0x08);
    EXPECT_EQ(g.peek(0xFF4D), 0xFF);
    EXPECT_EQ(g.peek(0xFF4F), 0xFF);
    EXPECT_EQ(g.peek(0xFF70), 0xFF);
    EXPECT_EQ(g.peek(0xFF68), 0xFF);
    EXPECT_EQ(g.peek(0xFF02), 0x7E);
    EXPECT_EQ(g.ppu().bgPalettes()[1], 0x1BEF);
    EXPECT_EQ(g.ppu().objPalettes()[5], 0x421F);
    g.write(0xFF40, 0x11);
    g.write(0xFF4F, 0x01);                             // ignored
    g.write(0x8000, 0x77);
    EXPECT_EQ(g.ppu().vram()[0x0000], 0x77);
    g.write(0xFF70, 0x03);                             // ignored: $D000 stays bank 1
    EXPECT_EQ(g.wramBank(), 1);
    // The DMG has none of the CGB's registers
    gb::Gameboy d;
    ASSERT(boot(d, cart({0x18, 0xFE})));
    EXPECT_EQ(d.peek(0xFF4F), 0xFF);
    EXPECT_EQ(d.peek(0xFF70), 0xFF);
    EXPECT_EQ(d.peek(0xFF76), 0xFF);
    EXPECT_EQ(d.peek(0xFF72), 0xFF);
}

TEST_CASE(gb_cgb_hdma) {
    gb::Gameboy g;
    ASSERT(bootCgb(g, cgbCart({0x18, 0xFE})));
    g.write(0xFF40, 0x11);                             // the LCD off
    for (int i = 0; i < 0x40; ++i) g.write((uint16_t)(0xC000 + i), (uint8_t)(i + 1));
    // General DMA: 2 blocks of 16 bytes at once, $C000 to $8000 (the low 4 bits of each address dropped)
    g.write(0xFF51, 0xC0); g.write(0xFF52, 0x0F);
    g.write(0xFF53, 0x80); g.write(0xFF54, 0x0F);
    const uint64_t c0 = g.cycles();
    g.write(0xFF55, 0x01);
    EXPECT_EQ(g.cycles() - c0, 4u + 2 * 16 * 2);      // 4 clocks, then 2 per byte
    EXPECT_EQ(g.ppu().vram()[0x00], 1);
    EXPECT_EQ(g.ppu().vram()[0x1F], 0x20);
    EXPECT_EQ(g.ppu().vram()[0x20], 0);
    EXPECT_EQ(g.peek(0xFF55), 0xFF);                   // done
    // HBlank DMA to VRAM bank 1: one block now (the LCD off), then one each HBlank
    g.write(0xFF4F, 0x01);
    g.write(0xFF51, 0xC0); g.write(0xFF52, 0x00);
    g.write(0xFF53, 0x81); g.write(0xFF54, 0x00);
    g.write(0xFF55, 0x82);                             // 3 blocks
    EXPECT_EQ(g.ppu().vram()[0x2100], 1);
    EXPECT_EQ(g.ppu().vram()[0x2110], 0);
    EXPECT_EQ(g.peek(0xFF55), 0x01);                   // running, 2 blocks left
    g.write(0xFF40, 0x91);                             // the LCD on: a block each line's HBlank
    for (int i = 0; i < 3000 && g.peek(0xFF55) != 0xFF; ++i) g.cpu().exec();
    EXPECT_EQ(g.peek(0xFF55), 0xFF);
    EXPECT_EQ(g.ppu().vram()[0x2110], 0x11);
    EXPECT_EQ(g.ppu().vram()[0x212F], 0x30);
    EXPECT_EQ(g.ppu().vram()[0x2130], 0);
    // Stopped with bit 7 clear: HDMA5 reads $80 | the blocks left
    g.write(0xFF55, 0x85);
    g.write(0xFF55, 0x00);
    EXPECT_EQ(g.peek(0xFF55) & 0x80, 0x80);
}

TEST_CASE(gb_cgb_speed_switch) {
    gb::Gameboy g;
    // LD A,1; LDH ($4D),A; STOP; then LDH A,($4D); LD ($C000),A; loop
    ASSERT(bootCgb(g, cgbCart({0x3E, 0x01, 0xE0, 0x4D, 0x10, 0x00, 0xF0, 0x4D, 0xEA, 0x00, 0xC0, 0x18, 0xFE})));
    EXPECT_TRUE(!g.isHighSpeed());
    while (g.cpu().state().pc < 0x0154) g.cpu().exec();
    EXPECT_EQ(g.peek(0xFF4D), 0x7F);                   // armed
    while (g.cpu().state().pc != 0x015B) g.cpu().exec();
    EXPECT_TRUE(g.isHighSpeed());
    EXPECT_EQ(g.peek(0xC000), 0xFE);                   // double speed, the request cleared
    // Twice the CPU's clocks in a frame; the LCD and the APU keep their time
    g.runFrame();
    const uint64_t c0 = g.cycles(), a0 = g.apuCycles();
    g.runFrame();
    EXPECT_NEAR((double)(g.apuCycles() - a0), 70224.0, 24.0);
    EXPECT_NEAR((double)(g.cycles() - c0), 2 * 70224.0, 48.0);
}

TEST_CASE(gb_cgb_rendering) {
    gb::Gameboy g;
    ASSERT(bootCgb(g, cgbCart({0x18, 0xFE})));
    g.write(0xFF40, 0x00);
    // Tile 1 in bank 0: colour 1 everywhere; tile 1 in bank 1: colour 2 everywhere
    for (int i = 0; i < 16; i += 2) { g.ppu().vram()[0x10 + i] = 0xFF; g.ppu().vram()[0x2010 + i + 1] = 0xFF; }
    // The map: tile 1; its attributes: palette 2 (bank 0) for column 0, palette 3 and bank 1 for column 1
    for (int i = 0; i < 32 * 32; ++i) g.ppu().vram()[0x1800 + i] = 1;
    g.ppu().vram()[0x3800] = 0x02;
    g.ppu().vram()[0x3801] = 0x0B;
    g.ppu().bgPalettes()[2 * 4 + 1] = 0x001F;          // red
    g.ppu().bgPalettes()[3 * 4 + 2] = 0x03E0;          // green
    // Two objects overlapping at x 4-7: OAM 0 at X=12 (palette 1, blue), OAM 1 at X=8 (palette 2, white)
    auto* oam = g.ppu().oam();
    const uint8_t objs[8] = {16 + 40, 12, 1, 0x01, 16 + 40, 8, 1, 0x02};
    std::memcpy(oam, objs, 8);
    g.ppu().objPalettes()[1 * 4 + 1] = 0x7C00;         // blue
    g.ppu().objPalettes()[2 * 4 + 1] = 0x7FFF;
    g.write(0xFF40, 0x93);                             // LCD, BG and objects on
    g.runFrame();
    g.runFrame();
    EXPECT_EQ(g.colorScreen()[0], 0x001F);
    EXPECT_EQ(g.colorScreen()[8], 0x03E0);
    EXPECT_EQ(g.colorScreen()[40 * 160 + 2], 0x7FFF);  // OAM 1 alone
    EXPECT_EQ(g.colorScreen()[40 * 160 + 5], 0x7C00);  // OAM 0 wins where they overlap (not the lower X)
    // The tile's priority bit puts it over objects, unless LCDC bit 0 is clear
    g.ppu().vram()[0x3800 + 5 * 32] = 0x82;
    g.runFrame();
    g.runFrame();
    EXPECT_EQ(g.colorScreen()[40 * 160 + 5], 0x001F);
    g.write(0xFF40, 0x92);
    g.runFrame();
    g.runFrame();
    EXPECT_EQ(g.colorScreen()[40 * 160 + 5], 0x7C00);
}

TEST_CASE(gb_cgb_pcm_and_wave_ram) {
    gb::Gameboy g;
    ASSERT(bootCgb(g, cgbCart({0x18, 0xFE})));
    // Square 2 at full volume, 50% duty: PCM12's high nibble 0 or 15
    g.write(0xFF26, 0x80);
    g.write(0xFF16, 0x80);
    g.write(0xFF17, 0xF0);
    g.write(0xFF18, 0x00);
    g.write(0xFF19, 0x87);
    int seen = 0;
    bool only0or15 = true;
    for (int i = 0; i < 2000; ++i) {
        g.cpu().exec();
        const uint8_t v = g.read<gb::OamCorruption::Read>(0xFF76);
        seen |= 1 << (v >> 4);
        if ((v >> 4) != 0 && (v >> 4) != 0x0F) only0or15 = false;
    }
    EXPECT_TRUE(only0or15);
    EXPECT_EQ(seen, (1 << 0) | (1 << 15));
    // Wave RAM while the wave channel plays: on the CGB the byte being played, any time (not $FF)
    for (int i = 0; i < 16; ++i) g.write((uint16_t)(0xFF30 + i), (uint8_t)(0x11 * i));
    g.write(0xFF1A, 0x80);
    g.write(0xFF1C, 0x20);
    g.write(0xFF1D, 0x00);
    g.write(0xFF1E, 0x87);
    bool sane = true;
    for (int i = 0; i < 100; ++i) {
        g.cpu().exec();
        const uint8_t w = g.read<gb::OamCorruption::Read>(0xFF30);
        if (w % 0x11 != 0 || w == 0xFF) sane = false;
    }
    EXPECT_TRUE(sane);
}
