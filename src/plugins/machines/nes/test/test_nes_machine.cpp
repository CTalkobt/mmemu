#include "test_harness.h"
#include "plugins/machines/nes/main/machine_nes.h"
#include "plugins/machines/nes/main/nes_apu.h"
#include "plugins/machines/nes/main/nes_cart.h"
#include "plugins/machines/nes/main/nes_ppu.h"
#include "libcore/main/machine_desc.h"
#include "libdevices/main/io_registry.h"
#include "libmem/main/ibus.h"
#include <initializer_list>
#include <string>
#include <vector>

namespace {

/** An iNES image: `prg` and `chr` (empty: CHR RAM) on `mapper`. */
std::vector<uint8_t> ines(const std::vector<uint8_t>& prg, const std::vector<uint8_t>& chr, int mapper = 0, bool vertical = false) {
    std::vector<uint8_t> img = {'N', 'E', 'S', 0x1A, (uint8_t)(prg.size() / 0x4000), (uint8_t)(chr.size() / 0x2000),
                                (uint8_t)(((mapper & 0x0F) << 4) | (vertical ? 1 : 0)), (uint8_t)(mapper & 0xF0), 0, 0, 0, 0, 0, 0, 0, 0};
    img.insert(img.end(), prg.begin(), prg.end());
    img.insert(img.end(), chr.begin(), chr.end());
    return img;
}

/** A tiny 6502 assembler writing into the last 32 KB of a PRG image ($8000-$FFFF). */
struct Asm {
    std::vector<uint8_t>& prg;
    uint16_t pc;
    Asm(std::vector<uint8_t>& p, uint16_t at) : prg(p), pc(at) {}
    size_t off(uint16_t a) const { return prg.size() - 0x8000 + (a - 0x8000); }
    Asm& b(std::initializer_list<int> bytes) { for (int v : bytes) prg[off(pc++)] = (uint8_t)v; return *this; }
    Asm& abs(int op, uint16_t a) { return b({op, a & 0xFF, a >> 8}); }
    Asm& lda(uint8_t v) { return b({0xA9, v}); }
    Asm& ldx(uint8_t v) { return b({0xA2, v}); }
    Asm& ldaA(uint16_t a) { return abs(0xAD, a); }
    Asm& sta(uint16_t a) { return abs(0x8D, a); }
    Asm& stx(uint16_t a) { return abs(0x8E, a); }
    Asm& incZ(uint8_t z) { return b({0xE6, z}); }
    Asm& waitVblank() { return abs(0x2C, 0x2002).b({0x10, 0xFB}); }    // BIT $2002; BPL *-3
    Asm& ppuAddr(uint16_t a) { return lda(a >> 8).sta(0x2006).lda(a & 0xFF).sta(0x2006); }
    Asm& self() { return abs(0x4C, pc); }                                // JMP *
    Asm& init() { return b({0x78, 0xD8}).ldx(0xFF).b({0x9A}); }          // SEI CLD LDX #$FF TXS
};

void vectors(std::vector<uint8_t>& prg, uint16_t reset, uint16_t nmi = 0, uint16_t irq = 0) {
    const size_t e = prg.size();
    prg[e - 6] = nmi & 0xFF; prg[e - 5] = nmi >> 8;
    prg[e - 4] = reset & 0xFF; prg[e - 3] = reset >> 8;
    prg[e - 2] = irq & 0xFF; prg[e - 1] = irq >> 8;
}

uint64_t run(MachineDescriptor* m, uint64_t cycles) {
    uint64_t t = 0;
    while (t < cycles) t += (uint64_t)m->schedulerStep(*m);
    return t;
}

template <class T> T* device(MachineDescriptor* m) {
    std::vector<IOHandler*> hs;
    m->ioRegistry->enumerate(hs);
    for (auto* h : hs) if (auto* d = dynamic_cast<T*>(h)) return d;
    return nullptr;
}

MachineDescriptor* boot(const std::vector<uint8_t>& img) {
    std::string err;
    MachineDescriptor* m = nes::create(false, &err);
    if (!m) return nullptr;
    if (!nes::insertCartridge(m, img, "t.nes", err)) { std::printf("insert: %s\n", err.c_str()); delete m; return nullptr; }
    return m;
}

constexpr uint64_t FRAME = 29781;   // CPU cycles a frame (29780.5)

}  // namespace

