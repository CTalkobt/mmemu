// The SPC7110: its map, the data port, the ALU, the RTC-4513 and the
// decompressor. The decompression vectors came from Mesen2's own
// Spc7110Decomp.cpp, compiled on its own and fed the same pseudo-random
// data ROM (there's no SPC7110 compressor to make meaningful data).

#include "test_harness.h"
#include "plugins/machines/snes/main/machine_snes.h"
#include "plugins/machines/snes/main/snes_spc7110.h"
#include "libcore/main/machine_desc.h"
#include "libmem/main/ibus.h"
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace {

// 2 MB: 1 MB of program, 1 MB of data ROM. Data ROM: 4 KB of LCG bytes (seeded `seed`),
// a directory at $1000 of three entries (modes 0, 1, 2, all from data $000010), markers further on.
std::vector<uint8_t> spcRom(uint32_t seed, bool rtc) {
    std::vector<uint8_t> img(0x200000, 0);
    const char* title = "MMSIM SPC7110        ";
    for (int i = 0; i < 21; ++i) img[0xFFC0 + i] = (uint8_t)title[i];
    img[0xFFD5] = 0x3A;                       // HiROM, FastROM
    img[0xFFD6] = rtc ? 0xF9 : 0xF5;
    img[0xFFD7] = 0x0B;
    img[0xFFD8] = 0x03;                       // 8 KB RAM
    img[0xFFBF] = 0x00;                       // cartridge type: SPC7110
    img[0xFFDC] = 0xFF; img[0xFFDD] = 0xFF;
    img[0xFFFC] = 0x00; img[0xFFFD] = 0x80;
    img[0x8000] = 0x80; img[0x8001] = 0xFE;   // BRA *
    uint32_t x = seed;
    for (uint32_t i = 0; i < 0x1000; ++i) { x = x * 1103515245u + 12345u; img[0x100000 + i] = (uint8_t)(x >> 16); }
    for (int e = 0; e < 3; ++e) { img[0x101000 + e * 4] = (uint8_t)e; img[0x101003 + e * 4] = 0x10; }
    img[0x0C1234] = 0xA1;                     // program ROM, $0C:1234 / $CC:1234
    img[0x110000] = 0xD1;                     // data ROM $010000
    img[0x1F0000] = 0xD2;                     // data ROM $0F0000
    return img;
}

// Mesen's results for seed 1: the first eight decodes of each mode
const uint32_t WANT[3][8] = {
    {0x00000002, 0x0000023f, 0x00023f0d, 0x023f0d7e, 0x3f0d7efc, 0x0d7efc8f, 0x7efc8f0a, 0xfc8f0a79},
    {0x00000000, 0x00000a00, 0x00000da0, 0x0000c29c, 0x00003f16, 0x00009b8f, 0x0000d513, 0x0000fe8f},
    {0x361d152c, 0x0593c1a6, 0xc4b77892, 0xfc9997e8, 0xa5f46478, 0xd5ab380f, 0x35333880, 0xe2a5ed96},
};

// The tile bytes the chip gives for those decodes
std::vector<uint8_t> tile(int mode) {
    std::vector<uint8_t> t(8u << mode);
    for (int i = 0; i < 8; ++i) {
        const uint32_t r = WANT[mode][i];
        if (mode == 0) t[i] = (uint8_t)r;
        else { t[i * 2] = (uint8_t)r; t[i * 2 + 1] = (uint8_t)(r >> 8); }
        if (mode == 2) { t[i * 2 + 16] = (uint8_t)(r >> 16); t[i * 2 + 17] = (uint8_t)(r >> 24); }
    }
    return t;
}

void w(snes::Spc7110& c, uint16_t a, uint8_t v) { c.cpuWrite(0x00, a, v); }
uint8_t r(snes::Spc7110& c, uint16_t a) { return (uint8_t)c.cpuRead(0x00, a, false); }

void startDecomp(snes::Spc7110& c, uint8_t index) {
    w(c, 0x4801, 0x00); w(c, 0x4802, 0x10); w(c, 0x4803, 0x00);   // directory at $001000
    w(c, 0x4804, index);
    w(c, 0x4805, 0x00); w(c, 0x4806, 0x00);                       // go
}

}  // namespace

