// The ST018: its ARM core and the link to the SNES. No Seta firmware is
// needed: a hand-assembled ARM program answers the SNES, and a random
// firmware run is checked against Mesen's ArmV3Cpu. The expected state
// came from Mesen's ArmV3Cpu.cpp compiled on its own with the same bus;
// 30 million random instructions in all modes matched it register for
// register and cycle for cycle.

#include "test_harness.h"
#include "plugins/machines/snes/main/machine_snes.h"
#include "plugins/machines/snes/main/snes_st018.h"
#include "libcore/main/machine_desc.h"
#include "libmem/main/ibus.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sys/stat.h>
#include <string>
#include <vector>

namespace {

void put32(std::vector<uint8_t>& fw, uint32_t at, uint32_t w) {
    fw[at] = (uint8_t)w; fw[at + 1] = (uint8_t)(w >> 8); fw[at + 2] = (uint8_t)(w >> 16); fw[at + 3] = (uint8_t)(w >> 24);
}

// Waits for a byte from the SNES, answers it plus one
std::vector<uint8_t> echoFirmware() {
    std::vector<uint8_t> fw(snes::St018::PRG_SIZE + snes::St018::DATA_SIZE, 0);
    const uint32_t prog[] = {
        0xE3A01101,     // 00: MOV  r1, #$40000000
        0xE5D12020,     // 04: LDRB r2, [r1, #$20]   status
        0xE3120008,     // 08: TST  r2, #8           a byte from the SNES?
        0x0AFFFFFC,     // 0C: BEQ  04
        0xE5D13010,     // 10: LDRB r3, [r1, #$10]
        0xE2833001,     // 14: ADD  r3, r3, #1
        0xE5C13000,     // 18: STRB r3, [r1]         to the SNES
        0xEAFFFFF8,     // 1C: B    04
    };
    for (uint32_t i = 0; i < sizeof prog / 4; ++i) put32(fw, i * 4, prog[i]);
    return fw;
}

// The random firmware the comparison with Mesen used (seed 1)
std::vector<uint8_t> randomFirmware() {
    const int seed = 1;
    uint64_t x = 0x9E3779B97F4A7C15ull * (uint64_t)seed;
    auto rnd = [&x]() { x ^= x << 13; x ^= x >> 7; x ^= x << 17; return (uint32_t)(x >> 16) ^ (uint32_t)x; };
    std::vector<uint8_t> fw(0x28000);
    for (uint32_t i = 0; i < 0x20000; i += 4) {
        uint32_t w = rnd();
        switch (rnd() % 8) {
        case 0: case 1: w = (w & 0x01FFFFFF) | (0xEu << 28); break;                                 // data processing
        case 2: w = (w & 0x03FFFFFF) | 0x04000000 | (0xEu << 28); w = (w & ~0x000F0000u) | 0x000E0000u; break;  // LDR/STR
        case 3: w = (w & 0x01FFFFFF) | 0x08000000 | (0xEu << 28); break;                           // LDM/STM
        case 4: w = (w & 0x00FFFFF0) | 0x00000090 | (0xEu << 28); w &= 0xF0FFFFFF; break;         // multiplies
        case 5: w = (w & 0x00FFFFFF) | 0x0A000000 | (rnd() % 15) << 28; w = (w & 0xFF00000F) | (rnd() & 0xFF0); break;
        case 6: { static const uint8_t m[] = {0x10, 0x11, 0x12, 0x13, 0x17, 0x1B, 0x1F};           // MSR, #mode
                  w = 0xE329F000u | (rnd() & 1) << 22 | (rnd() & 0xC0) | m[rnd() % 7]; break; }
        default: break;
        }
        put32(fw, i, w);
    }
    for (uint32_t i = 0x20000; i < fw.size(); ++i) fw[i] = (uint8_t)rnd();
    return fw;
}

std::vector<uint8_t> st018Cart() {
    std::vector<uint8_t> img(0x80000, 0);
    const char* title = "MMSIM ST018          ";
    for (int i = 0; i < 21; ++i) img[0x7FC0 + i] = (uint8_t)title[i];
    img[0x7FBF] = 0x02;                          // cartridge type: ST018
    img[0x7FD5] = 0x20; img[0x7FD6] = 0xF5; img[0x7FD7] = 0x09; img[0x7FD8] = 0x03;
    img[0x7FDC] = 0xFF; img[0x7FDD] = 0xFF;
    img[0x7FFC] = 0x00; img[0x7FFD] = 0x80;
    img[0] = 0x80; img[1] = 0xFE;                // BRA *
    return img;
}

}  // namespace

