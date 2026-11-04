#include "test_harness.h"
#include "plugins/devices/vera/main/vera.h"
#include <cmath>
#include <vector>

// VERA (#16, Commander X16): address ports, composer, layers, sprites, IRQs,
// FX, PSG/PCM audio and SPI.

namespace {
class MockSignal : public ISignalLine {
public:
    bool get() const override { return m_level; }
    void set(bool level) override { m_level = level; }
    void pulse() override {}
    bool m_level = false;
};

constexpr uint64_t CYCLES_PER_LINE  = 256;                        // 800 pixel clocks at 25 MHz, 8 MHz CPU
constexpr uint64_t CYCLES_PER_FRAME = CYCLES_PER_LINE * 525;

struct V {
    Vera v;
    MockSignal irq;
    V() { v.setIrqLine(&irq); }
    void w(int r, uint8_t x) { v.ioWrite(nullptr, 0x9F20 + r, x); }
    uint8_t r(int reg) { uint8_t x = 0; v.ioRead(nullptr, 0x9F20 + reg, &x); return x; }
    uint8_t p(int reg) { uint8_t x = 0; v.ioPeek(nullptr, 0x9F20 + reg, &x); return x; }
    void dcsel(int d, int addrsel = 0) { w(Vera::CTRL, (uint8_t)((d << 1) | addrsel)); }
    /** Point port sel at addr with increment table index inc (2 = +1, 3 = -1, ...). */
    void setAddr(int sel, uint32_t addr, int inc) {
        dcsel(0, sel);
        w(Vera::ADDRx_L, addr & 0xFF);
        w(Vera::ADDRx_M, (addr >> 8) & 0xFF);
        w(Vera::ADDRx_H, (uint8_t)(((addr >> 16) & 1) | (inc << 3)));
    }
    uint32_t addr() { return r(0) | (r(1) << 8) | ((r(2) & 1) << 16); }
    std::vector<uint32_t> frame() {
        std::vector<uint32_t> buf(640 * 480);
        v.renderFrame(buf.data());
        return buf;
    }
};
}  // namespace

TEST_CASE(vera_data0_auto_increment) {
    V t;
    t.setAddr(0, 0x01000, 2);   // +1
    t.w(Vera::DATA0, 0x11); t.w(Vera::DATA0, 0x22); t.w(Vera::DATA0, 0x33);
    ASSERT_EQ((int)t.v.vramPeek(0x1000), 0x11);
    ASSERT_EQ((int)t.v.vramPeek(0x1001), 0x22);
    ASSERT_EQ((int)t.v.vramPeek(0x1002), 0x33);
    ASSERT_EQ(t.addr(), 0x1003u);
    ASSERT_EQ((int)(t.r(Vera::ADDRx_H) >> 3), 2);   // increment index reads back

    t.setAddr(0, 0x02000, 4);   // +2
    t.w(Vera::DATA0, 0xA1); t.w(Vera::DATA0, 0xA2);
    ASSERT_EQ((int)t.v.vramPeek(0x2000), 0xA1);
    ASSERT_EQ((int)t.v.vramPeek(0x2002), 0xA2);
    ASSERT_EQ(t.addr(), 0x2004u);

    t.setAddr(0, 0x03000, 22);  // +40
    t.w(Vera::DATA0, 0xB1); t.w(Vera::DATA0, 0xB2);
    ASSERT_EQ((int)t.v.vramPeek(0x3000), 0xB1);
    ASSERT_EQ((int)t.v.vramPeek(0x3028), 0xB2);
    ASSERT_EQ(t.addr(), 0x3050u);

    t.setAddr(0, 0x04000, 3);   // -1
    t.w(Vera::DATA0, 0xC1); t.w(Vera::DATA0, 0xC2);
    ASSERT_EQ((int)t.v.vramPeek(0x4000), 0xC1);
    ASSERT_EQ((int)t.v.vramPeek(0x3FFF), 0xC2);
    ASSERT_EQ(t.addr(), 0x3FFEu);

    // reads through DATA0 return successive bytes
    t.setAddr(0, 0x01000, 2);
    ASSERT_EQ((int)t.r(Vera::DATA0), 0x11);
    ASSERT_EQ((int)t.r(Vera::DATA0), 0x22);
    ASSERT_EQ((int)t.r(Vera::DATA0), 0x33);

    // the 17-bit address wraps
    t.setAddr(0, 0x1FFFF, 2);
    t.w(Vera::DATA0, 0x5A);
    ASSERT_EQ(t.addr(), 0u);
}

