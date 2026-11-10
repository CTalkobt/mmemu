#include "test_harness.h"
#include "plugins/machines/snes/main/machine_snes.h"
#include "plugins/machines/snes/main/snes_apu.h"
#include "plugins/machines/snes/main/snes_ppu.h"
#include "libcore/main/machine_desc.h"
#include "libdevices/main/io_registry.h"
#include "libmem/main/ibus.h"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <initializer_list>
#include <string>
#include <vector>

namespace {

constexpr uint64_t FRAME = 1364ull * 262 + 262 * 40;   // master clocks a frame (with refresh)

/** A LoROM image: 64 KB, a valid header, code assembled at $00:8000. */
struct Rom {
    std::vector<uint8_t> img = std::vector<uint8_t>(0x10000, 0x00);
    uint16_t pc = 0x8000;
    Rom() {
        const char* title = "MMSIM TEST           ";
        for (int i = 0; i < 21; ++i) img[0x7FC0 + i] = (uint8_t)title[i];
        img[0x7FD5] = 0x20;                    // LoROM
        img[0x7FD7] = 0x06;                    // 64 KB
        img[0x7FD8] = 0x00;
        img[0x7FDC] = 0xFF; img[0x7FDD] = 0xFF; img[0x7FDE] = 0x00; img[0x7FDF] = 0x00;
        vector(0xFFFC, 0x8000);                // emulation reset
    }
    void vector(uint16_t at, uint16_t to) { img[at - 0x8000] = (uint8_t)to; img[at - 0x8000 + 1] = (uint8_t)(to >> 8); }
    Rom& b(std::initializer_list<int> bytes) { for (int v : bytes) img[(size_t)(pc++ - 0x8000)] = (uint8_t)v; return *this; }
    Rom& at(uint16_t a) { pc = a; return *this; }
    // Native mode, 8-bit A and X/Y, stack at $1FFF, D = 0
    Rom& init() { return b({0x78, 0x18, 0xFB, 0xC2, 0x30, 0xA2, 0xFF, 0x1F, 0x9A, 0xE2, 0x30}); }
    Rom& lda(uint8_t v) { return b({0xA9, v}); }
    Rom& sta(uint16_t a) { return b({0x8D, a & 0xFF, a >> 8}); }
    Rom& stz(uint16_t a) { return b({0x9C, a & 0xFF, a >> 8}); }
    Rom& ldaA(uint16_t a) { return b({0xAD, a & 0xFF, a >> 8}); }
    Rom& stal(uint32_t a) { return b({0x8F, (int)(a & 0xFF), (int)((a >> 8) & 0xFF), (int)(a >> 16)}); }
    Rom& ldal(uint32_t a) { return b({0xAF, (int)(a & 0xFF), (int)((a >> 8) & 0xFF), (int)(a >> 16)}); }
    Rom& self() { return b({0x80, 0xFE}); }   // BRA *
};

uint64_t run(MachineDescriptor* m, uint64_t clocks) {
    uint64_t t = 0;
    while (t < clocks) t += (uint64_t)m->schedulerStep(*m);
    return t;
}

template <class T> T* device(MachineDescriptor* m) {
    std::vector<IOHandler*> hs;
    m->ioRegistry->enumerate(hs);
    for (auto* h : hs) if (auto* d = dynamic_cast<T*>(h)) return d;
    return nullptr;
}

MachineDescriptor* boot(const Rom& r) {
    std::string err;
    MachineDescriptor* m = snes::create(false, &err);
    if (!m) return nullptr;
    if (!snes::insertCartridge(m, r.img, "t.sfc", err)) { std::printf("insert: %s\n", err.c_str()); delete m; return nullptr; }
    return m;
}

}  // namespace