// The header: iNES 1 and NES 2.0 fields; unsupported mappers refused
TEST_CASE(nes_header) {
    std::vector<uint8_t> prg(0x8000, 0), chr(0x2000, 0);
    nes::RomInfo info;
    std::string err;
    ASSERT(nes::parseHeader(ines(prg, chr, 4, true), info, err));
    EXPECT_EQ(info.mapper, 4);
    EXPECT_EQ(info.prgSize, 0x8000u);
    EXPECT_EQ(info.chrSize, 0x2000u);
    EXPECT_TRUE(info.mirroring == nes::Mirroring::Vertical);
    EXPECT_TRUE(!info.nes2);

    auto img = ines(prg, {}, 1);
    img[7] |= 0x08;                        // NES 2.0
    img[10] = 0x07;                        // 8 KB work RAM
    img[11] = 0x09;                        // 32 KB CHR RAM
    ASSERT(nes::parseHeader(img, info, err));
    EXPECT_TRUE(info.nes2);
    EXPECT_EQ(info.chrRamSize, 0x8000u);
    EXPECT_EQ(info.prgRamSize, 0x2000u);

    EXPECT_TRUE(!nes::Cartridge::load(ines(prg, chr, 5), err));
    EXPECT_TRUE(err.find("mapper 5") != std::string::npos);
    std::vector<uint8_t> junk = {'N', 'E', 'S'};
    EXPECT_TRUE(!nes::parseHeader(junk, info, err));
}

// RAM is mirrored every 2 KB; the 2A03 has no decimal mode
TEST_CASE(nes_ram_and_binary_adc) {
    std::vector<uint8_t> prg(0x8000, 0xEA);
    Asm a(prg, 0x8000);
    a.init().lda(0x42).sta(0x0005).ldaA(0x0805).sta(0x0006)
     .b({0xF8, 0x18}).lda(0x09).b({0x69, 0x01}).sta(0x0007)     // SED CLC LDA #9 ADC #1 -> $0A
     .self();
    vectors(prg, 0x8000);
    MachineDescriptor* m = boot(ines(prg, {}));
    ASSERT(m);
    EXPECT_EQ(nes::cartridgeInfo(m), std::string("t.nes (NROM, 32K PRG, 8K CHR RAM)"));
    run(m, 200);
    IBus* bus = m->buses[0].bus;
    EXPECT_EQ(bus->peek8(0x0006), 0x42);
    EXPECT_EQ(bus->peek8(0x1805), 0x42);
    EXPECT_EQ(bus->peek8(0x0007), 0x0A);
    delete m;
}

// Vertical blank: one NMI a frame with $2000 bit 7, none without
TEST_CASE(nes_vblank_nmi) {
    std::vector<uint8_t> prg(0x8000, 0xEA);
    Asm a(prg, 0x8000);
    a.init().lda(0x80).sta(0x2000).self();
    Asm h(prg, 0x8100);
    h.incZ(0x10).b({0x40});                                      // INC $10; RTI
    vectors(prg, 0x8000, 0x8100);
    MachineDescriptor* m = boot(ines(prg, {}));
    ASSERT(m);
    run(m, FRAME * 5);
    EXPECT_EQ(m->buses[0].bus->peek8(0x10), 5);
    auto* ppu = device<nes::Ppu>(m);
    ASSERT(ppu);
    EXPECT_EQ(ppu->frameCount(), 5u);
    delete m;
}

// $2006/$2007: buffered reads, horizontal mirroring, palette mirrors
TEST_CASE(nes_ppu_vram) {
    std::vector<uint8_t> prg(0x8000, 0xEA);
    Asm a(prg, 0x8000);
    a.init().ldaA(0x2002)
     .ppuAddr(0x2000).lda(0x55).sta(0x2007).lda(0x66).sta(0x2007)
     .ppuAddr(0x2400).ldaA(0x2007).ldaA(0x2007).sta(0x0000).ldaA(0x2007).sta(0x0001)   // mirror of $2000
     .ppuAddr(0x3F10).lda(0x2A).sta(0x2007)
     .ppuAddr(0x3F00).ldaA(0x2007).sta(0x0002)                     // palette reads are immediate
     .self();
    vectors(prg, 0x8000);
    MachineDescriptor* m = boot(ines(prg, {}));
    ASSERT(m);
    run(m, 1000);
    IBus* bus = m->buses[0].bus;
    EXPECT_EQ(bus->peek8(0x0000), 0x55);
    EXPECT_EQ(bus->peek8(0x0001), 0x66);
    EXPECT_EQ(bus->peek8(0x0002), 0x2A);
    auto* ppu = device<nes::Ppu>(m);
    EXPECT_EQ(ppu->paletteRam(0x00), 0x2A);
    EXPECT_EQ(ppu->paletteRam(0x10), 0x2A);
    EXPECT_EQ(ppu->peekVram(0x2800), 0x00);                        // the other nametable
    delete m;
}