TEST_CASE(snes_st018_arm_matches_mesen) {
    snes::St018 c(randomFirmware());
    int n = 0;
    while (n < 100000) { c.runTo(c.cycles() + 1); ++n; }    // one instruction each
    EXPECT_EQ(c.cycles(), (uint64_t)152674);
    EXPECT_EQ(c.cpsr().value(), 0x00000057u);
    const uint32_t want[16] = {0x079c087d, 0x00000000, 0xe1a34e95, 0x00000000, 0xe05d465e, 0x00000000, 0x00000000, 0x00000000,
                               0x00000000, 0x00000000, 0xe06c0ff0, 0xe9aec9f0, 0x00000000, 0x00000000, 0x000000f0, 0xe05d49bc};
    int same = 0;
    for (int r = 0; r < 16; ++r) same += c.reg(r) == want[r];
    EXPECT_EQ(same, 16);
    uint32_t sum = 0;
    for (uint32_t a = 0; a < snes::St018::RAM_SIZE; ++a) sum = sum * 31 + c.workRam(a);
    EXPECT_EQ(sum, 0x52fc3dbeu);
}

TEST_CASE(snes_st018_link) {
    snes::St018 c(echoFirmware());
    EXPECT_EQ(c.read(0x3804, true), 0x80);              // running, nothing waiting
    c.runTo(100);
    c.write(0x3802, 0x41);
    EXPECT_EQ(c.read(0x3804, true), 0x88);              // a byte for the ARM
    c.runTo(200);
    EXPECT_EQ(c.read(0x3804, true), 0x81);              // taken, and an answer for the SNES
    EXPECT_EQ(c.read(0x3800, false), 0x42);
    EXPECT_EQ(c.read(0x3804, true), 0x80);
    // Held in reset: the ARM stops, and restarts from 0 when released
    c.write(0x3804, 0x01);
    EXPECT_EQ(c.read(0x3804, true), 0x00);
    c.write(0x3802, 0x10);
    c.runTo(1000);
    EXPECT_EQ(c.read(0x3804, true), 0x08);              // not taken
    c.write(0x3804, 0x00);
    EXPECT_EQ(c.reg(15), 8u);                           // the pipeline refilled from 0
    c.runTo(1100);
    EXPECT_EQ(c.read(0x3800, false), 0x11);
}

TEST_CASE(snes_st018_cartridge) {
    const std::string dir = "/tmp/mmemu_st018_fw";
    mkdir(dir.c_str(), 0755);
    const auto fw = echoFirmware();
    { std::ofstream f(dir + "/st018.rom", std::ios::binary); f.write((const char*)fw.data(), (std::streamsize)fw.size()); }
    setenv("MMEMU_SNES_FIRMWARE", dir.c_str(), 1);
    std::string err;
    MachineDescriptor* m = snes::create(false, &err);
    ASSERT(snes::insertCartridge(m, st018Cart(), "st.sfc", err));
    EXPECT_EQ(snes::cartridgeInfo(m), std::string("st.sfc (MMSIM ST018, LoROM, 512K ROM, 8K RAM, battery, ST018)"));
    IBus* bus = m->buses[0].bus;
    bus->write8(0x003802, 0x7F);                         // through the SNES bus, $00 and $80 banks
    uint64_t t = 0;
    while (t < 20000) t += (uint64_t)m->schedulerStep(*m);
    EXPECT_EQ(bus->read8(0x803804) & 0x01, 0x01);
    EXPECT_EQ(bus->read8(0x003800), 0x80);
    // Without the firmware the cartridge is refused, naming the file
    setenv("MMEMU_SNES_FIRMWARE", "/nonexistent", 1);
    EXPECT_TRUE(!snes::insertCartridge(m, st018Cart(), "st.sfc", err));
    EXPECT_TRUE(err.find("/nonexistent/st018.rom") != std::string::npos);
    unsetenv("MMEMU_SNES_FIRMWARE");
    delete m;
    std::remove((dir + "/st018.rom").c_str());
    std::remove(dir.c_str());
}
