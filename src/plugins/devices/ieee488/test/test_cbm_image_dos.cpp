#include "test_harness.h"
#include "plugins/devices/ieee488/main/cbm_image_dos.h"
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>
#include <unistd.h>

namespace {
// A blank D64 with a marked track 1 sector 0 and track 18 sector 0
std::string makeD64() {
    std::string path = "/tmp/mmemu_test_direct_" + std::to_string(getpid()) + ".d64";
    std::vector<uint8_t> d(174848, 0);
    for (int i = 0; i < 256; ++i) d[i] = (uint8_t)i;               // track 1, sector 0
    const size_t bam = (size_t)(17 * 21) * 256;
    d[bam] = 18; d[bam + 1] = 1; d[bam + 2] = 0x41;
    d[bam + 0xA2] = 'I'; d[bam + 0xA3] = 'D';
    std::ofstream(path, std::ios::binary).write((const char*)d.data(), (std::streamsize)d.size());
    return path;
}
// Send `text` to a channel of unit 8 (OPEN with a name, or data)
void send(CbmImageDos& dos, uint8_t sa, const std::string& text) {
    dos.listen(0x28);
    dos.second(sa);
    for (char c : text) dos.ciout((uint8_t)c);
    dos.unlsn();
}
std::string readBytes(CbmImageDos& dos, uint8_t ch, int n, int* lastStatus = nullptr) {
    std::string out;
    dos.talk(0x48);
    dos.tksa((uint8_t)(0x60 | ch));
    int st = 0;
    for (int i = 0; i < n; ++i) {
        uint8_t b = 0;
        st = dos.acptr(&b);
        out += (char)b;
    }
    dos.untlk();
    if (lastStatus) *lastStatus = st;
    return out;
}
}  // namespace

// Direct access as the C128's boot sector and CP/M use it: OPEN "#",
// U1 / B-R block reads, B-P, the error channel
TEST_CASE(image_dos_direct_access) {
    std::string path = makeD64();
    CbmImageDos dos;
    ASSERT(dos.open(path));
    send(dos, 0xF8, "#");                                          // OPEN 13,8,8,"#"
    send(dos, 0xFF, "U1:8 0 01 00\r");                             // OPEN 15: U1 channel 8, track 1, sector 0
    EXPECT_EQ(readBytes(dos, 15, 2), std::string("00"));
    int st = 0;
    std::string blk = readBytes(dos, 8, 256, &st);
    EXPECT_EQ((uint8_t)blk[0], 0);
    EXPECT_EQ((uint8_t)blk[255], 255);
    EXPECT_EQ(st, 0x40);                                           // EOI with byte 255
    send(dos, 0x6F, "B-P 8 16\r");
    EXPECT_EQ((uint8_t)readBytes(dos, 8, 1)[0], 16);
    // B-R: the count is the first byte (track 18 sector 0 starts 18, 1)
    send(dos, 0x6F, "B-R 8 0 18 0\r");
    std::string br = readBytes(dos, 8, 1, &st);
    EXPECT_EQ((uint8_t)br[0], 1);                                  // pointer at 1
    EXPECT_EQ(st, 0x00);
    // Out of range, and writes
    send(dos, 0x6F, "U1:8 0 40 0\r");
    EXPECT_EQ(readBytes(dos, 15, 2), std::string("66"));
    send(dos, 0x6F, "U2:8 0 01 00\r");
    EXPECT_EQ(readBytes(dos, 15, 2), std::string("26"));
    std::remove(path.c_str());
}
