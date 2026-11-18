#include "test_harness.h"
#include "plugins/devices/drivewire/main/drivewire_server.h"
#include <cstdio>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

// The DriveWire 4 server's protocol, byte by byte, as NitrOS-9's rbdw /
// dwio / clock2_dw and HDB-DOS drive it (#356).

namespace {

using DW = DriveWireServer;

std::string tempImage(const std::string& tag, size_t sectors, uint8_t salt = 0) {
    const std::string path = "/tmp/mmsim_dw_" + std::to_string((long)getpid()) + "_" + tag + ".dsk";
    std::vector<uint8_t> img(sectors * 256);
    for (size_t i = 0; i < img.size(); ++i) img[i] = (uint8_t)((i / 256) * 7 + i + salt);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write((const char*)img.data(), (std::streamsize)img.size());
    return path;
}

std::vector<uint8_t> fileBytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void sendAll(DW& dw, std::initializer_list<int> bytes) {
    for (int b : bytes) dw.receive((uint8_t)b);
}

std::vector<uint8_t> drain(DW& dw) {
    std::vector<uint8_t> out;
    while (dw.hasOutput()) out.push_back(dw.send());
    return out;
}

uint8_t expected(size_t lsn, size_t i, uint8_t salt = 0) { return (uint8_t)(lsn * 7 + lsn * 256 + i + salt); }

}  // namespace

// OP_READEX: the sector, the guest's checksum, then 0
TEST_CASE(drivewire_readex_with_checksum) {
    const std::string img = tempImage("readex", 700);
    DW dw;
    ASSERT(dw.mount(0, img));
    sendAll(dw, {DW::OP_READEX, 0, 0x00, 0x02, 0x63});      // drive 0, LSN 611 ($000263)
    ASSERT(dw.idle() == false);                             // waiting for the checksum
    std::vector<uint8_t> data = drain(dw);
    ASSERT_EQ((int)data.size(), 256);
    for (size_t i = 0; i < 256; ++i) ASSERT_EQ(data[i], expected(611, i));
    const uint16_t sum = DW::checksum(data.data(), 256);
    dw.receive((uint8_t)(sum >> 8));
    ASSERT(!dw.hasOutput());
    dw.receive((uint8_t)sum);
    ASSERT_EQ(dw.send(), DW::E_OK);
    ASSERT(!dw.hasOutput());
    ASSERT(dw.idle());
    ASSERT_EQ((int)dw.stats().reads, 1);
    ASSERT_EQ((int)dw.stats().lastLsn, 611);
    std::remove(img.c_str());
}

// A wrong checksum gets E$CRC; OP_REREADEX sends the sector again
TEST_CASE(drivewire_readex_crc_then_reread) {
    const std::string img = tempImage("reread", 40);
    DW dw;
    ASSERT(dw.mount(1, img));
    sendAll(dw, {DW::OP_READEX, 1, 0, 0, 5});
    std::vector<uint8_t> data = drain(dw);
    const uint16_t sum = DW::checksum(data.data(), 256);
    sendAll(dw, {(sum >> 8) ^ 0x01, sum & 0xFF});           // as if a byte was lost
    ASSERT_EQ(dw.send(), DW::E_CRC);
    ASSERT_EQ((int)dw.stats().crcErrors, 1);
    sendAll(dw, {DW::OP_REREADEX, 1, 0, 0, 5});
    std::vector<uint8_t> again = drain(dw);
    ASSERT(again == data);
    sendAll(dw, {sum >> 8, sum & 0xFF});
    ASSERT_EQ(dw.send(), DW::E_OK);
    ASSERT_EQ((int)dw.stats().rereads, 1);
    std::remove(img.c_str());
}

// No image: 256 zeros, then E$NotRdy whatever the checksum
TEST_CASE(drivewire_readex_empty_drive) {
    DW dw;
    sendAll(dw, {DW::OP_READEX, 3, 0, 0, 0});
    std::vector<uint8_t> data = drain(dw);
    ASSERT_EQ((int)data.size(), 256);
    for (uint8_t b : data) ASSERT_EQ(b, 0);
    sendAll(dw, {0, 0});
    ASSERT_EQ(dw.send(), DW::E_NOTRDY);
    ASSERT(!dw.hasOutput());
}