namespace {
// Tile 1 = colour 1 throughout; tile 1 at the top-left; black backdrop, white colour 1
void drawSetup(Asm& a) {
    a.init().ldaA(0x2002).ppuAddr(0x0010);
    for (int i = 0; i < 8; ++i) a.lda(0xFF).sta(0x2007);
    for (int i = 0; i < 8; ++i) a.lda(0x00).sta(0x2007);
    a.ppuAddr(0x2000).lda(0x01).sta(0x2007);
    a.ppuAddr(0x3F00).lda(0x0F).sta(0x2007).lda(0x30).sta(0x2007);
    a.lda(0x00).sta(0x2000).sta(0x2005).sta(0x2005);
}
}  // namespace

// The background: a tile drawn where the nametable puts it
TEST_CASE(nes_render_background) {
    std::vector<uint8_t> prg(0x8000, 0xEA);
    Asm a(prg, 0x8000);
    drawSetup(a);
    a.waitVblank().lda(0x0A).sta(0x2001).self();                  // background on, left column shown
    vectors(prg, 0x8000);
    MachineDescriptor* m = boot(ines(prg, {}));
    ASSERT(m);
    run(m, FRAME * 4);
    auto* ppu = device<nes::Ppu>(m);
    std::vector<uint32_t> frame(256 * 240);
    ppu->renderFrame(frame.data());
    EXPECT_EQ(frame[0], nes::Ppu::color(0x30));
    EXPECT_EQ(frame[7 * 256 + 7], nes::Ppu::color(0x30));
    EXPECT_EQ(frame[8], nes::Ppu::color(0x0F));
    EXPECT_EQ(frame[8 * 256], nes::Ppu::color(0x0F));
    delete m;
}

// Sprite 0 over an opaque background pixel sets $2002 bit 6
TEST_CASE(nes_sprite0_hit) {
    std::vector<uint8_t> prg(0x8000, 0xEA);
    Asm a(prg, 0x8000);
    drawSetup(a);
    a.lda(0x00).sta(0x2003).lda(0x00).sta(0x2004).lda(0x01).sta(0x2004).lda(0x00).sta(0x2004).lda(0x04).sta(0x2004);
    a.waitVblank().lda(0x1E).sta(0x2001);
    a.abs(0x2C, 0x2002).b({0x50, 0xFB});                           // BIT $2002; BVC *-3
    a.lda(0x01).sta(0x0010).self();
    vectors(prg, 0x8000);
    MachineDescriptor* m = boot(ines(prg, {}));
    ASSERT(m);
    run(m, FRAME * 3);
    EXPECT_EQ(m->buses[0].bus->peek8(0x10), 1);
    delete m;
}

// OAM DMA copies a page to OAM and stalls the CPU 513 or 514 cycles
TEST_CASE(nes_oam_dma) {
    std::vector<uint8_t> prg(0x8000, 0xEA);
    Asm a(prg, 0x8000);
    a.init().ldx(0x00);
    a.b({0x8A, 0x9D, 0x00, 0x02, 0xE8, 0xD0, 0xF9});               // loop: TXA; STA $0200,X; INX; BNE loop
    a.lda(0x00).sta(0x2003).lda(0x02);
    const uint16_t dma = a.pc;
    a.sta(0x4014).self();
    vectors(prg, 0x8000);
    MachineDescriptor* m = boot(ines(prg, {}));
    ASSERT(m);
    int cycles = 0;
    for (int i = 0; i < 100000 && m->cpus[0].cpu->pc() != dma; ++i) m->schedulerStep(*m);
    ASSERT_EQ(m->cpus[0].cpu->pc(), dma);
    // STA $4014, then the DMA halts the CPU on the next read (JMP's opcode fetch)
    cycles = m->schedulerStep(*m);
    EXPECT_EQ(cycles, 4);
    cycles = m->schedulerStep(*m);
    EXPECT_TRUE(cycles == 3 + 513 || cycles == 3 + 514);
    auto* ppu = device<nes::Ppu>(m);
    EXPECT_EQ(ppu->oam(0x00), 0x00);
    EXPECT_EQ(ppu->oam(0x41), 0x41);
    EXPECT_EQ(ppu->oam(0x06), 0x06 & 0xE3);                         // attribute bits 2-4 do not exist
    EXPECT_EQ(ppu->oam(0xFF), 0xFF);
    delete m;
}

