#include "test_harness.h"
#include "plugins/devices/drivewire/main/drivewire_server.h"
#include <cstdio>
#include <fstream>
#include <string>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
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

// ---------------------------------------------------------------------------
// Virtual channels: dwio's OP_SERREAD poll and scdwv's writes and status
// calls (NitrOS-9 level1/modules/dwio.asm, scdwv.asm)
// ---------------------------------------------------------------------------
namespace {

std::vector<uint8_t> serRead(DW& dw) {
    dw.receive(DW::OP_SERREAD);
    return drain(dw);
}

std::vector<uint8_t> bytes(std::initializer_list<int> b) {
    std::vector<uint8_t> v;
    for (int x : b) v.push_back((uint8_t)x);
    return v;
}

// A channel's answers until it has nothing (single bytes and SERREADMs), and the last poll's answer
std::string readChannel(DW& dw, int port, std::vector<uint8_t>& last) {
    std::string s;
    for (;;) {
        last = serRead(dw);
        if (last[0] == 1 + port) {
            s += (char)last[1];
        } else if (last[0] == 0x11 + port) {
            sendAll(dw, {DW::OP_SERREADM, port, last[1]});
            for (uint8_t b : drain(dw)) s += (char)b;
        } else {
            return s;
        }
    }
}

}  // namespace

// One or two bytes waiting: (1 + port, byte) per poll; a channel the guest's
// driver hasn't opened (OP_SERINIT) isn't offered
TEST_CASE(drivewire_serread_single_byte) {
    DW dw;
    dw.hostWrite(0, "A");
    ASSERT(serRead(dw) == bytes({0, 0}));                   // /Term not open yet: kept
    sendAll(dw, {DW::OP_SERINIT, 0});
    ASSERT(dw.channelOpen(0));
    ASSERT(serRead(dw) == bytes({1, 'A'}));
    ASSERT(serRead(dw) == bytes({0, 0}));
    dw.hostWrite(7, "xy");
    sendAll(dw, {DW::OP_SERINIT, 7});
    ASSERT(serRead(dw) == bytes({8, 'x'}));
    ASSERT(serRead(dw) == bytes({8, 'y'}));
    ASSERT(serRead(dw) == bytes({0, 0}));
    ASSERT_EQ((int)dw.stats().serialIn, 3);
    ASSERT(dw.idle());
}

// Three or more: ($11 + port, count), then OP_SERREADM port, n takes n of them
TEST_CASE(drivewire_serread_multi_byte) {
    DW dw;
    sendAll(dw, {DW::OP_SERINIT, 5});
    dw.hostWrite(5, "0123456789");
    ASSERT(serRead(dw) == bytes({0x16, 10}));
    sendAll(dw, {DW::OP_SERREADM, 5, 4});                   // dwio's buffer had room for 4
    ASSERT(drain(dw) == bytes({'0', '1', '2', '3'}));
    ASSERT(serRead(dw) == bytes({0x16, 6}));
    sendAll(dw, {DW::OP_SERREADM, 5, 6});
    ASSERT(drain(dw) == bytes({'4', '5', '6', '7', '8', '9'}));
    ASSERT(serRead(dw) == bytes({0, 0}));
    // At most 255 announced
    dw.hostWrite(5, std::string(300, 'z'));
    ASSERT(serRead(dw) == bytes({0x16, 255}));
    sendAll(dw, {DW::OP_SERREADM, 5, 255});
    ASSERT_EQ((int)drain(dw).size(), 255);
    ASSERT(serRead(dw) == bytes({0x16, 45}));
    ASSERT_EQ((int)dw.hostPending(5), 45);
}

// A window (/Z1-/Z15, ports $11-$1F) takes keys one at a time: ($40 + n, key)
TEST_CASE(drivewire_serread_window_keys) {
    DW dw;
    sendAll(dw, {DW::OP_SERINIT, 0x11, DW::OP_SERINIT, 0x1F});
    dw.hostWrite(0x11, "dir");
    ASSERT(serRead(dw) == bytes({0x41, 'd'}));
    ASSERT(serRead(dw) == bytes({0x41, 'i'}));
    ASSERT(serRead(dw) == bytes({0x41, 'r'}));
    dw.hostWrite(0x1F, "q");
    ASSERT(serRead(dw) == bytes({0x4F, 'q'}));
    ASSERT(serRead(dw) == bytes({0, 0}));
}