// The header decides the map; work RAM and its mirrors
TEST_CASE(snes_lorom_wram) {
    Rom r;
    r.init().lda(0x42).stal(0x7E0010).ldaA(0x0010).stal(0x7E0011)
     .lda(0x99).sta(0x1FF0).ldal(0x7E1FF0).stal(0x7F0000).self();
    MachineDescriptor* m = boot(r);
    ASSERT(m);
    EXPECT_EQ(snes::cartridgeInfo(m), std::string("t.sfc (MMSIM TEST, LoROM, 64K ROM)"));
    run(m, 20000);
    IBus* bus = m->buses[0].bus;
    EXPECT_EQ(bus->peek8(0x7E0011), 0x42);
    EXPECT_EQ(bus->peek8(0x7F0000), 0x99);
    EXPECT_EQ(bus->peek8(0x800010), 0x42);          // bank $80 mirrors the low 8 KB
    delete m;
}

// HiROM: the header at $FFC0, ROM at $C0:0000
TEST_CASE(snes_hirom) {
    std::vector<uint8_t> img(0x20000, 0xEA);
    const char* title = "HIROM TEST           ";
    for (int i = 0; i < 21; ++i) img[0xFFC0 + i] = (uint8_t)title[i];
    img[0xFFD5] = 0x21;
    img[0xFFD7] = 0x07;
    img[0xFFD6] = 0x01;                            // ROM + RAM, no battery
    img[0xFFD8] = 0x03;                            // 8 KB of RAM
    img[0xFFDC] = 0xFF; img[0xFFDD] = 0xFF; img[0xFFDE] = 0; img[0xFFDF] = 0;
    img[0xFFFC] = 0x00; img[0xFFFD] = 0x80;
    img[0x8000] = 0x78;                            // SEI at $00:8000
    img[0x1234] = 0x5A;
    img[0x11234] = 0xA5;
    std::string err;
    MachineDescriptor* m = snes::create(false, &err);
    ASSERT(snes::insertCartridge(m, img, "h.sfc", err));
    EXPECT_EQ(snes::cartridgeInfo(m), std::string("h.sfc (HIROM TEST, HiROM, 128K ROM, 8K RAM)"));
    IBus* bus = m->buses[0].bus;
    EXPECT_EQ(bus->peek8(0xC01234), 0x5A);
    EXPECT_EQ(bus->peek8(0xC11234), 0xA5);
    EXPECT_EQ(bus->peek8(0x408000), 0x78);
    EXPECT_EQ(bus->peek8(0x008000), 0x78);                 // $00-$3F:$8000 shows the same 32 KB
    bus->write8(0x306000, 0x77);                   // SRAM at $20-$3F:$6000
    EXPECT_EQ(bus->peek8(0xB06000), 0x77);
    delete m;
}

// Multiply and divide ($4202-$4206 -> $4214-$4217)
TEST_CASE(snes_mul_div) {
    Rom r;
    r.init().lda(12).sta(0x4202).lda(34).sta(0x4203).b({0xEA, 0xEA, 0xEA, 0xEA})
     .ldaA(0x4216).sta(0x0000).ldaA(0x4217).sta(0x0001)
     .lda(0x10).sta(0x4204).lda(0x27).sta(0x4205).lda(100).sta(0x4206)        // $2710 = 10000 / 100
     .b({0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA})
     .ldaA(0x4214).sta(0x0002).ldaA(0x4215).sta(0x0003).ldaA(0x4216).sta(0x0004).self();
    MachineDescriptor* m = boot(r);
    ASSERT(m);
    run(m, 20000);
    IBus* bus = m->buses[0].bus;
    EXPECT_EQ(bus->peek8(0x0000) | bus->peek8(0x0001) << 8, 408);
    EXPECT_EQ(bus->peek8(0x0002) | bus->peek8(0x0003) << 8, 100);
    EXPECT_EQ(bus->peek8(0x0004), 0);
    delete m;
}

// One NMI a frame at vertical blank; a frame is 262 lines of 1364 clocks
TEST_CASE(snes_vblank_nmi) {
    Rom r;
    r.init().lda(0x80).sta(0x4200).b({0xCB, 0x80, 0xFD});             // WAI; BRA back
    r.at(0x9000).b({0xEE, 0x00, 0x00, 0xAD, 0x10, 0x42, 0x40});       // INC $0000; LDA $4210; RTI
    r.vector(0xFFEA, 0x9000);
    MachineDescriptor* m = boot(r);
    ASSERT(m);
    run(m, FRAME * 4);
    EXPECT_EQ(m->buses[0].bus->peek8(0x0000), 4);
    EXPECT_EQ(device<snes::Ppu>(m)->frameCount(), 4u);
    delete m;
}