// The controller: strobe, then A B Select Start Up Down Left Right, then 1s
TEST_CASE(nes_controller) {
    std::vector<uint8_t> prg(0x8000, 0xEA);
    Asm a(prg, 0x8000);
    a.init().lda(0x01).sta(0x4016).lda(0x00).sta(0x4016);
    for (int i = 0; i < 9; ++i) a.ldaA(0x4016).b({0x29, 0x01}).sta((uint16_t)(0x10 + i));   // AND #1
    a.self();
    vectors(prg, 0x8000);
    MachineDescriptor* m = boot(ines(prg, {}));
    ASSERT(m);
    ASSERT(m->onGamepad);
    m->onGamepad(0, MachineDescriptor::PAD_A | MachineDescriptor::PAD_START | MachineDescriptor::PAD_RIGHT);
    run(m, 500);
    IBus* bus = m->buses[0].bus;
    const int want[9] = {1, 0, 0, 1, 0, 0, 0, 1, 1};
    for (int i = 0; i < 9; ++i) EXPECT_EQ(bus->peek8((uint16_t)(0x10 + i)), want[i]);
    // Keyboard: Z is B
    EXPECT_TRUE(m->onKey("z", true));
    m->onGamepad(0, 0);
    delete m;
}

// APU: length counters, $4015 status, the frame counter's IRQ
TEST_CASE(nes_apu_length_and_frame_irq) {
    std::vector<uint8_t> prg(0x8000, 0xEA);
    Asm a(prg, 0x8000);
    a.init().lda(0x0F).sta(0x4015).lda(0x00).sta(0x4000).lda(0x08).sta(0x4003)   // pulse 1, length index 1 (254)
     .ldaA(0x4015).sta(0x0010).self();
    vectors(prg, 0x8000);
    MachineDescriptor* m = boot(ines(prg, {}));
    ASSERT(m);
    auto* apu = device<nes::Apu>(m);
    ASSERT(apu);
    run(m, 100);
    EXPECT_EQ(m->buses[0].bus->peek8(0x10) & 0x01, 0x01);
    EXPECT_EQ(apu->lengthCounter(0), 254);
    run(m, 31000);
    EXPECT_EQ(apu->lengthCounter(0), 252);                          // two half frames
    EXPECT_TRUE(apu->irq());                                        // 4-step mode, IRQ not inhibited
    EXPECT_EQ(m->buses[0].bus->peek8(0x4015) & 0x40, 0x40);
    delete m;
}

// The pulse channel sounds: samples come out once it plays
TEST_CASE(nes_apu_audio) {
    std::vector<uint8_t> prg(0x8000, 0xEA);
    Asm a(prg, 0x8000);
    a.init().lda(0x01).sta(0x4015).lda(0xBF).sta(0x4000).lda(0x00).sta(0x4001)
     .lda(0xFD).sta(0x4002).lda(0x00).sta(0x4003).self();          // 50% duty, constant volume 15, ~440 Hz
    vectors(prg, 0x8000);
    MachineDescriptor* m = boot(ines(prg, {}));
    ASSERT(m);
    auto* apu = device<nes::Apu>(m);
    run(m, FRAME * 2);
    std::vector<float> buf(4096);
    const int n = apu->pullSamples(buf.data(), (int)buf.size());
    EXPECT_TRUE(n > 1200);                                         // ~1470 samples in two frames
    float lo = 1, hi = -1;
    for (int i = n / 2; i < n; ++i) { lo = std::min(lo, buf[i]); hi = std::max(hi, buf[i]); }
    EXPECT_TRUE(hi - lo > 0.1f);
    delete m;
}