TEST_CASE(vera_data1_and_addrsel) {
    V t;
    t.setAddr(0, 0x00100, 2);
    t.setAddr(1, 0x10200, 2);   // leaves ADDRSEL = 1
    ASSERT_EQ((int)(t.r(Vera::CTRL) & 1), 1);
    ASSERT_EQ(t.addr(), 0x10200u);
    t.w(Vera::DATA1, 0x77);
    t.w(Vera::DATA0, 0x66);
    ASSERT_EQ((int)t.v.vramPeek(0x10200), 0x77);
    ASSERT_EQ((int)t.v.vramPeek(0x00100), 0x66);
    ASSERT_EQ(t.v.address(1), 0x10201u);
    ASSERT_EQ(t.v.address(0), 0x00101u);
    t.dcsel(0, 0);
    ASSERT_EQ(t.addr(), 0x00101u);
}

TEST_CASE(vera_reset_values) {
    V t;
    ASSERT_EQ((int)t.r(Vera::CTRL), 0);
    ASSERT_EQ((int)t.r(Vera::DC0), 0x00);    // DC_VIDEO
    ASSERT_EQ((int)t.r(Vera::DC1), 128);     // HSCALE
    ASSERT_EQ((int)t.r(Vera::DC2), 128);     // VSCALE
    ASSERT_EQ((int)t.r(Vera::DC3), 0);       // border
    t.dcsel(1);
    ASSERT_EQ((int)t.r(Vera::DC0), 0);       // HSTART
    ASSERT_EQ((int)t.r(Vera::DC1), 640 >> 2);
    ASSERT_EQ((int)t.r(Vera::DC2), 0);
    ASSERT_EQ((int)t.r(Vera::DC3), 480 >> 1);
    ASSERT_EQ((int)(t.r(Vera::CTRL) >> 1), 1);
    t.dcsel(63);
    ASSERT_EQ((int)t.r(Vera::DC0), 'V');
    ASSERT_EQ((int)t.r(Vera::DC1), 47);
    ASSERT_EQ((int)t.r(Vera::DC2), 0);
    ASSERT_EQ((int)t.r(Vera::DC3), 2);
    ASSERT_EQ((int)t.r(Vera::AUDIO_CTRL), 0x40);   // FIFO empty
    ASSERT_EQ((int)t.r(Vera::SPI_DATA), 0xFF);

    // CTRL bit 7 resets the chip
    t.dcsel(0);
    t.w(Vera::DC1, 64);
    t.w(Vera::CTRL, 0x80);
    ASSERT_EQ((int)t.r(Vera::DC1), 128);
}

TEST_CASE(vera_palette_write_changes_frame) {
    V t;
    t.dcsel(0);
    t.w(Vera::DC0, 0x01);              // VGA, no layers: colour 0 everywhere
    t.v.tick(CYCLES_PER_FRAME * 2);
    ASSERT_EQ(t.frame()[240 * 640 + 320], 0xFF000000u);

    t.setAddr(0, 0x1FA00, 2);
    t.w(Vera::DATA0, 0x52);            // GGGGBBBB
    t.w(Vera::DATA0, 0x0C);            // ----RRRR
    t.v.tick(CYCLES_PER_FRAME * 2);
    ASSERT_EQ(t.frame()[240 * 640 + 320], 0xFFCC5522u);
    ASSERT_EQ(t.v.frameCount() >= 3u, true);

    // output mode 0: blue screen
    t.dcsel(0);
    t.w(Vera::DC0, 0x00);
    t.v.tick(CYCLES_PER_FRAME * 2);
    ASSERT_EQ(t.frame()[0], 0xFF0000FFu);
}