// The sound CPU's IPL ROM answers $AA $BB on ports 0 and 1
TEST_CASE(snes_apu_ipl_handshake) {
    Rom r;
    r.init();
    const uint16_t loop = r.pc;
    r.ldaA(0x2140).b({0xC9, 0xAA, 0xD0, 0xF9}).ldaA(0x2141).b({0xC9, 0xBB, 0xD0, 0xF2}).lda(1).sta(0x0000).self();
    (void)loop;
    MachineDescriptor* m = boot(r);
    ASSERT(m);
    run(m, FRAME);
    EXPECT_EQ(m->buses[0].bus->peek8(0x0000), 1);
    delete m;
}

// Upload a program through the IPL protocol; it answers on port 0
TEST_CASE(snes_apu_upload) {
    snes::Apu apu;
    uint64_t t = 0;
    auto runFor = [&](uint64_t c) { t += c; apu.runTo(t); };
    auto waitPort0 = [&](uint8_t v) { for (int i = 0; i < 1000 && apu.cpuRead(0) != v; ++i) runFor(16); return apu.cpuRead(0) == v; };
    ASSERT(waitPort0(0xAA));
    ASSERT_EQ(apu.cpuRead(1), 0xBB);
    // MOV A,#$5A; MOV $F4,A; BRA *
    const uint8_t prog[] = {0xE8, 0x5A, 0xC4, 0xF4, 0x2F, 0xFE};
    apu.cpuWrite(2, 0x00); apu.cpuWrite(3, 0x02); apu.cpuWrite(1, 0x01); apu.cpuWrite(0, 0xCC);
    ASSERT(waitPort0(0xCC));
    for (int i = 0; i < (int)sizeof prog; ++i) {
        apu.cpuWrite(1, prog[i]);
        apu.cpuWrite(0, (uint8_t)i);
        ASSERT(waitPort0((uint8_t)i));
    }
    apu.cpuWrite(2, 0x00); apu.cpuWrite(3, 0x02); apu.cpuWrite(1, 0x00); apu.cpuWrite(0, (uint8_t)(sizeof prog + 1));
    runFor(2000);
    EXPECT_EQ(apu.ram()[0x0200], 0xE8);
    EXPECT_EQ(apu.cpuRead(0), 0x5A);
}

// The DSP plays a looped BRR square wave on voice 0
TEST_CASE(snes_dsp_voice) {
    snes::Apu apu;
    uint8_t* ram = apu.ram();
    // DIR at $0300: sample 0 starts and loops at $0400
    ram[0x0300] = 0x00; ram[0x0301] = 0x04; ram[0x0302] = 0x00; ram[0x0303] = 0x04;
    ram[0x0400] = 0xC3;                         // shift 12, filter 0, loop, end
    for (int i = 1; i <= 4; ++i) ram[0x0400 + i] = 0x77;
    for (int i = 5; i <= 8; ++i) ram[0x0400 + i] = 0x99;
    auto& d = apu.dsp();
    d.write(0x00, 0x7F); d.write(0x01, 0x7F);   // VOL
    d.write(0x02, 0x00); d.write(0x03, 0x10);   // PITCH $1000: 32 kHz
    d.write(0x04, 0x00);                        // SRCN
    d.write(0x05, 0x00); d.write(0x07, 0x7F);   // GAIN direct, full
    d.write(0x0C, 0x7F); d.write(0x1C, 0x7F);   // MVOL
    d.write(0x5D, 0x03);                        // DIR
    d.write(0x6C, 0x20);                        // FLG: unmuted, echo writes off
    d.write(0x4C, 0x01);                        // KON voice 0
    apu.runTo(snes::Apu::CLOCK_HZ / 10);           // 100 ms
    std::vector<float> buf(8192);
    const int n = apu.pullSamples(buf.data(), (int)buf.size());
    EXPECT_TRUE(n > 3000);
    float lo = 1, hi = -1;
    for (int i = n / 2; i < n; ++i) { lo = std::min(lo, buf[i]); hi = std::max(hi, buf[i]); }
    EXPECT_TRUE(hi - lo > 0.2f);
    EXPECT_TRUE(d.envelope(0) > 0);
}

