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

// Burst format (GCR, no directory), query disk format, inquire status,
// sector interleave and the CHGUTL utilities (1571 User's Guide, Appendix G)
TEST_CASE(image_dos_burst_format_query) {
    std::string path = makeD64();
    CbmImageDos dos;
    ASSERT(dos.open(path));
    auto cmd = [&](std::string bytes) { send(dos, 0xFF, std::string("U0") + bytes); };
    // INQUIRE STATUS, C=1 W=1 before a log-in: disk change ($B)
    cmd(std::string("\xCC", 1));
    ASSERT(dos.burstOut().size() == 1 && dos.burstOut().front() == 0x1B);
    // QUERY DISK FORMAT: a GCR disk, the status byte only; it logs the disk in
    cmd(std::string("\x8A", 1));
    ASSERT(dos.burstOut().size() == 1 && dos.burstOut().front() == 0x10);
    cmd(std::string("\xCC", 1));
    ASSERT(dos.burstOut().size() == 1 && dos.burstOut().front() == 0x10);
    // INQUIRE STATUS W=0: set it; W=1 reads it back
    cmd(std::string("\x0C\x17", 2));
    EXPECT_EQ((int)dos.burstOut().size(), 0);
    cmd(std::string("\x8C", 1));
    ASSERT(dos.burstOut().size() == 1 && dos.burstOut().front() == 0x17);
    // SECTOR INTERLEAVE: read 5, set 3; a 2-sector read then goes 1/0, 1/3
    cmd(std::string("\x88", 1));
    ASSERT(dos.burstOut().size() == 1 && dos.burstOut().front() == 5);
    cmd(std::string("\x08\x03", 2));
    cmd(std::string("\x88", 1));
    EXPECT_EQ(dos.burstOut().front(), 3);
    // READ with T=1 (no transfer): status bytes only
    cmd(std::string("\x80\x01\x00\x02", 4));
    EXPECT_EQ((int)dos.burstOut().size(), 2);
    // CHGUTL: "U0>M1" (1571 mode) answers OK on the error channel
    cmd(">M1");
    EXPECT_EQ(dos.burstOut().size(), (size_t)0);
    // FORMAT GCR (M=0, ID "AB"): every sector $4B, $01...; no output
    cmd(std::string("\x06\x00" "AB", 4));
    EXPECT_EQ((int)dos.burstOut().size(), 0);
    cmd(std::string("\xCC", 1));                                  // to be logged in again
    EXPECT_EQ(dos.burstOut().front(), 0x1B);
    cmd(std::string("\x04", 1));
    cmd(std::string("\x00\x12\x00\x01", 4));                   // 18/0
    ASSERT_EQ((int)dos.burstOut().size(), 257);
    EXPECT_EQ(dos.burstOut()[1], 0x4B);
    EXPECT_EQ(dos.burstOut()[2], 0x01);
    EXPECT_EQ(dos.burstOut()[256], 0x01);
    {
        std::ifstream f(path, std::ios::binary);                   // on the file too
        std::vector<uint8_t> img((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        EXPECT_EQ(img[0x16500], 0x4B);                             // 18/0
        EXPECT_EQ(img[683 * 256 - 1], 0x01);
    }
    // FORMAT MFM on a GCR image: format error
    cmd(std::string("\x06\x80", 2));
    cmd(std::string("\x8C", 1));
    EXPECT_EQ(dos.burstOut().front(), 0x16);
    std::remove(path.c_str());
}

namespace {
// An ImageDisk file: 40 cylinders, side 0, 10 x 512-byte sectors numbered 0-9
// laid out 0 5 1 6 2 7 3 8 4 9; sector (c, s) starts with c, s
std::string makeImd() {
    std::string path = "/tmp/mmemu_test_mfm_" + std::to_string(getpid()) + ".imd";
    std::string hdr = "IMD 1.18: 06/11/2026 12:00:00\r\ntest\r\n\x1a";
    std::vector<uint8_t> d(hdr.begin(), hdr.end());
    const uint8_t order[10] = {0, 5, 1, 6, 2, 7, 3, 8, 4, 9};
    for (int c = 0; c < 40; ++c) {
        const uint8_t t[5] = {5, (uint8_t)c, 0, 10, 2};
        d.insert(d.end(), t, t + 5);
        d.insert(d.end(), order, order + 10);
        for (int i = 0; i < 10; ++i) {
            d.push_back(1);
            std::vector<uint8_t> sec(512, 0xE5);
            sec[0] = (uint8_t)c; sec[1] = order[i];
            d.insert(d.end(), sec.begin(), sec.end());
        }
    }
    std::ofstream(path, std::ios::binary).write((const char*)d.data(), (std::streamsize)d.size());
    return path;
}
}  // namespace

// MFM disks (.imd) through the burst commands, as the C128's CP/M uses
// them: inquire, query, read and write by cylinder / sector number /
// side, disk change before a log-in, the MFM format; no Commodore DOS
TEST_CASE(image_dos_mfm_imd) {
    std::string path = makeImd();
    CbmImageDos dos;
    ASSERT(dos.open(path));
    auto cmd = [&](std::string bytes) { send(dos, 0xFF, std::string("U0") + bytes); };
    auto& q = dos.burstOut();
    cmd(std::string("\x00\x01\x00\x01", 4));                       // read before a log-in
    ASSERT(q.size() == 1 && q.front() == 0xAB);                    // MFM, 512, disk change
    cmd(std::string("\x04", 1));                                   // inquire: MFM, 512-byte sectors
    ASSERT(q.size() == 1 && q.front() == 0xA0);
    cmd(std::string("\x8A\x04", 2));                               // query track 4
    ASSERT_EQ((int)q.size(), 7);
    EXPECT_EQ(q[0], 0xA0); EXPECT_EQ(q[1], 0xA0);
    EXPECT_EQ(q[2], 10);                                           // sectors
    EXPECT_EQ(q[3], 4);                                            // the track in the headers
    EXPECT_EQ(q[4], 0); EXPECT_EQ(q[5], 9);                        // lowest, highest
    EXPECT_EQ(q[6], 2);                                            // interleave
    cmd(std::string("\x00\x03\x04\x02", 4));                       // read 3/4, 3/5
    ASSERT_EQ((int)q.size(), 2 * 513);
    EXPECT_EQ(q[1], 3); EXPECT_EQ(q[2], 4);
    EXPECT_EQ(q[514], 3); EXPECT_EQ(q[515], 5);
    cmd(std::string("\x10\x03\x00\x01", 4));                       // side 1: none
    ASSERT(q.size() == 1 && (q.front() & 0x0F) == 0x03);
    cmd(std::string("\x00\x03\x0C\x01", 4));                       // no sector 12
    ASSERT(q.size() == 1 && (q.front() & 0x0F) == 0x02);
    // Write 7/9: 512 bytes, then the status; on the file
    cmd(std::string("\x02\x07\x09\x01", 4));
    ASSERT(dos.burstWriting());
    for (int i = 0; i < 511; ++i) ASSERT(!dos.burstWriteByte((uint8_t)i));
    ASSERT(dos.burstWriteByte(0x99));
    dos.burstFinishSector();
    ASSERT(q.back() == 0xA0);
    {
        CbmImageDos again;
        ASSERT(again.open(path));
        auto c2 = [&](std::string bytes) { send(again, 0xFF, std::string("U0") + bytes); };
        c2(std::string("\x04", 1));
        c2(std::string("\x00\x07\x09\x01", 4));
        ASSERT_EQ((int)again.burstOut().size(), 513);
        EXPECT_EQ(again.burstOut()[1], 0x00);
        EXPECT_EQ(again.burstOut()[512], 0x99);
    }
    // No Commodore DOS on it
    send(dos, 0xF0, "$");
    std::string err;
    uint8_t ch = 0;
    dos.talk(0x48); dos.tksa(0x6F);
    while (dos.acptr(&ch) >= 0 && ch != '\r') err += (char)ch;
    dos.untlk();
    EXPECT_EQ(err.substr(0, 2), std::string("74"));
    // FORMAT MFM: IBM-8 style, 8 x 512 numbered from 1, both sides, 40 cylinders
    cmd(std::string("\x26\x81\x00\x02\x27\x08\x00\x00\xE5", 9));
    cmd(std::string("\x04", 1));
    cmd(std::string("\x9A\x00", 2));                               // query side 1
    ASSERT_EQ((int)q.size(), 7);
    EXPECT_EQ(q[2], 8); EXPECT_EQ(q[4], 1); EXPECT_EQ(q[5], 8);
    cmd(std::string("\x00\x27\x08\x01", 4));                       // the last sector of the last track
    ASSERT_EQ((int)q.size(), 513);
    EXPECT_EQ(q[1], 0xE5);
    std::remove(path.c_str());
}