// Channels with data take turns
TEST_CASE(drivewire_serread_round_robin) {
    DW dw;
    sendAll(dw, {DW::OP_SERINIT, 1, DW::OP_SERINIT, 2});
    dw.hostWrite(1, "ac");
    dw.hostWrite(2, "b");
    ASSERT(serRead(dw) == bytes({2, 'a'}));
    ASSERT(serRead(dw) == bytes({3, 'b'}));
    ASSERT(serRead(dw) == bytes({2, 'c'}));
    ASSERT(serRead(dw) == bytes({0, 0}));
}

// SERWRITE, SERWRITEM and FASTWRITE ($80 + port) reach their own channels
TEST_CASE(drivewire_serwrite_per_channel) {
    DW dw;
    std::string err;
    ASSERT(dw.attachChannel(3, "none", err));               // (not the virtual modem)
    ASSERT(dw.attachChannel(4, "none", err));
    sendAll(dw, {DW::OP_SERINIT, 0, DW::OP_SERINIT, 3, DW::OP_SERINIT, 4, DW::OP_SERINIT, 0x11});
    sendAll(dw, {DW::OP_SERWRITE, 3, 'a', DW::OP_SERWRITEM, 4, 3, 'x', 'y', 'z'});
    sendAll(dw, {DW::OP_FASTWRITE + 0, 'T', DW::OP_FASTWRITE + 3, 'b', DW::OP_FASTWRITE + 0x11, 'w'});
    sendAll(dw, {DW::OP_SERWRITEM, 4, 0});                  // (an empty one)
    ASSERT(dw.idle());
    ASSERT(!dw.hasOutput());
    ASSERT_EQ(dw.channelOutput(0), std::string("T"));
    ASSERT_EQ(dw.channelOutput(3), std::string("ab"));
    ASSERT_EQ(dw.channelOutput(4), std::string("xyz"));
    ASSERT_EQ(dw.channelOutput(0x11), std::string("w"));
    ASSERT_EQ((int)dw.stats().serialOut, 7);
    ASSERT_EQ((int)dw.stats().unknownOps, 0);
    dw.clearChannelOutput(3);
    ASSERT(dw.channelOutput(3).empty());
    // To a far end: loopback (cooked: LF and CR LF in become CR, telnet's IAC dropped)
    ASSERT(dw.attachChannel(3, "loopback", err));
    sendAll(dw, {DW::OP_SERWRITEM, 3, 9, 'o', 'k', '\n', 0xFF, 0xFB, 0x01, 'z', '\r', '\n'});
    dw.pollChannels();
    ASSERT(serRead(dw) == bytes({0x14, 5}));
    sendAll(dw, {DW::OP_SERREADM, 3, 5});
    ASSERT(drain(dw) == bytes({'o', 'k', '\r', 'z', '\r'}));
    ASSERT(dw.attachChannel(3, "loopback,raw", err));
    sendAll(dw, {DW::OP_SERWRITE, 3, '\n'});
    dw.pollChannels();
    ASSERT(serRead(dw) == bytes({4, '\n'}));
    ASSERT(!dw.attachChannel(3, "bogus:1", err));
    ASSERT(!dw.attachChannel(0, "modem", err));             // (the modem is on /N1-/N14)
}