TEST_CASE(vera_text_mode_glyph) {
    V t;
    // X16 default: layer 1, 128x64 map at $1B000, 1bpp 8x8 tiles at $1F000
    t.w(Vera::L1_CONFIG, 0x60);
    t.w(Vera::L1_MAPBASE, 0xD8);
    t.w(Vera::L1_TILEBASE, 0xF8);
    t.setAddr(0, 0x1B000, 2);
    t.w(Vera::DATA0, 0x01);            // tile 1
    t.w(Vera::DATA0, 0x61);            // bg 6 (blue), fg 1 (white)
    t.setAddr(0, 0x1F008, 2);
    t.w(Vera::DATA0, 0x80);            // glyph 1 row 0: leftmost pixel set
    t.dcsel(0);
    t.w(Vera::DC0, 0x21);              // VGA + layer 1
    t.v.tick(CYCLES_PER_FRAME * 2 + 1000);
    auto f = t.frame();
    ASSERT_EQ(f[0], 0xFFFFFFFFu);
    ASSERT_EQ(f[1], 0xFF0000AAu);

    // HSCALE 64 doubles pixels horizontally
    t.w(Vera::DC1, 64);
    t.v.tick(CYCLES_PER_FRAME * 2);
    f = t.frame();
    ASSERT_EQ(f[0], 0xFFFFFFFFu);
    ASSERT_EQ(f[1], 0xFFFFFFFFu);
    ASSERT_EQ(f[2], 0xFF0000AAu);

    // border outside HSTART/HSTOP
    t.w(Vera::DC1, 128);
    t.w(Vera::DC3, 2);                 // border colour 2 (red)
    t.dcsel(1);
    t.w(Vera::DC0, 8 >> 2);            // HSTART = 8
    t.v.tick(CYCLES_PER_FRAME * 2);
    f = t.frame();
    ASSERT_EQ(f[0], 0xFF880000u);
    ASSERT_EQ(f[8], 0xFFFFFFFFu);
}

TEST_CASE(vera_sprite_over_layer) {
    V t;
    t.w(Vera::L1_CONFIG, 0x60);
    t.w(Vera::L1_MAPBASE, 0xD8);
    t.w(Vera::L1_TILEBASE, 0xF8);
    // map row 6 (y 48-55): tile 0 with bg 6; glyph 0 blank
    for (int i = 0; i < 128; i++) {
        t.v.vramPoke(0x1B000 + 6 * 256 + i * 2, 0);
        t.v.vramPoke(0x1B000 + 6 * 256 + i * 2 + 1, 0x60);
    }
    for (int i = 0; i < 8; i++) t.v.vramPoke(0x1F000 + i, 0);
    // sprite image at $10000: 8x8, 8bpp, colour 2
    for (int i = 0; i < 64; i++) t.v.vramPoke(0x10000 + i, 2);
    // sprite 0 attributes through DATA0
    t.setAddr(0, 0x1FC00, 2);
    const uint8_t attr[8] = {0x00, 0x88, 100, 0, 50, 0, 0x0C, 0x00};   // 8bpp at $10000, x 100, y 50, z 3
    for (uint8_t b : attr) t.w(Vera::DATA0, b);
    t.dcsel(0);
    t.w(Vera::DC0, 0x61);              // VGA + layer 1 + sprites
    t.v.tick(CYCLES_PER_FRAME * 2 + 1000);
    auto f = t.frame();
    ASSERT_EQ(f[50 * 640 + 99],  0xFF0000AAu);   // layer
    ASSERT_EQ(f[50 * 640 + 100], 0xFF880000u);   // sprite
    ASSERT_EQ(f[57 * 640 + 107], 0xFF880000u);
    ASSERT_EQ(f[50 * 640 + 108], 0xFF0000AAu);

    // sprites off: the layer shows
    t.w(Vera::DC0, 0x21);
    t.v.tick(CYCLES_PER_FRAME * 2);
    ASSERT_EQ(t.frame()[50 * 640 + 100], 0xFF0000AAu);
}

TEST_CASE(vera_sprite_collision_irq) {
    V t;
    for (int i = 0; i < 64; i++) t.v.vramPoke(0x10000 + i, 2);
    const uint8_t a0[8] = {0x00, 0x88, 10, 0, 10, 0, 0x1C, 0x00};   // mask 1, z 3
    const uint8_t a1[8] = {0x00, 0x88, 12, 0, 12, 0, 0x1C, 0x00};
    for (int i = 0; i < 8; i++) { t.v.vramPoke(0x1FC00 + i, a0[i]); t.v.vramPoke(0x1FC08 + i, a1[i]); }
    t.dcsel(0);
    t.w(Vera::DC0, 0x41);
    t.w(Vera::IEN, Vera::INT_SPRCOL);
    t.v.tick(CYCLES_PER_LINE * 481);
    ASSERT_EQ((int)(t.r(Vera::ISR) & 0xF4), 0x14);
    ASSERT(t.irq.m_level);
}

