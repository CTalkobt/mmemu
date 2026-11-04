#include "test_harness.h"
#include "plugins/devices/vera/main/sdcard_spi.h"
#include <cstdio>
#include <fstream>
#include <vector>

namespace {
// Send a 6-byte command, then clock $FF until a response byte other than $FF
uint8_t command(SdCardSpi& sd, uint8_t cmd, uint32_t arg) {
    sd.select(true);
    uint8_t frame[6] = {(uint8_t)(0x40 | cmd), (uint8_t)(arg >> 24), (uint8_t)(arg >> 16),
                        (uint8_t)(arg >> 8), (uint8_t)arg, 0x95};
    for (uint8_t b : frame) sd.handle(b);
    for (int i = 0; i < 16; i++) {
        uint8_t r = sd.handle(0xFF);
        if (r != 0xFF) return r;
    }
    return 0xFF;
}

std::string makeImage(size_t blocks) {
    std::string path = std::string(P_tmpdir) + "/mmemu_sdcard_test.img";
    std::vector<uint8_t> data(blocks * 512);
    for (size_t i = 0; i < data.size(); i++) data[i] = (uint8_t)(i / 512 + i);
    std::ofstream(path, std::ios::binary).write((const char*)data.data(), data.size());
    return path;
}
}

TEST_CASE(sdcard_spi_init_read_and_write_block) {
    std::string path = makeImage(16);
    SdCardSpi sd;
    ASSERT(sd.attach(path));
    ASSERT_EQ((int)command(sd, 0, 0), 0x01);        // CMD0: idle
    ASSERT_EQ((int)command(sd, 8, 0x1AA), 0x01);    // CMD8: R7, idle
    for (int i = 0; i < 4; i++) sd.handle(0xFF);    // rest of R7
    ASSERT_EQ((int)command(sd, 55, 0), 0x01);
    ASSERT_EQ((int)command(sd, 41, 0x40000000), 0x00);  // ACMD41: ready

    // CMD17 block 3: R1 0, data token, 512 bytes
    ASSERT_EQ((int)command(sd, 17, 3), 0x00);
    ASSERT_EQ((int)sd.handle(0xFF), 0xFE);
    for (int i = 0; i < 512; i++) ASSERT_EQ((int)sd.handle(0xFF), (int)(uint8_t)(3 + 3 * 512 + i));
    sd.handle(0xFF); sd.handle(0xFF);               // CRC

    // CMD24 block 5: token, 512 bytes, CRC
    ASSERT_EQ((int)command(sd, 24, 5), 0x00);
    sd.handle(0xFE);
    for (int i = 0; i < 512; i++) sd.handle((uint8_t)(0xA5 ^ i));
    sd.handle(0xFF); sd.handle(0xFF);
    sd.detach();

    std::ifstream f(path, std::ios::binary);
    f.seekg(5 * 512);
    std::vector<char> blk(512);
    f.read(blk.data(), 512);
    for (int i = 0; i < 512; i++) ASSERT_EQ((int)(uint8_t)blk[i], (int)(uint8_t)(0xA5 ^ i));
    std::remove(path.c_str());
}

TEST_CASE(sdcard_spi_out_of_range_and_deselected) {
    std::string path = makeImage(4);
    SdCardSpi sd;
    ASSERT(sd.attach(path));
    ASSERT_EQ((int)command(sd, 17, 100), 0x00);
    ASSERT_EQ((int)sd.handle(0xFF), 0x08);          // error token: out of range
    sd.select(false);
    ASSERT_EQ((int)sd.handle(0x40), 0xFF);          // not selected: no answer
    std::remove(path.c_str());
}