// SS.Open / SS.Close (SERSETSTAT) mark a path open; SS.ComSt carries PD.OPT's
// 26 bytes; SERTERM closes the channel
TEST_CASE(drivewire_sersetstat_open_close_comst) {
    DW dw;
    sendAll(dw, {DW::OP_SERINIT, 2, DW::OP_SERSETSTAT, 2, DW::SS_OPEN});
    ASSERT(dw.channelOpen(2));
    ASSERT(dw.channelPathOpen(2));
    sendAll(dw, {DW::OP_SERSETSTAT, 2, DW::SS_COMST});
    for (int i = 0; i < DW::COMST_BYTES; ++i) {
        ASSERT(!dw.idle());
        dw.receive((uint8_t)(0x40 + i));
    }
    ASSERT(dw.idle());
    const std::vector<uint8_t> opt = dw.channelComSt(2);
    ASSERT_EQ((int)opt.size(), 26);
    ASSERT_EQ(opt[0], 0x40);
    ASSERT_EQ(opt[25], 0x40 + 25);
    sendAll(dw, {DW::OP_SERGETSTAT, 2, 0x26});              // advertised, no answer
    ASSERT(!dw.hasOutput());
    sendAll(dw, {DW::OP_SERGETSTAT, 2, DW::SS_KYSNS});      // Level 1 reads SS.KySns's byte
    ASSERT(drain(dw) == bytes({0}));
    sendAll(dw, {DW::OP_SERGETSTAT, 2, DW::SS_KYSNS});      // Level 2 doesn't: the next command drops it
    ASSERT_EQ((int)dw.outputSize(), 1);
    ASSERT(serRead(dw) == bytes({0, 0}));
    sendAll(dw, {DW::OP_SERSETSTAT, 2, DW::SS_CLOSE});
    ASSERT(!dw.channelPathOpen(2));
    ASSERT(dw.channelOpen(2));
    sendAll(dw, {DW::OP_SERTERM, 2});
    ASSERT(!dw.channelOpen(2));
    ASSERT(!dw.hasOutput());
    // The guest's reset (and dwio's OP_DWINIT) close them all
    sendAll(dw, {DW::OP_SERINIT, 2, DW::OP_SERINIT, 0});
    sendAll(dw, {DW::OP_DWINIT, 1});
    ASSERT_EQ(dw.send(), DW::SERVER_VERSION);
    ASSERT(!dw.channelOpen(2));
    ASSERT(!dw.channelOpen(0));
}

// The host hangs up: ($10, port) once what was waiting has been read and
// its reader has had CLOSE_DELAY to take it
TEST_CASE(drivewire_serread_channel_closed) {
    DW dw;
    sendAll(dw, {DW::OP_SERINIT, 6, DW::OP_SERSETSTAT, 6, DW::SS_OPEN, DW::OP_SERINIT, 8});
    dw.hostClose(8);
    ASSERT(serRead(dw) == bytes({0x10, 8}));                // (nothing was read: at once)
    dw.hostWrite(6, "!");
    dw.hostClose(6);
    ASSERT(serRead(dw) == bytes({7, '!'}));
    ASSERT(serRead(dw) == bytes({0, 0}));
    dw.advance(DW::CLOSE_DELAY / 2);
    ASSERT(serRead(dw) == bytes({0, 0}));
    dw.advance(DW::CLOSE_DELAY / 2);
    ASSERT(serRead(dw) == bytes({0x10, 6}));
    ASSERT(serRead(dw) == bytes({0, 0}));                   // once
    dw.hostClose(0x11);                                     // (a window doesn't hang up)
    ASSERT(serRead(dw) == bytes({0, 0}));
}