TEST_CASE(vera_line_irq) {
    V t;
    t.w(Vera::IRQLINE_L, 100);
    t.w(Vera::IEN, Vera::INT_LINE);
    t.v.tick(CYCLES_PER_LINE * 99);
    ASSERT_EQ((int)t.r(Vera::IRQLINE_L), 98);   // SCANLINE_L
    ASSERT(!t.irq.m_level);
    ASSERT_EQ((int)(t.r(Vera::ISR) & 0x03), 0);
    t.v.tick(300);
    ASSERT_EQ((int)t.r(Vera::IRQLINE_L), 100);
    ASSERT(t.irq.m_level);
    ASSERT_EQ((int)(t.r(Vera::ISR) & 0x03), Vera::INT_LINE);
    t.w(Vera::ISR, Vera::INT_LINE);             // write 1 to clear
    ASSERT(!t.irq.m_level);
    ASSERT_EQ((int)(t.r(Vera::ISR) & 0x03), 0);

    // line 300: bit 8 in IEN bit 7; SCANLINE bit 8 reads in IEN bit 6
    t.w(Vera::IRQLINE_L, 300 & 0xFF);
    t.w(Vera::IEN, 0x80 | Vera::INT_LINE);
    t.v.tick(CYCLES_PER_LINE * 200);
    ASSERT(t.irq.m_level);
    ASSERT_EQ(t.v.scanline(), 300);
    ASSERT_EQ((int)(t.r(Vera::IEN) & 0xC0), 0xC0);
}

TEST_CASE(vera_vsync_irq) {
    V t;
    t.w(Vera::IEN, Vera::INT_VSYNC);
    t.v.tick(CYCLES_PER_LINE * 479);
    ASSERT(!t.irq.m_level);
    t.v.tick(CYCLES_PER_LINE * 2);
    ASSERT(t.irq.m_level);
    ASSERT_EQ((int)(t.r(Vera::ISR) & 0x01), 1);
    t.w(Vera::ISR, 0x01);
    ASSERT(!t.irq.m_level);
    // the next frame raises it again
    t.v.tick(CYCLES_PER_FRAME);
    ASSERT(t.irq.m_level);
    // AFLOW is pending (FIFO empty) but not enabled: no IRQ from it
    t.w(Vera::ISR, 0x01);
    ASSERT_EQ((int)(t.r(Vera::ISR) & Vera::INT_AFLOW), Vera::INT_AFLOW);
    ASSERT(!t.irq.m_level);
}

TEST_CASE(vera_peek_has_no_side_effects) {
    V t;
    t.v.vramPoke(0x1000, 0xAB);
    t.v.vramPoke(0x1001, 0xCD);
    t.setAddr(0, 0x1000, 2);
    ASSERT_EQ((int)t.p(Vera::DATA0), 0xAB);
    ASSERT_EQ((int)t.p(Vera::DATA0), 0xAB);
    ASSERT_EQ(t.v.address(0), 0x1000u);
    ASSERT_EQ((int)t.r(Vera::DATA0), 0xAB);
    ASSERT_EQ(t.v.address(0), 0x1001u);
    ASSERT_EQ((int)t.p(Vera::DATA0), 0xCD);
    // every register peeks as it reads
    for (int reg = 0; reg < 0x20; reg++) {
        if (reg == Vera::DATA0 || reg == Vera::DATA1) continue;
        ASSERT_EQ((int)t.p(reg), (int)t.r(reg));
    }
    DeviceInfo info;
    t.v.getDeviceInfo(info);
    ASSERT_EQ((int)info.registers.size(), 32);
    ASSERT_EQ(t.v.address(0), 0x1001u);
}