// OP_READ (DW3): status, checksum, data; an error is the one byte
TEST_CASE(drivewire_read_dw3) {
    const std::string img = tempImage("read", 4);
    DW dw;
    ASSERT(dw.mount(0, img));
    sendAll(dw, {DW::OP_READ, 0, 0, 0, 2});
    std::vector<uint8_t> r = drain(dw);
    ASSERT_EQ((int)r.size(), 259);
    ASSERT_EQ(r[0], 0);
    const uint16_t sum = DW::checksum(&r[3], 256);
    ASSERT_EQ(r[1], (uint8_t)(sum >> 8));
    ASSERT_EQ(r[2], (uint8_t)sum);
    ASSERT_EQ(r[3 + 10], expected(2, 10));
    sendAll(dw, {DW::OP_READ, 9, 0, 0, 2});
    r = drain(dw);
    ASSERT_EQ((int)r.size(), 1);
    ASSERT_EQ(r[0], DW::E_NOTRDY);
    std::remove(img.c_str());
}

// OP_WRITE: through to the file; a bad checksum is E$CRC and writes nothing; OP_REWRITE then
TEST_CASE(drivewire_write_and_checksum_error) {
    const std::string img = tempImage("write", 10);
    DW dw;
    ASSERT(dw.mount(0, img));
    std::vector<uint8_t> sector(256);
    for (int i = 0; i < 256; ++i) sector[i] = (uint8_t)(0xA0 ^ i);
    const uint16_t sum = DW::checksum(sector.data(), 256);
    auto write = [&](uint8_t op, uint16_t check) {
        sendAll(dw, {op, 0, 0, 0, 4});
        for (uint8_t b : sector) dw.receive(b);
        ASSERT(!dw.hasOutput());
        dw.receive((uint8_t)(check >> 8));
        dw.receive((uint8_t)check);
    };
    write(DW::OP_WRITE, (uint16_t)(sum + 1));
    ASSERT_EQ(dw.send(), DW::E_CRC);
    std::vector<uint8_t> f = fileBytes(img);
    ASSERT_EQ(f[4 * 256], expected(4, 0));                   // unchanged
    write(DW::OP_REWRITE, sum);
    ASSERT_EQ(dw.send(), DW::E_OK);
    f = fileBytes(img);
    for (int i = 0; i < 256; ++i) ASSERT_EQ(f[4 * 256 + i], sector[i]);
    ASSERT_EQ(f[5 * 256], expected(5, 0));
    // Past the end: the image grows (zeros between)
    sendAll(dw, {DW::OP_WRITE, 0, 0, 0, 12});
    for (uint8_t b : sector) dw.receive(b);
    sendAll(dw, {sum >> 8, sum & 0xFF});
    ASSERT_EQ(dw.send(), DW::E_OK);
    f = fileBytes(img);
    ASSERT_EQ((int)f.size(), 13 * 256);
    ASSERT_EQ(f[11 * 256], 0);
    ASSERT_EQ(f[12 * 256 + 1], sector[1]);
    // And reads back through the server
    uint8_t back[256];
    ASSERT_EQ(dw.readSector(0, 12, back), DW::E_OK);
    ASSERT_EQ(back[255], sector[255]);
    std::remove(img.c_str());
}

// Copy-on-write keeps writes in memory; read-only refuses them
TEST_CASE(drivewire_copy_on_write_and_read_only) {
    const std::string img = tempImage("cow", 8);
    const std::vector<uint8_t> before = fileBytes(img);
    DW dw;
    ASSERT(dw.mount(0, img, DW::Mode::CopyOnWrite));
    ASSERT(dw.mount(1, img, DW::Mode::ReadOnly));
    std::vector<uint8_t> sector(256, 0x5A);
    const uint16_t sum = DW::checksum(sector.data(), 256);
    for (int drive = 0; drive < 2; ++drive) {
        sendAll(dw, {DW::OP_WRITE, drive, 0, 0, 3});
        for (uint8_t b : sector) dw.receive(b);
        sendAll(dw, {sum >> 8, sum & 0xFF});
        ASSERT_EQ(dw.send(), drive == 0 ? DW::E_OK : DW::E_WP);
    }
    ASSERT(fileBytes(img) == before);
    uint8_t back[256];
    dw.readSector(0, 3, back);
    ASSERT_EQ(back[0], 0x5A);
    dw.readSector(1, 3, back);
    ASSERT_EQ(back[0], expected(3, 0));
    ASSERT(dw.describe().find("drive 0: mmsim_dw_") != std::string::npos);
    ASSERT(dw.describe().find("(copy on write)") != std::string::npos);
    std::remove(img.c_str());
}