// The virtual modem (/N1-/N14 with no far end): `dw ...` answers, then the
// channel closes; other lines answer FAIL (net.as's "FAIL nnn" format)
TEST_CASE(drivewire_modem_dw_commands) {
    const std::string img = tempImage("modem", 4);
    DW dw;
    ASSERT(dw.mount(0, img));
    sendAll(dw, {DW::OP_SERINIT, 1, DW::OP_SERSETSTAT, 1, DW::SS_OPEN});
    const std::string cmd = "dw disk show\r";
    sendAll(dw, {DW::OP_SERWRITEM, 1, (int)cmd.size()});
    for (char c : cmd) dw.receive((uint8_t)c);
    std::vector<uint8_t> last;
    const std::string text = readChannel(dw, 1, last);
    ASSERT(text.rfind("OK\r", 0) == 0);
    ASSERT(text.find(img) != std::string::npos);
    ASSERT(last == bytes({0, 0}));
    dw.advance(DW::CLOSE_DELAY);
    ASSERT(serRead(dw) == bytes({0x10, 1}));                // then the channel closes
    ASSERT(serRead(dw) == bytes({0, 0}));
    sendAll(dw, {DW::OP_SERSETSTAT, 1, DW::SS_CLOSE, DW::OP_SERTERM, 1});
    // An unknown command
    sendAll(dw, {DW::OP_SERINIT, 1});
    for (char c : std::string("dw frob\r")) sendAll(dw, {DW::OP_FASTWRITE + 1, (uint8_t)c});
    const std::string fail = readChannel(dw, 1, last);
    ASSERT(fail.rfind("FAIL 208 ", 0) == 0);
    dw.advance(DW::CLOSE_DELAY);
    ASSERT(serRead(dw) == bytes({0x10, 1}));
    // Not a command: FAIL, the channel stays
    sendAll(dw, {DW::OP_SERINIT, 1});
    for (char c : std::string("ATZ\r")) sendAll(dw, {DW::OP_FASTWRITE + 1, (uint8_t)c});
    ASSERT(readChannel(dw, 1, last).rfind("FAIL ", 0) == 0);
    ASSERT(last == bytes({0, 0}));
    std::remove(img.c_str());
}

// Ports by name, and the CLI's `drivewire` command
TEST_CASE(drivewire_port_names_and_cli) {
    ASSERT_EQ(DW::parsePort("term"), 0);
    ASSERT_EQ(DW::parsePort("/N3"), 3);
    ASSERT_EQ(DW::parsePort("n13"), 13);
    ASSERT_EQ(DW::parsePort("midi"), 14);
    ASSERT_EQ(DW::parsePort("z1"), 0x11);
    ASSERT_EQ(DW::parsePort("$1F"), 0x1F);
    ASSERT_EQ(DW::parsePort("15"), -1);
    ASSERT_EQ(DW::parsePort("n15"), -1);
    ASSERT_EQ(DW::parsePort("z16"), -1);
    ASSERT_EQ(DW::portName(0), std::string("Term"));
    ASSERT_EQ(DW::portName(0x12), std::string("Z2"));
    DW dw;
    ASSERT(dw.hostCommand({"type", "term", "dir"}).find("4 bytes") != std::string::npos);
    ASSERT_EQ((int)dw.hostPending(0), 4);
    sendAll(dw, {DW::OP_SERINIT, 0, DW::OP_FASTWRITE, 'h', DW::OP_FASTWRITE, '\r', DW::OP_FASTWRITE, '\n'});
    ASSERT_EQ(dw.hostCommand({"read", "term"}), std::string("h\n"));
    ASSERT(dw.hostCommand({"read", "term"}).find("nothing") != std::string::npos);
    ASSERT(dw.hostCommand({}).find("/Term") != std::string::npos);
    ASSERT(dw.hostCommand({"channel", "n2", "loopback"}).find("loopback") != std::string::npos);
    // Network access: off by default; on/off and its synonyms
    ASSERT(dw.hostCommand({}).find("network access (tcp connect): off") != std::string::npos);
    ASSERT(dw.hostCommand({"network", "yes"}).find(": on") != std::string::npos);
    ASSERT(dw.hostCommand({"network"}).find(": on") != std::string::npos);
    ASSERT(dw.hostCommand({"net", "0"}).find(": off") != std::string::npos);
    ASSERT(dw.hostCommand({"network", "maybe"}).find("Syntax") != std::string::npos);
    ASSERT(!dw.network());
}

