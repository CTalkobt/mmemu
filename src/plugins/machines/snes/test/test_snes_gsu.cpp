// The Super FX with a small hand-built cartridge: the 65816 starts a GSU
// program (IWT, WITH/ADD, FROM/STW, STOP) and reads its results. krom's
// GSU instruction tests and drawing demos are in test_snes_roms.cpp.

#include "test_harness.h"
#include "plugins/machines/snes/main/machine_snes.h"
#include "libcore/main/machine_desc.h"
#include "libmem/main/ibus.h"
#include <cstdio>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> gsuCart() {
    std::vector<uint8_t> img(0x10000, 0);
    const char* title = "MMSIM SUPERFX        ";
    for (int i = 0; i < 21; ++i) img[0x7FC0 + i] = (uint8_t)title[i];
    img[0x7FD5] = 0x20; img[0x7FD6] = 0x13; img[0x7FD7] = 0x06;
    img[0x7FBD] = 0x05;                                     // 32 KB of GSU RAM
    img[0x7FDC] = 0xFF; img[0x7FDD] = 0xFF;
    img[0x7FFC] = 0x00; img[0x7FFD] = 0x80;
    std::vector<uint8_t> c = {0x78, 0x18, 0xFB, 0xE2, 0x30};    // SEI CLC XCE SEP #$30
    // While the GSU has the ROM the 65816 can't run from it: the start and the wait are in work RAM at $0100
    const uint8_t wram[] = {
        0xA9, 0x00, 0x8D, 0x1E, 0x30, 0xA9, 0x81, 0x8D, 0x1F, 0x30,   // R15 = $8100: go
        0xAD, 0x30, 0x30, 0x29, 0x20, 0xD0, 0xF9,           // wait while SFR.G
        0x5C, 0x00, 0x00, 0x00,                             // JML back (patched)
    };
    const size_t jml = 18;
    std::vector<uint8_t> w(wram, wram + sizeof wram);
    const uint8_t setup[] = {
        0xA9, 0x18, 0x8D, 0x3A, 0x30,                       // SCMR: RON, RAN
        0x9C, 0x34, 0x30,                                   // PBR = 0
    };
    c.insert(c.end(), setup, setup + sizeof setup);
    const size_t copyAt = c.size();
    for (size_t i = 0; i < w.size(); ++i) { c.push_back(0xA9); c.push_back(0); c.push_back(0x8D); c.push_back((uint8_t)i); c.push_back(0x01); }
    c.push_back(0x4C); c.push_back(0x00); c.push_back(0x01);           // JMP $0100
    const uint16_t cont = (uint16_t)(0x8000 + c.size());
    w[jml] = (uint8_t)cont; w[jml + 1] = (uint8_t)(cont >> 8);
    for (size_t i = 0; i < w.size(); ++i) c[copyAt + i * 5 + 1] = w[i];
    const uint8_t after[] = {
        0xAD, 0x02, 0x30, 0x8D, 0x00, 0x00,                 // R1 to $0000/$0001
        0xAD, 0x03, 0x30, 0x8D, 0x01, 0x00,
        0xAF, 0x10, 0x00, 0x70, 0x8D, 0x02, 0x00,           // GSU RAM $70:0010 to $0002
        0xAD, 0x31, 0x30, 0x8D, 0x03, 0x00,                 // SFR high (the IRQ bit) to $0003
        0xAD, 0x31, 0x30, 0x8D, 0x04, 0x00,                 // again: acknowledged
        0x80, 0xFE,
    };
    c.insert(c.end(), after, after + sizeof after);
    std::copy(c.begin(), c.end(), img.begin());
    const uint8_t gsu[] = {
        0xF1, 0x34, 0x12,                                   // IWT R1, #$1234
        0xF2, 0x01, 0x01,                                   // IWT R2, #$0101
        0x21, 0x52,                                         // WITH R1 : ADD R2
        0xF3, 0x10, 0x00,                                   // IWT R3, #$0010
        0xB1, 0x33,                                         // FROM R1 : STW (R3)
        0x00, 0x01,                                         // STOP, NOP
    };
    std::copy(gsu, gsu + sizeof gsu, img.begin() + 0x100);
    return img;
}

}  // namespace

TEST_CASE(snes_superfx_program) {
    std::string err;
    MachineDescriptor* m = snes::create(false, &err);
    ASSERT(snes::insertCartridge(m, gsuCart(), "fx.sfc", err));
    EXPECT_EQ(snes::cartridgeInfo(m), std::string("fx.sfc (MMSIM SUPERFX, LoROM, 64K ROM, Super FX, 32K RAM)"));
    uint64_t t = 0;
    while (t < 300000) t += (uint64_t)m->schedulerStep(*m);
    IBus* bus = m->buses[0].bus;
    EXPECT_EQ(bus->peek8(0x000000), 0x35);
    EXPECT_EQ(bus->peek8(0x000001), 0x13);
    EXPECT_EQ(bus->peek8(0x000002), 0x35);
    EXPECT_TRUE(bus->peek8(0x000003) & 0x80);              // STOP raised the IRQ
    EXPECT_TRUE(!(bus->peek8(0x000004) & 0x80));           // reading $3031 cleared it
    EXPECT_EQ(bus->peek8(0x700011), 0x13);                  // the SNES sees the GSU's RAM
    delete m;
}
