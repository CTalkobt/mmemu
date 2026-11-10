// The Cx4 with a small hand-built cartridge: the S-CPU points the Cx4 at a
// program in ROM (loaded through its program cache) and reads back its
// registers, data RAM and status.

#include "test_harness.h"
#include "plugins/machines/snes/main/machine_snes.h"
#include "plugins/machines/snes/main/snes_cx4.h"
#include "libcore/main/machine_desc.h"
#include "libmem/main/ibus.h"
#include <string>
#include <vector>

TEST_CASE(snes_cx4_program) {
    EXPECT_EQ(snes::Cx4::DATA_ROM[1], 0x800000u);            // the data ROM: 1/n...
    EXPECT_EQ(snes::Cx4::DATA_ROM[1023], 0x03243Au);
    std::vector<uint8_t> img(0x10000, 0);
    const char* title = "MMSIM CX4            ";
    for (int i = 0; i < 21; ++i) img[0x7FC0 + i] = (uint8_t)title[i];
    img[0x7FBF] = 0x10;                                       // cartridge type: Cx4
    img[0x7FD5] = 0x20; img[0x7FD6] = 0xF3; img[0x7FD7] = 0x06;
    img[0x7FDC] = 0xFF; img[0x7FDD] = 0xFF;
    img[0x7FFC] = 0x00; img[0x7FFD] = 0x80;
    const uint8_t cpu[] = {
        0x78, 0x18, 0xFB, 0xE2, 0x30,
        0x9C, 0x49, 0x7F, 0xA9, 0xA0, 0x8D, 0x4A, 0x7F, 0x9C, 0x4B, 0x7F,   // cache base $00:A000
        0x9C, 0x4D, 0x7F, 0x9C, 0x4E, 0x7F,                                 // program bank 0
        0x9C, 0x4F, 0x7F,                                                   // PC 0: run
        0xAD, 0x5E, 0x7F, 0x29, 0x40, 0xD0, 0xF9,                           // wait while running
        0xAD, 0x80, 0x7F, 0x8D, 0x00, 0x00,                                 // R0
        0xAD, 0x85, 0x7F, 0x8D, 0x01, 0x00,                                 // R1's top byte
        0xAD, 0x10, 0x60, 0x8D, 0x02, 0x00,                                 // data RAM $010
        0xAD, 0x86, 0x7F, 0x8D, 0x03, 0x00,                                 // R2
        0xAD, 0x5E, 0x7F, 0x8D, 0x04, 0x00,                                 // status
        0x80, 0xFE,
    };
    std::copy(cpu, cpu + sizeof cpu, img.begin());
    const uint16_t cx4[] = {
        0x6405,     // LD A, #5
        0x8403,     // ADD A, #3
        0xF000,     // SWAP A, R0
        0x7401,     // RDROM #1
        0x6008,     // LD A, ROM buffer
        0xF001,     // SWAP A, R1
        0x6442,     // LD A, #$42
        0xE00C,     // ST A -> RAM buffer
        0x6410,     // LD A, #$10
        0xE800,     // WRRAM byte 0 -> data RAM[A]
        0x6407,     // LD A, #7
        0x9C03,     // SMUL #3
        0x6002,     // LD A, product (low)
        0xF002,     // SWAP A, R2
        0xFC00,     // STOP
    };
    for (size_t i = 0; i < sizeof cx4 / sizeof cx4[0]; ++i) { img[0x2000 + i * 2] = (uint8_t)cx4[i]; img[0x2001 + i * 2] = (uint8_t)(cx4[i] >> 8); }
    std::string err;
    MachineDescriptor* m = snes::create(false, &err);
    ASSERT(snes::insertCartridge(m, img, "cx4.sfc", err));
    EXPECT_EQ(snes::cartridgeInfo(m), std::string("cx4.sfc (MMSIM CX4, LoROM, 64K ROM, Cx4)"));
    uint64_t t = 0;
    while (t < 400000) t += (uint64_t)m->schedulerStep(*m);
    IBus* bus = m->buses[0].bus;
    EXPECT_EQ(bus->peek8(0x000000), 0x08);
    EXPECT_EQ(bus->peek8(0x000001), 0x80);
    EXPECT_EQ(bus->peek8(0x000002), 0x42);
    EXPECT_EQ(bus->peek8(0x000003), 0x15);                   // 7 * 3
    EXPECT_EQ(bus->peek8(0x000004), 0x02);                   // stopped, IRQ flag
    delete m;
}