// `tcp connect HOST PORT` (NitrOS-9's telnet): the channel carries the
// connection both ways; the far end's hang-up closes it
TEST_CASE(drivewire_modem_tcp_connect) {
    const int lfd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT(lfd >= 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT(::bind(lfd, (sockaddr*)&a, sizeof a) == 0 && ::listen(lfd, 1) == 0);
    socklen_t al = sizeof a;
    ::getsockname(lfd, (sockaddr*)&a, &al);
    const int port = ntohs(a.sin_port);
    DW dw;
    ASSERT(!dw.network());                                  // off until the host allows it
    sendAll(dw, {DW::OP_SERINIT, 3, DW::OP_SERSETSTAT, 3, DW::SS_OPEN});
    for (char c : "tcp connect 127.0.0.1 " + std::to_string(port) + "\r") sendAll(dw, {DW::OP_FASTWRITE + 3, (uint8_t)c});
    std::vector<uint8_t> last;
    ASSERT(readChannel(dw, 3, last).rfind("FAIL 205 ", 0) == 0);
    ASSERT(last == bytes({0, 0}));                          // the channel stays, no connection
    pollfd pf{lfd, POLLIN, 0};
    ASSERT_EQ(::poll(&pf, 1, 50), 0);
    ASSERT(dw.hostCommand({"network", "on"}).find(": on") != std::string::npos);
    ASSERT(dw.network());
    for (char c : "tcp connect 127.0.0.1 " + std::to_string(port) + "\r") sendAll(dw, {DW::OP_FASTWRITE + 3, (uint8_t)c});
    ASSERT(readChannel(dw, 3, last).rfind("OK connected to 127.0.0.1:", 0) == 0);
    const int peer = ::accept(lfd, nullptr, nullptr);
    ASSERT(peer >= 0);
    sendAll(dw, {DW::OP_FASTWRITE + 3, 'x', DW::OP_SERWRITEM, 3, 2, 'y', '\n'});
    char buf[8] = {};
    size_t got = 0;
    while (got < 3) {
        const ssize_t n = ::recv(peer, buf + got, sizeof buf - got, 0);
        ASSERT(n > 0);
        got += (size_t)n;
    }
    ASSERT_EQ(std::string(buf, 3), std::string("xy\n"));
    ASSERT(::send(peer, "hi\n", 3, 0) == 3);
    std::string in;
    for (int i = 0; i < 200 && in.size() < 3; ++i) {
        usleep(1000);
        dw.pollChannels();
        in += readChannel(dw, 3, last);
    }
    ASSERT_EQ(in, std::string("hi\n"));                     // (raw: bytes as they come)
    ::close(peer);
    for (int i = 0; i < 200 && dw.channelEndpoint(3).find("connected") != std::string::npos; ++i) {
        usleep(1000);
        dw.pollChannels();
    }
    dw.advance(DW::CLOSE_DELAY);
    ASSERT(serRead(dw) == bytes({0x10, 3}));                // the guest's telnet hears the hang-up
    ::close(lfd);
}

// A hang-up waits until the guest is quiet on DriveWire: while it writes to
// another channel, or has bytes waiting on one, the close isn't announced
// (NitrOS-9 Level 1's dwio would corrupt a busy process queue)
TEST_CASE(drivewire_close_waits_for_a_quiet_guest) {
    DW dw;
    sendAll(dw, {DW::OP_SERINIT, 1, DW::OP_SERINIT, 2});
    dw.hostClose(2);
    dw.advance(DW::CLOSE_DELAY);
    // The guest keeps writing to /N1: no announcement
    for (int i = 0; i < 20; ++i) {
        sendAll(dw, {DW::OP_FASTWRITE + 1, 'x'});
        dw.advance(0.1);
        ASSERT(serRead(dw) == bytes({0, 0}));
    }
    // Bytes waiting for the guest on /N1 go first; the quiet time counts
    // from its last write (0.1 s ago)
    dw.hostWrite(1, "a");
    ASSERT(serRead(dw) == bytes({2, 'a'}));
    dw.advance(DW::CLOSE_QUIET - 0.15);
    ASSERT(serRead(dw) == bytes({0, 0}));
    dw.advance(0.1);
    ASSERT(serRead(dw) == bytes({0x10, 2}));                // quiet: the close
    ASSERT(serRead(dw) == bytes({0, 0}));
}
