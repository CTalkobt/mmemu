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