namespace {
// Mode 1, BG1 4bpp: tile 1 solid colour 1 at the top left; colour 1 red, backdrop blue
void drawSetup(Rom& r) {
    r.init().lda(0x80).sta(0x2100)                         // forced blank
     .lda(0x01).sta(0x2105)                                // mode 1
     .lda(0x04).sta(0x2107)                                // BG1 map at word $0400
     .lda(0x00).sta(0x210B)                                // BG1 chars at word $0000
     .lda(0x80).sta(0x2115)                                // increment after $2119
     .lda(0x10).sta(0x2116).lda(0x00).sta(0x2117);         // tile 1 (16 words a 4bpp tile)
    for (int i = 0; i < 8; ++i) r.lda(0xFF).sta(0x2118).lda(0x00).sta(0x2119);   // planes 0/1: colour 1
    for (int i = 0; i < 8; ++i) r.lda(0x00).sta(0x2118).lda(0x00).sta(0x2119);   // planes 2/3
    r.lda(0x00).sta(0x2116).lda(0x04).sta(0x2117).lda(0x01).sta(0x2118).lda(0x00).sta(0x2119);   // map(0,0) = tile 1
    r.lda(0x00).sta(0x2121).lda(0x00).sta(0x2122).lda(0x7C).sta(0x2122)       // colour 0: blue
     .lda(0x1F).sta(0x2122).lda(0x00).sta(0x2122);                           // colour 1: red
    r.lda(0x01).sta(0x212C)                                // BG1 on the main screen
     .stz(0x210D).stz(0x210D).lda(0xFF).sta(0x210E).sta(0x210E);   // scroll 0, -1 (V offset 1023: line 1 shows row 0)
}
}  // namespace

// The background, and colour math with the fixed colour
TEST_CASE(snes_render_bg) {
    Rom r;
    drawSetup(r);
    r.lda(0x0F).sta(0x2100).self();
    MachineDescriptor* m = boot(r);
    ASSERT(m);
    run(m, FRAME * 3);
    auto* ppu = device<snes::Ppu>(m);
    std::vector<uint32_t> f(256 * 224);
    ppu->renderFrame(f.data());
    EXPECT_EQ(f[0], snes::Ppu::argb(0x001F));
    EXPECT_EQ(f[7 * 256 + 7], snes::Ppu::argb(0x001F));
    EXPECT_EQ(f[8], snes::Ppu::argb(0x7C00));
    EXPECT_EQ(f[100 * 256 + 100], snes::Ppu::argb(0x7C00));
    delete m;
}