// MMC1: five serial writes to $E000 select the 16 KB bank at $8000
TEST_CASE(nes_mmc1) {
    std::vector<uint8_t> prg(0x20000, 0xEA);
    for (int b = 0; b < 8; ++b) prg[(size_t)b * 0x4000] = (uint8_t)b;
    Asm a(prg, 0xC000);                                             // the last bank, fixed at $C000
    a.init().lda(0x80).sta(0x8000)
     .lda(0x05).sta(0xE000).b({0x4A}).sta(0xE000).b({0x4A}).sta(0xE000).b({0x4A}).sta(0xE000).b({0x4A}).sta(0xE000)
     .ldaA(0x8000).sta(0x0010).self();
    vectors(prg, 0xC000);
    MachineDescriptor* m = boot(ines(prg, {}, 1));
    ASSERT(m);
    run(m, 300);
    EXPECT_EQ(m->buses[0].bus->peek8(0x10), 5);
    delete m;
}

// UxROM, CNROM and AxROM bank selects
TEST_CASE(nes_simple_mappers) {
    {   // UxROM: 8 banks, the last fixed at $C000
        std::vector<uint8_t> prg(0x20000, 0xEA);
        for (int b = 0; b < 8; ++b) prg[(size_t)b * 0x4000] = (uint8_t)(0x40 + b);
        Asm a(prg, 0xC000);
        a.init().lda(0x03).sta(0x8000).ldaA(0x8000).sta(0x0010).self();
        vectors(prg, 0xC000);
        MachineDescriptor* m = boot(ines(prg, {}, 2));
        ASSERT(m);
        run(m, 200);
        EXPECT_EQ(m->buses[0].bus->peek8(0x10), 0x43);
        delete m;
    }
    {   // CNROM: CHR bank 2 of 4
        std::vector<uint8_t> prg(0x8000, 0xEA), chr(0x8000, 0);
        for (int b = 0; b < 4; ++b) chr[(size_t)b * 0x2000] = (uint8_t)(0x70 + b);
        Asm a(prg, 0x8000);
        a.init().lda(0x02).sta(0x8000).self();
        vectors(prg, 0x8000);
        MachineDescriptor* m = boot(ines(prg, chr, 3));
        ASSERT(m);
        run(m, 200);
        EXPECT_EQ(device<nes::Ppu>(m)->peekVram(0x0000), 0x72);
        delete m;
    }
    {   // AxROM: 32 KB bank 1, single-screen B
        std::vector<uint8_t> prg(0x10000, 0xEA);
        prg[0x8000] = 0x99;
        // bank 0 (image $0000) and bank 1 (image $8000) both carry the program at $8100
        auto put = [&](size_t base) {
            const uint8_t code[] = {0x78, 0xD8, 0xA9, 0x11, 0x8D, 0x00, 0x80, 0xAD, 0x00, 0x80, 0x85, 0x10, 0x4C, 0x0C, 0x81};
            std::copy(code, code + sizeof code, prg.begin() + (long)(base + 0x100));
            prg[base + 0x7FFC] = 0x00; prg[base + 0x7FFD] = 0x81;
        };
        put(0x0000);
        put(0x8000);
        MachineDescriptor* m = boot(ines(prg, {}, 7));
        ASSERT(m);
        run(m, 200);
        EXPECT_EQ(m->buses[0].bus->peek8(0x10), 0x99);
        delete m;
    }
}

// MMC3: the scanline counter, clocked by PPU A12 (sprites at $1000), raises
// /IRQ on the line the latch counts down to
TEST_CASE(nes_mmc3_irq) {
    std::vector<uint8_t> prg(0x8000, 0xEA);
    Asm a(prg, 0xE000);
    a.init().lda(0x40).sta(0x4017)                                  // no APU frame IRQ
     .lda(0x00).sta(0x2001).lda(0x08).sta(0x2000)                   // sprites at $1000, background at $0000
     .lda(20).sta(0xC000).sta(0xC001).sta(0xE001).b({0x58})         // latch 20, reload, enable; CLI
     .waitVblank().lda(0x18).sta(0x2001).self();
    Asm h(prg, 0xE100);
    h.sta(0xE000).incZ(0x10).b({0x40});                             // acknowledge; INC $10; RTI
    vectors(prg, 0xE000, 0, 0xE100);
    MachineDescriptor* m = boot(ines(prg, {}, 4));
    ASSERT(m);
    auto* ppu = device<nes::Ppu>(m);
    int line = -100;
    for (int i = 0; i < 200000; ++i) {
        m->schedulerStep(*m);
        if (m->cpus[0].cpu->pc() == 0xE100) { line = ppu->scanline(); break; }
    }
    if (line != 19 && line != 20) std::printf("    MMC3 IRQ on line %d\n", line);
    EXPECT_TRUE(line == 19 || line == 20);
    delete m;
}
