// The OBC1: its sprite-table ports over the cartridge RAM, on a hand-built
// LoROM cartridge (chipset $25, 8 KB RAM, as Metal Combat).

#include "test_harness.h"
#include "plugins/machines/snes/main/machine_snes.h"
#include "libcore/main/machine_desc.h"
#include "libmem/main/ibus.h"
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> obc1Cart() {
    std::vector<uint8_t> img(0x80000, 0);
    const char* title = "MMSIM OBC1           ";
    for (int i = 0; i < 21; ++i) img[0x7FC0 + i] = (uint8_t)title[i];
    img[0x7FD5] = 0x20; img[0x7FD6] = 0x25; img[0x7FD7] = 0x09; img[0x7FD8] = 0x03;
    img[0x7FDC] = 0xFF; img[0x7FDD] = 0xFF;
    img[0x7FFC] = 0x00; img[0x7FFD] = 0x80;
    img[0x0000] = 0x80; img[0x0001] = 0xFE;   // BRA *
    return img;
}

}  // namespace

TEST_CASE(snes_obc1_ports) {
    std::string err;
    MachineDescriptor* m = snes::create(false, &err);
    ASSERT(snes::insertCartridge(m, obc1Cart(), "obc1.sfc", err));
    EXPECT_EQ(snes::cartridgeInfo(m), std::string("obc1.sfc (MMSIM OBC1, LoROM, 512K ROM, 8K RAM, battery, OBC1)"));
    IBus* bus = m->buses[0].bus;
    // The table at $1C00 ($7FF5 bit 0 clear), sprite 5: its bytes at $1C14-$1C17
    bus->write8(0x007FF5, 0x00);
    bus->write8(0x007FF6, 0x05);
    bus->write8(0x007FF0, 0x11); bus->write8(0x007FF1, 0x22); bus->write8(0x007FF2, 0x33); bus->write8(0x007FF3, 0x44);
    EXPECT_EQ(bus->peek8(0x007C14), 0x11);
    EXPECT_EQ(bus->peek8(0x007C17), 0x44);
    EXPECT_EQ(bus->peek8(0x807FF2), 0x33);                   // read back through the port, in $80-$BF too
    // Its two high bits: sprite 5 is bits 2-3 of $1E01
    bus->write8(0x007E01, 0xFF);
    bus->write8(0x007FF4, 0x00);
    EXPECT_EQ(bus->peek8(0x007E01), 0xF3);
    EXPECT_EQ(bus->peek8(0x007FF4), 0xF3);
    bus->write8(0x007FF4, 0x02);
    EXPECT_EQ(bus->peek8(0x007E01), 0xFB);
    // The table at $1800: sprite 5's first byte at $1814
    bus->write8(0x007FF5, 0x01);
    bus->write8(0x007FF0, 0x99);
    EXPECT_EQ(bus->peek8(0x007814), 0x99);
    EXPECT_EQ(bus->peek8(0x007C14), 0x11);
    // The RAM is the cartridge's: $70:0000 sees the same bytes
    EXPECT_EQ(bus->peek8(0x701814), 0x99);
    delete m;
}