// OP_TIME: six bytes from the clock; OP_SETTIME's six are taken and ignored
TEST_CASE(drivewire_time) {
    DW dw;
    dw.setClock([] {
        std::tm t{};
        t.tm_year = 126; t.tm_mon = 10; t.tm_mday = 17; t.tm_hour = 20; t.tm_min = 5; t.tm_sec = 42;
        return t;
    });
    dw.receive(DW::OP_TIME);
    const std::vector<uint8_t> t = drain(dw);
    ASSERT_EQ((int)t.size(), 6);
    ASSERT_EQ(t[0], 126);                                   // 2026
    ASSERT_EQ(t[1], 11);                                    // November (1-12)
    ASSERT_EQ(t[2], 17);
    ASSERT_EQ(t[3], 20);
    ASSERT_EQ(t[4], 5);
    ASSERT_EQ(t[5], 42);
    sendAll(dw, {DW::OP_SETTIME, 126, 1, 2, 3, 4, 5});
    ASSERT(dw.idle());
    ASSERT(!dw.hasOutput());
}

// INIT, TERM, the RESETs, NOP, GETSTAT/SETSTAT: one-way; DWINIT answers 4
TEST_CASE(drivewire_init_term_reset) {
    DW dw;
    sendAll(dw, {DW::OP_RESET3, DW::OP_RESET1, DW::OP_RESET2, DW::OP_NOP, DW::OP_INIT, DW::OP_TERM});
    ASSERT(!dw.hasOutput());
    ASSERT(dw.idle());
    sendAll(dw, {DW::OP_GETSTAT, 0, 0x27, DW::OP_SETSTAT, 0, 0x29});
    ASSERT(!dw.hasOutput());
    ASSERT(dw.idle());
    sendAll(dw, {DW::OP_DWINIT, 1});                        // NitrOS-9's dwio, driver version 1
    ASSERT_EQ(dw.send(), DW::SERVER_VERSION);
    ASSERT(!dw.hasOutput());
    // An unknown opcode is one byte, ignored
    dw.receive(0x0C);
    ASSERT(dw.idle());
    ASSERT(!dw.hasOutput());
    ASSERT_EQ((int)dw.stats().unknownOps, 1);
    // The guest's reset drops a half-sent command and unread answers
    sendAll(dw, {DW::OP_READEX, 0, 0});
    dw.receive(DW::OP_TIME);                                // (part of the LSN)
    dw.reset();
    ASSERT(dw.idle());
    dw.receive(DW::OP_TIME);
    ASSERT_EQ((int)drain(dw).size(), 6);
}

// Virtual serial: SERREAD answers "no data"; the rest is taken whole and dropped
TEST_CASE(drivewire_serread_no_data) {
    DW dw;
    dw.receive(DW::OP_SERREAD);
    std::vector<uint8_t> r = drain(dw);
    ASSERT_EQ((int)r.size(), 2);
    ASSERT_EQ(r[0], 0);
    ASSERT_EQ(r[1], 0);
    ASSERT_EQ((int)dw.stats().serialPolls, 1);
    sendAll(dw, {DW::OP_SERINIT, 1, DW::OP_SERWRITE, 1, 'A', DW::OP_FASTWRITE + 2, 'B', DW::OP_SERGETSTAT, 1, 0x01});
    sendAll(dw, {DW::OP_SERWRITEM, 1, 3, 'x', 'y', 'z'});
    ASSERT(dw.idle());
    // SS.ComSt carries 26 more bytes
    sendAll(dw, {DW::OP_SERSETSTAT, 1, DW::SS_COMST});
    for (int i = 0; i < 25; ++i) dw.receive(0x55);
    ASSERT(!dw.idle());
    dw.receive(0x55);
    ASSERT(dw.idle());
    sendAll(dw, {DW::OP_SERSETSTAT, 1, 0x2A, DW::OP_SERTERM, 1});
    ASSERT(dw.idle());
    ASSERT(!dw.hasOutput());
    ASSERT_EQ((int)dw.stats().unknownOps, 0);
    // A named object can't be mounted
    sendAll(dw, {DW::OP_NAMEOBJ_MOUNT, 3, 'A', 'B', 'C'});
    ASSERT_EQ(dw.send(), 0);
    ASSERT(dw.idle());
    // The printer keeps its bytes
    sendAll(dw, {DW::OP_PRINT, 'H', DW::OP_PRINT, 'I', DW::OP_PRINTFLUSH});
    ASSERT(dw.printBuffer() == std::vector<uint8_t>({'H', 'I'}));
}

// A command cut short is dropped after 250 ms, and the stream goes on
TEST_CASE(drivewire_command_timeout) {
    DW dw;
    sendAll(dw, {DW::OP_WRITE, 0, 0, 0, 1, 0x11, 0x22});
    dw.advance(0.1);
    ASSERT(!dw.idle());
    dw.advance(0.2);
    ASSERT(dw.idle());
    dw.receive(DW::OP_SERREAD);
    ASSERT_EQ((int)drain(dw).size(), 2);
}