TEST_CASE(vera_psg_voice_produces_sound) {
    V t;
    float buf[4096];
    t.v.tick(8000);
    int n = t.v.pullSamples(buf, 4096);
    ASSERT(n > 0);
    for (int i = 0; i < n; i++) ASSERT(buf[i] == 0.0f);   // silent after reset

    t.setAddr(0, 0x1F9C0, 2);          // voice 0
    t.w(Vera::DATA0, 1000 & 0xFF);
    t.w(Vera::DATA0, 1000 >> 8);
    t.w(Vera::DATA0, 0xC0 | 63);       // left + right, full volume
    t.w(Vera::DATA0, 0x00 | 32);       // pulse, 50% width
    ASSERT_EQ(t.v.psg().channel(0).freq, 1000);
    t.v.tick(80000);                   // 10 ms
    n = t.v.pullSamples(buf, 4096);
    ASSERT(n >= 480 && n <= 500);      // 48828 Hz
    float peak = 0;
    for (int i = 0; i < n; i++) peak = std::max(peak, std::fabs(buf[i]));
    ASSERT(peak > 0.05f);
    ASSERT(peak <= 1.0f);
    ASSERT_EQ(t.v.nativeSampleRate(), 48828);

    float lr[64];
    t.v.tick(2000);
    ASSERT(t.v.pullStereoSamples(lr, 32) > 0);
}

TEST_CASE(vera_pcm_fifo_aflow_and_samples) {
    V t;
    ASSERT_EQ((int)(t.r(Vera::ISR) & Vera::INT_AFLOW), Vera::INT_AFLOW);
    t.w(Vera::AUDIO_CTRL, 0x0F);       // 8-bit mono, full volume
    t.w(Vera::AUDIO_RATE_REG, 0);      // stopped while filling
    for (int i = 0; i < 1100; i++) t.w(Vera::AUDIO_DATA, 0x40);
    ASSERT_EQ((int)(t.r(Vera::ISR) & Vera::INT_AFLOW), 0);
    ASSERT_EQ((int)(t.r(Vera::AUDIO_CTRL) & 0xC0), 0);   // neither full nor empty
    t.w(Vera::IEN, Vera::INT_AFLOW);
    ASSERT(!t.irq.m_level);

    t.w(Vera::AUDIO_RATE_REG, 128);    // one FIFO byte per output sample
    float drain[8192];
    t.v.pullSamples(drain, 8192);
    t.v.tick(200 * 512 * 8 / 25 + 100);   // ~200 samples
    ASSERT(t.v.pcm().fifoCount() < 1024);
    ASSERT_EQ((int)(t.r(Vera::ISR) & Vera::INT_AFLOW), Vera::INT_AFLOW);
    ASSERT(t.irq.m_level);
    float buf[512];
    int n = t.v.pullSamples(buf, 512);
    ASSERT(n >= 190);
    ASSERT(buf[n - 1] > 0.3f);

    // FIFO reset
    t.w(Vera::AUDIO_CTRL, 0x80 | 0x0F);
    ASSERT_EQ(t.v.pcm().fifoCount(), 0u);
    ASSERT_EQ((int)(t.r(Vera::AUDIO_CTRL) & 0x40), 0x40);
}

TEST_CASE(vera_fx_multiplier) {
    V t;
    t.dcsel(2);
    t.w(Vera::DC3, 0x10);              // FX_MULT: multiplier on
    t.w(Vera::DC0, 0x40);              // FX_CTRL: cache write
    t.dcsel(6);
    t.w(Vera::DC0, 0x2C); t.w(Vera::DC1, 0x01);   // 300
    t.w(Vera::DC2, 0xF9); t.w(Vera::DC3, 0xFF);   // -7
    t.setAddr(0, 0x02001, 0);
    t.w(Vera::DATA0, 0x00);            // all four bytes, at the 32-bit aligned address
    uint32_t r = t.v.vramPeek(0x2000) | t.v.vramPeek(0x2001) << 8 | t.v.vramPeek(0x2002) << 16 |
                 (uint32_t)t.v.vramPeek(0x2003) << 24;
    ASSERT_EQ((int32_t)r, -2100);

    t.dcsel(2);
    t.w(Vera::DC3, 0x10 | 0x40);       // accumulate
    ASSERT_EQ(t.v.fxAccumulator(), -2100);
    t.setAddr(0, 0x02004, 0);
    t.w(Vera::DATA0, 0x00);
    r = t.v.vramPeek(0x2004) | t.v.vramPeek(0x2005) << 8 | t.v.vramPeek(0x2006) << 16 |
        (uint32_t)t.v.vramPeek(0x2007) << 24;
    ASSERT_EQ((int32_t)r, -4200);

    // reading DCSEL 6 $9F29 resets the accumulator; a peek does not
    t.dcsel(6);
    t.p(Vera::DC0);
    ASSERT_EQ(t.v.fxAccumulator(), -2100);
    t.r(Vera::DC0);
    ASSERT_EQ(t.v.fxAccumulator(), 0);
    // reading $9F2A accumulates
    t.r(Vera::DC1);
    ASSERT_EQ(t.v.fxAccumulator(), -2100);
}

