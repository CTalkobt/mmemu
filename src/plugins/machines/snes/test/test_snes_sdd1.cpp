// The S-DD1: its ROM banking and mirroring, and a DMA that receives
// decompressed data. There is no S-DD1 compressor to make known data, so
// the DMA's bytes are checked against the decompressor (Andreas Naive's
// code) run directly on the same ROM bytes.

#include "test_harness.h"
#include "plugins/machines/snes/main/machine_snes.h"
#include "plugins/machines/snes/main/snes_sdd1_decomp.h"
#include "libcore/main/machine_desc.h"
#include "libmem/main/ibus.h"
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> sdd1Cart() {
    std::vector<uint8_t> img(0x200000, 0);                   // 2 MB: two 1 MB blocks
    const char* title = "MMSIM SDD1           ";
    for (int i = 0; i < 21; ++i) img[0x7FC0 + i] = (uint8_t)title[i];
    img[0x7FD5] = 0x22; img[0x7FD6] = 0x43; img[0x7FD7] = 0x0B;
    img[0x7FDC] = 0xFF; img[0x7FDD] = 0xFF;
    img[0x7FFC] = 0x00; img[0x7FFD] = 0x80;
    const uint8_t cpu[] = {
        0x78, 0x18, 0xFB, 0xE2, 0x30,
        0x9C, 0x81, 0x21, 0x9C, 0x82, 0x21, 0x9C, 0x83, 0x21,               // WRAM port at $7E:0000
        0xA9, 0x08, 0x8D, 0x00, 0x43, 0xA9, 0x80, 0x8D, 0x01, 0x43,         // DMA 0: fixed source -> $2180
        0x9C, 0x02, 0x43, 0xA9, 0x10, 0x8D, 0x03, 0x43, 0xA9, 0xC0, 0x8D, 0x04, 0x43,   // from $C0:1000
        0xA9, 0x20, 0x8D, 0x05, 0x43, 0x9C, 0x06, 0x43,                     // 32 bytes
        0xA9, 0x01, 0x8D, 0x00, 0x48, 0x8D, 0x01, 0x48,                     // S-DD1: channel 0
        0x8D, 0x0B, 0x42,                                                   // go
        0x80, 0xFE,
    };
    std::copy(cpu, cpu + sizeof cpu, img.begin());
    uint32_t x = 12345;                                      // the "compressed" data: anything decodes
    for (int i = 0; i < 0x400; ++i) { x = x * 1103515245u + 12345u; img[0x1000 + i] = (uint8_t)(x >> 16); }
    img[0x100000] = 0xB1;                                    // markers for the banking
    img[0x108000] = 0xB2;
    return img;
}

}  // namespace

TEST_CASE(snes_sdd1_banking) {
    std::string err;
    MachineDescriptor* m = snes::create(false, &err);
    ASSERT(snes::insertCartridge(m, sdd1Cart(), "sdd1.sfc", err));
    EXPECT_EQ(snes::cartridgeInfo(m), std::string("sdd1.sfc (MMSIM SDD1, LoROM, 2048K ROM, S-DD1)"));
    IBus* bus = m->buses[0].bus;
    EXPECT_EQ(bus->peek8(0xC00000), 0x78);                   // block 0
    bus->write8(0x004804, 0x01);
    EXPECT_EQ(bus->peek8(0xC00000), 0xB1);                   // block 1
    EXPECT_EQ(bus->peek8(0x004804), 0x01);
    EXPECT_EQ(bus->peek8(0x208000), 0xB1);                   // $20:8000 is ROM $100000
    EXPECT_EQ(bus->peek8(0x218000), 0xB2);                   // $21:8000 is ROM $108000
    bus->write8(0x004805, 0x80);                             // $20-$3F mirror $00-$1F
    EXPECT_EQ(bus->peek8(0x208000), 0x78);                   // = $00:8000
    EXPECT_EQ(bus->peek8(0x218000), 0x00);                   // = $01:8000 (ROM $8000)
    delete m;
}

TEST_CASE(snes_sdd1_dma_decompression) {
    const std::vector<uint8_t> img = sdd1Cart();
    // The expected bytes: the decompressor reading the same ROM ($C0:1000 = offset $1000)
    snes::Sdd1Reader reader = [&img](uint32_t addr) { return img[(addr & 0xFFFFF) % img.size()]; };
    snes::Sdd1Decomp d;
    d.Init(&reader, 0xC01000);
    std::vector<uint8_t> want(32);
    for (auto& b : want) b = d.GetDecompressedByte();
    std::string err;
    MachineDescriptor* m = snes::create(false, &err);
    ASSERT(snes::insertCartridge(m, img, "sdd1.sfc", err));
    uint64_t t = 0;
    while (t < 200000) t += (uint64_t)m->schedulerStep(*m);
    IBus* bus = m->buses[0].bus;
    int same = 0;
    for (int i = 0; i < 32; ++i) same += bus->peek8(0x7E0000 + (uint32_t)i) == want[i];
    EXPECT_EQ(same, 32);
    // And it isn't simply the ROM: the decompressed bytes differ from the source
    int raw = 0;
    for (int i = 0; i < 32; ++i) raw += bus->peek8(0x7E0000 + (uint32_t)i) == img[0x1000 + i];
    EXPECT_TRUE(raw < 32);
    EXPECT_EQ(bus->peek8(0x004801), 0x00);                   // the channel's bit clears when its length runs out
    delete m;
}