TEST_CASE(snes_spc7110_map) {
    std::string err;
    MachineDescriptor* m = snes::create(false, &err);
    ASSERT(snes::insertCartridge(m, spcRom(1, false), "spc.sfc", err));
    EXPECT_EQ(snes::cartridgeInfo(m), std::string("spc.sfc (MMSIM SPC7110, HiROM, 2048K ROM, 8K RAM, battery, SPC7110)"));
    IBus* bus = m->buses[0].bus;
    EXPECT_EQ(bus->peek8(0x008000), 0x80);                   // the program, HiROM
    EXPECT_EQ(bus->peek8(0xCC1234), 0xA1);                   // and at $C0-$CF
    EXPECT_EQ(bus->peek8(0xD10000), 0xD1);                   // data window 0 = data ROM MB 0
    EXPECT_EQ(bus->peek8(0xDF0000), 0xD2);
    bus->write8(0x004831, 0x01);                             // 1 MB of data ROM: MB 1 wraps to MB 0
    EXPECT_EQ(bus->peek8(0xD10000), 0xD1);
    EXPECT_EQ(bus->peek8(0x004831), 0x01);
    bus->write8(0x306000, 0x5A);                             // RAM: $30:6000 = offset $0000 of 8 KB
    EXPECT_EQ(bus->peek8(0x006000), 0x5A);
    EXPECT_EQ(bus->peek8(0x806000), 0x5A);
    delete m;
}

TEST_CASE(snes_spc7110_alu) {
    const auto img = spcRom(1, false);
    snes::Spc7110 c(&img, nullptr, false);
    // Unsigned 16 x 16: $1234 x $5678
    w(c, 0x4820, 0x34); w(c, 0x4821, 0x12); w(c, 0x4824, 0x78); w(c, 0x4825, 0x56);
    EXPECT_EQ(r(c, 0x4828) | r(c, 0x4829) << 8 | r(c, 0x482A) << 16 | (uint32_t)r(c, 0x482B) << 24, 0x1234u * 0x5678u);
    EXPECT_EQ(r(c, 0x482F), 0x01);
    // Signed: -2 x 3 = -6
    w(c, 0x482E, 0x01);
    w(c, 0x4820, 0xFE); w(c, 0x4821, 0xFF); w(c, 0x4824, 0x03); w(c, 0x4825, 0x00);
    EXPECT_EQ(r(c, 0x4828) | r(c, 0x4829) << 8 | r(c, 0x482A) << 16 | (uint32_t)r(c, 0x482B) << 24, 0xFFFFFFFAu);
    // Signed 32 / 16: -100 / 7 = -14 remainder -2
    w(c, 0x4820, 0x9C); w(c, 0x4821, 0xFF); w(c, 0x4822, 0xFF); w(c, 0x4823, 0xFF);
    w(c, 0x4826, 0x07); w(c, 0x4827, 0x00);
    EXPECT_EQ(r(c, 0x4828) | r(c, 0x4829) << 8 | r(c, 0x482A) << 16 | (uint32_t)r(c, 0x482B) << 24, 0xFFFFFFF2u);
    EXPECT_EQ(r(c, 0x482C) | r(c, 0x482D) << 8, 0xFFFE);
    // Unsigned 100000 / 7 = 14285 remainder 5; dividing by zero: 0 remainder the dividend's low word
    w(c, 0x482E, 0x00);
    w(c, 0x4820, 0xA0); w(c, 0x4821, 0x86); w(c, 0x4822, 0x01); w(c, 0x4823, 0x00);
    w(c, 0x4826, 0x07); w(c, 0x4827, 0x00);
    EXPECT_EQ(r(c, 0x4828) | r(c, 0x4829) << 8 | r(c, 0x482A) << 16, 14285);
    EXPECT_EQ(r(c, 0x482C), 5);
    w(c, 0x4826, 0x00); w(c, 0x4827, 0x00);
    EXPECT_EQ(r(c, 0x4828) | r(c, 0x4829) << 8, 0);
    EXPECT_EQ(r(c, 0x482C) | r(c, 0x482D) << 8, 0x86A0);
}

TEST_CASE(snes_spc7110_data_port) {
    const auto img = spcRom(1, false);
    snes::Spc7110 c(&img, nullptr, false);
    const uint8_t* d = &img[0x100000];
    // Base $000100, step 1: reads walk the data ROM
    w(c, 0x4818, 0x00);
    w(c, 0x4811, 0x00); w(c, 0x4812, 0x01); w(c, 0x4813, 0x00);
    EXPECT_EQ(r(c, 0x4810), d[0x100]);
    EXPECT_EQ(r(c, 0x4810), d[0x101]);
    EXPECT_EQ(r(c, 0x4811), 0x02);
    // Step $0010 (mode bit 0)
    w(c, 0x4816, 0x10); w(c, 0x4817, 0x00); w(c, 0x4818, 0x01);
    EXPECT_EQ(r(c, 0x4810), d[0x102]);
    EXPECT_EQ(r(c, 0x4810), d[0x112]);
    // Base + offset (bit 1), the step moving the offset (bit 4): base stays, offset grows
    w(c, 0x4811, 0x00); w(c, 0x4812, 0x02); w(c, 0x4813, 0x00);
    w(c, 0x4814, 0x05); w(c, 0x4815, 0x00); w(c, 0x4818, 0x13);
    EXPECT_EQ(r(c, 0x4810), d[0x205]);
    EXPECT_EQ(r(c, 0x4810), d[0x215]);
    EXPECT_EQ(r(c, 0x4812), 0x02);
    // Bit 5-6 = 3: reading $481A adds the offset to the base
    w(c, 0x4818, 0x60);
    w(c, 0x4811, 0x00); w(c, 0x4812, 0x03); w(c, 0x4813, 0x00);
    w(c, 0x4814, 0x40); w(c, 0x4815, 0x00);
    r(c, 0x481A);
    EXPECT_EQ(r(c, 0x4811) | r(c, 0x4812) << 8, 0x0340);
    EXPECT_EQ(r(c, 0x4810), d[0x340]);
    // Past the 1 MB the size register allows: 0
    w(c, 0x4818, 0x00);
    w(c, 0x4811, 0x00); w(c, 0x4812, 0x00); w(c, 0x4813, 0x10);
    EXPECT_EQ(r(c, 0x4810), 0x00);
}