TEST_CASE(vera_fx_cache_fill_and_write) {
    V t;
    const uint8_t src[4] = {0x11, 0x22, 0x33, 0x44};
    for (int i = 0; i < 4; i++) t.v.vramPoke(0x3000 + i, src[i]);
    t.dcsel(2);
    t.w(Vera::DC3, 0x00);              // cache byte index 0
    t.w(Vera::DC0, 0x20);              // cache fill
    t.setAddr(0, 0x3000, 2);
    for (int i = 0; i < 4; i++) ASSERT_EQ((int)t.r(Vera::DATA0), (int)src[i]);
    for (int i = 0; i < 4; i++) ASSERT_EQ((int)t.v.fxCache(i), (int)src[i]);

    t.dcsel(2);
    t.w(Vera::DC0, 0x40);              // cache write
    t.v.vramPoke(0x4001, 0x00);
    t.setAddr(0, 0x4000, 0);
    t.w(Vera::DATA0, 0x0C);            // nibble mask: byte 1 untouched
    ASSERT_EQ((int)t.v.vramPeek(0x4000), 0x11);
    ASSERT_EQ((int)t.v.vramPeek(0x4001), 0x00);
    ASSERT_EQ((int)t.v.vramPeek(0x4002), 0x33);
    ASSERT_EQ((int)t.v.vramPeek(0x4003), 0x44);

    // FX off: plain writes again
    t.dcsel(2);
    t.w(Vera::DC0, 0x00);
    t.setAddr(0, 0x4001, 2);
    t.w(Vera::DATA0, 0x99);
    ASSERT_EQ((int)t.v.vramPeek(0x4001), 0x99);
    ASSERT_EQ((int)t.v.vramPeek(0x4002), 0x33);
}

TEST_CASE(vera_fx_line_draw_address) {
    V t;
    // line mode: ADDR1 steps by ADDR0's increment each time the X position
    // overflows; X increment 0.5 → every second DATA1 access
    t.setAddr(0, 0x0000, 22);          // ADDR0 increment +40 (the "minor" step)
    t.setAddr(1, 0x1000, 2);           // ADDR1 +1
    t.dcsel(2);
    t.w(Vera::DC0, 0x01);              // FX_CTRL: line draw
    t.dcsel(3);
    t.w(Vera::DC0, 0x00);              // FX_X_INCR_L
    t.w(Vera::DC1, 0x01);              // FX_X_INCR_H: 0.5 (0x100 << 7 = 0x8000)
    t.dcsel(0, 1);
    t.w(Vera::DATA1, 1);
    ASSERT_EQ(t.v.address(1), 0x1001u + 40u);   // 0.5 + 0.5 overflows on the first write
    t.w(Vera::DATA1, 2);
    ASSERT_EQ(t.v.address(1), 0x1002u + 40u);
    t.w(Vera::DATA1, 3);
    ASSERT_EQ(t.v.address(1), 0x1003u + 80u);
}

TEST_CASE(vera_ntsc_field_and_spi) {
    V t;
    t.dcsel(0);
    t.w(Vera::DC0, 0x02);              // NTSC composite
    ASSERT_EQ(t.v.currentField(), 0);
    t.v.tick((uint64_t)794 * 525 * 8 / 25 + 100);
    ASSERT_EQ(t.v.currentField(), 1);
    ASSERT_EQ((int)(t.r(Vera::DC0) & 0x80), 0x80);
    t.w(Vera::DC0, 0x02);              // the field bit is read-only
    ASSERT_EQ(t.v.currentField(), 1);

    // SPI with no card: a transfer goes busy, then reads $FF
    t.w(Vera::SPI_CTRL, 0x01);
    ASSERT_EQ((int)t.r(Vera::SPI_CTRL), 0x01);
    t.w(Vera::SPI_DATA, 0x40);
    ASSERT_EQ((int)(t.r(Vera::SPI_CTRL) & 0x80), 0x80);
    t.v.tick(10);
    ASSERT_EQ((int)(t.r(Vera::SPI_CTRL) & 0x80), 0);
    ASSERT_EQ((int)t.r(Vera::SPI_DATA), 0xFF);
}
