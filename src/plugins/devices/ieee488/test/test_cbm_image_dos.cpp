#include "test_harness.h"
#include "plugins/devices/ieee488/main/cbm_image_dos.h"
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>
#include <unistd.h>
#include <sys/stat.h>

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
    std::remove(path.c_str());
}

// U2 / B-W write the buffer to the image file; a read-only file is write
// protected
TEST_CASE(image_dos_block_write) {
    std::string path = makeD64();
    {
        CbmImageDos dos;
        ASSERT(dos.open(path));
        send(dos, 0xF8, "#");
        send(dos, 0xFF, "B-P 8 0\r");
        send(dos, 0x68, std::string("HELLO"));                     // into the buffer
        send(dos, 0x6F, "U2:8 0 02 03\r");
        EXPECT_EQ(readBytes(dos, 15, 2), std::string("00"));
        send(dos, 0x6F, "B-P 8 0\r");
        send(dos, 0x68, std::string("ABC"));
        send(dos, 0x6F, "B-W 8 0 02 04\r");                        // byte 0 = pointer (3)
        EXPECT_EQ(readBytes(dos, 15, 2), std::string("00"));
        send(dos, 0x6F, "U1:8 0 02 03\r");
        EXPECT_EQ(readBytes(dos, 8, 5), std::string("HELLO"));
    }
    std::ifstream f(path, std::ios::binary);
    std::vector<char> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const size_t t2 = 21 * 256;                                    // track 2 starts after 21 sectors
    EXPECT_EQ(std::string(&d[t2 + 3 * 256], 5), std::string("HELLO"));
    EXPECT_EQ((int)d[t2 + 4 * 256], 3);
    EXPECT_EQ(std::string(&d[t2 + 4 * 256 + 1], 2), std::string("BC"));
    // Read-only on the host: WRITE PROTECT ON
    chmod(path.c_str(), 0444);
    CbmImageDos ro;
    ASSERT(ro.open(path));
    send(ro, 0xF8, "#");
    send(ro, 0xFF, "U2:8 0 02 03\r");
    EXPECT_EQ(readBytes(ro, 15, 2), std::string("26"));
    chmod(path.c_str(), 0644);
    std::remove(path.c_str());
}

// 1571 burst commands ("U0" + bytes): inquire disk, a two-sector read (5
// sectors on, as the C128's CP/M lays out a track), a write, fastload
TEST_CASE(image_dos_burst_commands) {
    std::string path = makeD64();
    CbmImageDos dos;
    ASSERT(dos.open(path));
    auto cmd = [&](std::string bytes) { send(dos, 0xFF, std::string("U0") + bytes); };
    cmd(std::string("\x04", 1));                                   // inquire disk
    ASSERT(dos.burstOut().size() == 1 && dos.burstOut().front() == 0x10);   // GCR, 256 bytes, OK
    cmd(std::string("\x00\x01\x00\x02", 4));                       // read 1/0, 2 sectors
    auto& q = dos.burstOut();
    ASSERT_EQ((int)q.size(), 2 * 257);
    EXPECT_EQ(q[0], 0x10);
    EXPECT_EQ(q[1], 0x00);                                         // 1/0 starts 0, 1, 2...
    EXPECT_EQ(q[2 + 255], 0x10);                                   // the second status
    // the second sector is 1/5
    cmd(std::string("\x00\x01\x0D\x01", 4));                       // track 1 sector 13 ($0D is not a CR here)
    EXPECT_EQ((int)dos.burstOut().size(), 257);
    cmd(std::string("\x00\x01\x30\x01", 4));                       // no sector 48: header not found
    ASSERT(dos.burstOut().size() == 1 && dos.burstOut().front() == 0x12);
    // Write 2/3: 256 bytes, then the status
    cmd(std::string("\x02\x02\x03\x01", 4));
    ASSERT(dos.burstWriting());
    for (int i = 0; i < 255; ++i) ASSERT(!dos.burstWriteByte((uint8_t)i));
    ASSERT(dos.burstWriteByte(0xEE));
    dos.burstFinishSector();
    ASSERT(!dos.burstWriting() && dos.burstOut().back() == 0x10);
    cmd(std::string("\x00\x02\x03\x01", 4));
    EXPECT_EQ(dos.burstOut()[256], 0xEE);
    // Fastload: the drive reads the file itself
    cmd(std::string("\x1F" "PROG", 5));
    std::string name;
    ASSERT(dos.takeFastload(name) && name == "PROG");
    std::remove(path.c_str());
}