TEST_CASE(snes_spc7110_decompression) {
    const auto img = spcRom(1, false);
    for (int mode = 0; mode < 3; ++mode) {
        snes::Spc7110 c(&img, nullptr, false);
        startDecomp(c, (uint8_t)mode);
        EXPECT_EQ(r(c, 0x480C), 0x80);
        const auto want = tile(mode);
        int same = 0;
        for (size_t i = 0; i < want.size(); ++i) same += (uint8_t)c.cpuRead(0x50, (uint16_t)i, false) == want[i];
        EXPECT_EQ(same, (int)want.size());
    }
    // Skipping (flag 1): with skip 2 each row is every other decode
    snes::Spc7110 c(&img, nullptr, false);
    w(c, 0x480B, 0x01); w(c, 0x4807, 0x02);
    startDecomp(c, 0);
    int same = 0;
    for (int i = 0; i < 4; ++i) same += r(c, 0x4800) == (uint8_t)WANT[0][i * 2];
    EXPECT_EQ(same, 4);
    // The length counter counts down with each read
    w(c, 0x4809, 0x10); w(c, 0x480A, 0x00);
    r(c, 0x4800);
    EXPECT_EQ(r(c, 0x4809), 0x0F);
}

TEST_CASE(snes_spc7110_rtc) {
    const auto img = spcRom(1, true);
    snes::Spc7110 c(&img, nullptr, true);
    int64_t now = 1000000000;
    c.clock = [&now] { return now; };
    // Set 1999-12-31 23:59:05, a Friday (5)
    const uint8_t set[16] = {5, 0, 9, 5, 3, 2, 1, 3, 2, 1, 9, 9, 5, 0, 0, 0};
    w(c, 0x4840, 0x01); w(c, 0x4841, 0x03); w(c, 0x4841, 0x00);
    for (uint8_t v : set) w(c, 0x4841, v);
    w(c, 0x4840, 0x00);
    now += 60;                                               // a minute on: 2000-01-01 00:00:05, Saturday
    const uint8_t want[13] = {5, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 6};
    w(c, 0x4840, 0x01); w(c, 0x4841, 0x0C); w(c, 0x4841, 0x00);
    EXPECT_EQ(r(c, 0x4842), 0x80);
    int same = 0;
    for (uint8_t v : want) same += r(c, 0x4841) == v;
    EXPECT_EQ(same, 13);
    // Its state survives a round trip through the battery file layout
    snes::Spc7110 c2(&img, nullptr, true);
    c2.clock = [&now] { return now; };
    c2.setRtcState(c.rtcState());
    EXPECT_EQ(c2.rtcState() == c.rtcState(), true);
}

TEST_CASE(snes_spc7110_rtc_file) {
    const std::string save = "/tmp/mmemu_spc7110_test.srm", rtc = "/tmp/mmemu_spc7110_test.rtc";
    std::remove(save.c_str());
    std::remove(rtc.c_str());
    std::string err;
    MachineDescriptor* m = snes::create(false, &err);
    ASSERT(snes::insertCartridge(m, spcRom(1, true), "spc.sfc", err, save));
    EXPECT_EQ(snes::cartridgeInfo(m), std::string("spc.sfc (MMSIM SPC7110, HiROM, 2048K ROM, 8K RAM, battery, SPC7110, RTC)"));
    IBus* bus = m->buses[0].bus;
    // Set the month to 7 through the bus, then save
    bus->write8(0x004840, 0x01); bus->write8(0x004841, 0x03); bus->write8(0x004841, 0x08);
    bus->write8(0x004841, 0x07);
    bus->write8(0x004840, 0x00);
    snes::flushSave(m);
    std::ifstream f(rtc, std::ios::binary);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    EXPECT_EQ(data.size(), (size_t)24);
    if (data.size() == 24) EXPECT_EQ(data[8], 0x07);
    delete m;
    std::remove(save.c_str());
    std::remove(rtc.c_str());
}