// DMA to VRAM, and HDMA changing the backdrop colour part way down
TEST_CASE(snes_dma_hdma) {
    Rom r;
    r.init().lda(0x80).sta(0x2100).lda(0x80).sta(0x2115).stz(0x2116).stz(0x2117)
     // DMA channel 0: 8 bytes from $00:A000 to $2118/$2119 (mode 1)
     .lda(0x01).sta(0x4300).lda(0x18).sta(0x4301).stz(0x4302).lda(0xA0).sta(0x4303).stz(0x4304)
     .lda(0x08).sta(0x4305).stz(0x4306).lda(0x01).sta(0x420B)
     // HDMA channel 1: mode 3 to $2121/$2122, the table at $00:B000
     .lda(0x03).sta(0x4310).lda(0x21).sta(0x4311).stz(0x4312).lda(0xB0).sta(0x4313).stz(0x4314)
     .stz(0x212C).lda(0x02).sta(0x420C).lda(0x0F).sta(0x2100).self();
    const uint8_t data[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    for (int i = 0; i < 8; ++i) r.img[0x2000 + i] = data[i];
    const uint8_t table[] = {16, 0, 0, 0x1F, 0x00,          // 16 lines: colour 0 = red
                             16, 0, 0, 0xE0, 0x03,          // then green
                             0};
    for (size_t i = 0; i < sizeof table; ++i) r.img[0x3000 + i] = table[i];
    MachineDescriptor* m = boot(r);
    ASSERT(m);
    run(m, FRAME * 3);
    auto* ppu = device<snes::Ppu>(m);
    EXPECT_EQ(ppu->vram(0), 0x2211);
    EXPECT_EQ(ppu->vram(3), 0x8877);
    std::vector<uint32_t> f(256 * 224);
    ppu->renderFrame(f.data());
    EXPECT_EQ(f[0], snes::Ppu::argb(0x001F));
    EXPECT_EQ(f[15 * 256], snes::Ppu::argb(0x001F));
    EXPECT_EQ(f[16 * 256], snes::Ppu::argb(0x03E0));
    EXPECT_EQ(f[100 * 256], snes::Ppu::argb(0x03E0));
    delete m;
}

// Auto joypad read into $4218/$4219
TEST_CASE(snes_joypad) {
    Rom r;
    r.init().lda(0x81).sta(0x4200).b({0xCB, 0x80, 0xFD});
    r.at(0x9000).ldaA(0x4218).sta(0x0000).ldaA(0x4219).sta(0x0001).ldaA(0x4210).b({0x40});
    r.vector(0xFFEA, 0x9000);
    MachineDescriptor* m = boot(r);
    ASSERT(m);
    ASSERT(m->onGamepad);
    m->onGamepad(0, MachineDescriptor::PAD_A | MachineDescriptor::PAD_START | MachineDescriptor::PAD_LEFT);
    run(m, FRAME * 2);
    IBus* bus = m->buses[0].bus;
    EXPECT_EQ(bus->peek8(0x0000), 0x80);                 // A
    EXPECT_EQ(bus->peek8(0x0001), 0x12);                 // Start, Left
    delete m;
}

namespace {
/** Set CGRAM colour 0 (the backdrop). */
void backdrop(snes::Ppu& p, uint16_t c) { p.write(0x21, 0); p.write(0x22, (uint8_t)c); p.write(0x22, (uint8_t)(c >> 8)); }
/** Draw the rest of a frame (from line `from`) and finish it. */
void finishFrame(snes::Ppu& p, int from) { for (int l = from; l < 225; ++l) p.endLine(l); p.startVblank(); }
}  // namespace

// A register write part way along a line splits it: pixel x leaves at dot x + 22
TEST_CASE(snes_ppu_midline_write) {
    snes::Ppu p;
    p.write(0x00, 0x0F);
    backdrop(p, 0x001F);
    p.startFrame();
    p.endLine(0);
    p.sync(1, 22 + 100);                    // pixels 0-100 drawn red
    backdrop(p, 0x7C00);
    finishFrame(p, 1);
    const auto d = p.getDimensions();
    EXPECT_EQ(d.width, 256);
    EXPECT_EQ(d.height, 224);
    std::vector<uint32_t> f(256 * 224);
    p.renderFrame(f.data());
    EXPECT_EQ(f[100], snes::Ppu::argb(0x001F));
    EXPECT_EQ(f[101], snes::Ppu::argb(0x7C00));
    EXPECT_EQ(f[256], snes::Ppu::argb(0x7C00));
}

// Pseudo-hires gives 512-pixel lines; interlace draws one field's rows a frame
TEST_CASE(snes_ppu_hires_interlace) {
    snes::Ppu p;
    p.write(0x00, 0x0F);
    p.write(0x33, 0x08);                    // pseudo-hires
    backdrop(p, 0x001F);
    p.startFrame();
    finishFrame(p, 0);
    auto d = p.getDimensions();
    EXPECT_EQ(d.width, 512);
    EXPECT_EQ(d.height, 448);
    std::vector<uint32_t> f(512 * 448);
    p.renderFrame(f.data());
    EXPECT_EQ(f[0], snes::Ppu::argb(0x001F));
    EXPECT_EQ(f[512 + 511], snes::Ppu::argb(0x001F));

    p.write(0x33, 0x01);                    // interlace: two fields, red then blue
    p.startFrame();
    const bool first = p.oddField();
    finishFrame(p, 0);
    p.startFrame();
    backdrop(p, 0x7C00);
    finishFrame(p, 0);
    d = p.getDimensions();
    EXPECT_EQ(d.width, 512);
    EXPECT_EQ(d.height, 448);
    p.renderFrame(f.data());
    const uint32_t red = snes::Ppu::argb(0x001F), blue = snes::Ppu::argb(0x7C00);
    EXPECT_EQ(f[(first ? 1 : 0) * 512 + 7], red);
    EXPECT_EQ(f[(first ? 0 : 1) * 512 + 7], blue);
    EXPECT_EQ(f[(first ? 3 : 2) * 512 + 7], red);
}

// Overscan: 239 lines, and vertical blank at line 240
TEST_CASE(snes_ppu_overscan) {
    snes::Ppu p;
    p.write(0x00, 0x0F);
    p.write(0x33, 0x04);
    p.startFrame();
    EXPECT_EQ(p.vblankLine(), 240);
    for (int l = 0; l < 240; ++l) p.endLine(l);
    p.startVblank();
    EXPECT_EQ(p.getDimensions().height, 239);
}

// Battery RAM: loaded from the .srm, written back within a second of a change
TEST_CASE(snes_battery_save) {
    EXPECT_EQ(snes::savePathFor("/x/y/Game.v1.sfc"), std::string("/x/y/Game.v1.srm"));
    EXPECT_EQ(snes::savePathFor("dir.d/game"), std::string("dir.d/game.srm"));
    std::vector<uint8_t> img(0x20000, 0xEA);
    const char* title = "BATTERY TEST         ";
    for (int i = 0; i < 21; ++i) img[0xFFC0 + i] = (uint8_t)title[i];
    img[0xFFD5] = 0x21; img[0xFFD6] = 0x02; img[0xFFD7] = 0x07; img[0xFFD8] = 0x03;   // HiROM, ROM+RAM+battery, 8 KB
    img[0xFFDC] = 0xFF; img[0xFFDD] = 0xFF;
    img[0xFFFC] = 0x00; img[0xFFFD] = 0x80;
    img[0x8000] = 0x80; img[0x8001] = 0xFE;                 // BRA *
    const std::string path = (std::filesystem::temp_directory_path() / "mmsim_snes_battery_test.srm").string();
    std::filesystem::remove(path);
    std::string err;
    MachineDescriptor* m = snes::create(false, &err);
    ASSERT(snes::insertCartridge(m, img, "b.sfc", err, path));
    EXPECT_EQ(snes::cartridgeInfo(m), std::string("b.sfc (BATTERY TEST, HiROM, 128K ROM, 8K RAM, battery)"));
    m->buses[0].bus->write8(0x206000, 0x5A);
    m->buses[0].bus->write8(0x207FFF, 0xA5);
    run(m, FRAME * 62);                                     // a second: written out
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    ASSERT_EQ(file.size(), (size_t)0x2000);
    EXPECT_EQ(file[0], 0x5A);
    EXPECT_EQ(file[0x1FFF], 0xA5);
    m->buses[0].bus->write8(0x206001, 0x77);
    snes::removeCartridge(m);                               // saved on the way out
    delete m;
    m = snes::create(false, &err);
    ASSERT(snes::insertCartridge(m, img, "b.sfc", err, path));
    EXPECT_EQ(m->buses[0].bus->peek8(0x206000), 0x5A);
    EXPECT_EQ(m->buses[0].bus->peek8(0x206001), 0x77);
    EXPECT_EQ(m->buses[0].bus->peek8(0x207FFF), 0xA5);
    delete m;
    std::filesystem::remove(path);
}
