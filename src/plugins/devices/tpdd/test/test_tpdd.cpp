#include "test_harness.h"
#include "plugins/devices/tpdd/main/tpdd.h"
#include "plugins/devices/im6402/main/im6402.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

// A fresh, empty host directory under the system temp directory
struct TempDir {
    fs::path path;
    explicit TempDir(const std::string& tag) {
        path = fs::temp_directory_path() / ("mmsim_tpdd_" + tag + "_" + std::to_string(::getpid()));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    void write(const std::string& name, const std::string& data) const {
        std::ofstream(path / name, std::ios::binary) << data;
    }
    std::string read(const std::string& name) const {
        std::ifstream f(path / name, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }
};

void sendAll(Tpdd& d, const std::vector<uint8_t>& bytes) {
    for (uint8_t b : bytes) d.send(b);
}
void sendStr(Tpdd& d, const std::string& s) {
    for (char c : s) d.send((uint8_t)c);
}

// One operation-mode request; returns everything the drive answered
std::vector<uint8_t> xfer(Tpdd& d, uint8_t type, const std::vector<uint8_t>& data = {}) {
    sendAll(d, Tpdd::request(type, data));
    return d.takeOutput();
}

std::vector<uint8_t> nameField(const std::string& s, uint8_t form) {
    std::vector<uint8_t> v;
    std::string n = s;
    n.resize(24, ' ');
    for (char c : n) v.push_back((uint8_t)c);
    v.push_back('F');
    v.push_back(form);
    return v;
}

// A $12 normal return's error code (or -1 if the answer is something else)
int err(const std::vector<uint8_t>& r) {
    if (r.size() != 4 || r[0] != 0x12 || r[1] != 1) return -1;
    if (Tpdd::checksum(r.data(), 3) != r[3]) return -2;
    return r[2];
}

std::string entryName(const std::vector<uint8_t>& r) {
    return std::string(r.begin() + 2, r.begin() + 26);
}

}  // namespace

TEST_CASE(tpdd_packets) {
    // The wiki's precomputed Close: 5A 5A 02 00 FD
    const auto close = Tpdd::request(0x02, {});
    ASSERT_EQ(close.size(), 5u);
    ASSERT_EQ(close[4], 0xFD);
    ASSERT_EQ(Tpdd::tpddName("HELLO.BA"), std::string("HELLO .BA") + std::string(15, ' '));
    ASSERT_EQ(Tpdd::tpddName("a.do").substr(0, 9), "A     .DO");
    ASSERT_EQ(Tpdd::tpddName("longfilename.txt").substr(0, 16), "longfilename.txt");
    // A corrupted checksum is refused
    Tpdd d;
    auto bad = Tpdd::request(0x07, {});
    bad.back() ^= 1;
    sendAll(d, bad);
    ASSERT_EQ(err(d.takeOutput()), 0x41);
    // No directory: no files
    ASSERT_EQ(err(xfer(d, 0x00, nameField("", 1))), 0x70);
}

// Directory listing: first / next, the 6.2 names, sizes, free sectors
TEST_CASE(tpdd_directory) {
    TempDir t("dir");
    t.write("HELLO.BA", std::string(300, 'x'));
    t.write("TEXT.DO", "hello");
    t.write("longfilename.txt", "12345");
    fs::create_directory(t.path / "GAMES");
    Tpdd d;
    ASSERT(d.setRootDirectory(t.path.string()));
    auto r = xfer(d, 0x00, nameField("", 1));
    ASSERT_EQ(r.size(), 31u);
    ASSERT_EQ(r[0], 0x11);
    ASSERT_EQ(r[1], 28);
    ASSERT_EQ(Tpdd::checksum(r.data(), 30), r[30]);
    ASSERT_EQ(entryName(r).substr(0, 9), "GAMES .<>");
    r = xfer(d, 0x00, nameField("", 2));
    ASSERT_EQ(entryName(r).substr(0, 9), "HELLO .BA");
    ASSERT_EQ(r[26], 'F');
    ASSERT_EQ(r[27], 0x01);                // 300 bytes, big-endian
    ASSERT_EQ(r[28], 0x2C);
    ASSERT_EQ(r[29], 80 - 3);              // free sectors: three files of one sector
    r = xfer(d, 0x00, nameField("", 2));
    ASSERT_EQ(entryName(r).substr(0, 9), "TEXT  .DO");
    r = xfer(d, 0x00, nameField("", 2));
    ASSERT_EQ(entryName(r).substr(0, 16), "longfilename.txt");
    r = xfer(d, 0x00, nameField("", 2));
    ASSERT_EQ(r[2], 0);                    // the end: an empty name
    ASSERT_EQ(r[26], 0);
    // Reference by name, case-insensitively
    r = xfer(d, 0x00, nameField("text  .do", 0));
    ASSERT_EQ(entryName(r).substr(0, 9), "TEXT  .DO");
    ASSERT_EQ(r[28], 5);
    // A wrong length is a parameter error
    ASSERT_EQ(err(xfer(d, 0x00, {0x00})), 0x36);
}

// Write a new file in two blocks, read it back in 128-byte blocks
TEST_CASE(tpdd_write_and_read) {
    TempDir t("rw");
    Tpdd d;
    ASSERT(d.setRootDirectory(t.path.string()));
    std::vector<uint8_t> data(200);
    for (size_t i = 0; i < data.size(); ++i) data[i] = (uint8_t)i;
    auto r = xfer(d, 0x00, nameField("NEW   .DO", 0));
    ASSERT_EQ(r[0], 0x11);
    ASSERT_EQ(r[2], 0);                    // not there yet
    ASSERT_EQ(err(xfer(d, 0x01, {0x01})), 0x00);
    ASSERT_EQ(err(xfer(d, 0x04, std::vector<uint8_t>(data.begin(), data.begin() + 128))), 0x00);
    ASSERT_EQ(err(xfer(d, 0x04, std::vector<uint8_t>(data.begin() + 128, data.end()))), 0x00);
    ASSERT_EQ(err(xfer(d, 0x03)), 0x37);   // read while open for write: mismatch
    ASSERT_EQ(err(xfer(d, 0x02)), 0x00);
    const std::string host = t.read("NEW.DO");
    ASSERT_EQ(host.size(), 200u);
    ASSERT_EQ((uint8_t)host[199], 199);
    // Read it back
    xfer(d, 0x00, nameField("NEW   .DO", 0));
    ASSERT_EQ(err(xfer(d, 0x01, {0x03})), 0x00);
    r = xfer(d, 0x03);
    ASSERT_EQ(r[0], 0x10);
    ASSERT_EQ(r[1], 128);
    ASSERT_EQ(r[2 + 127], 127);
    r = xfer(d, 0x03);
    ASSERT_EQ(r[1], 72);
    ASSERT_EQ(r[2 + 71], 199);
    ASSERT_EQ(Tpdd::checksum(r.data(), 74), r[74]);
    ASSERT_EQ(err(xfer(d, 0x03)), 0x3F);   // end of file
    ASSERT_EQ(err(xfer(d, 0x04, {1})), 0x37);
    xfer(d, 0x02);
    // Append
    xfer(d, 0x00, nameField("NEW   .DO", 0));
    ASSERT_EQ(err(xfer(d, 0x01, {0x02})), 0x00);
    ASSERT_EQ(err(xfer(d, 0x04, {0xEE})), 0x00);
    xfer(d, 0x02);
    ASSERT_EQ(t.read("NEW.DO").size(), 201u);
    // Errors: open for read of a missing file; open without a name; bad mode
    xfer(d, 0x00, nameField("NOPE  .BA", 0));
    ASSERT_EQ(err(xfer(d, 0x01, {0x03})), 0x10);
    ASSERT_EQ(err(xfer(d, 0x01, {0x07})), 0x36);
    Tpdd e;
    e.setRootDirectory(t.path.string());
    ASSERT_EQ(err(xfer(e, 0x01, {0x03})), 0x30);
    // Write protect
    e.setWriteProtect(true);
    xfer(e, 0x00, nameField("X     .DO", 0));
    ASSERT_EQ(err(xfer(e, 0x01, {0x01})), 0x50);
    ASSERT_EQ(xfer(e, 0x0C), (std::vector<uint8_t>{0x15, 0x01, 0x02, (uint8_t)~(0x15 + 0x01 + 0x02)}));
    ASSERT_EQ(err(xfer(e, 0x07)), 0x00);
}

TEST_CASE(tpdd_delete_and_rename) {
    TempDir t("ren");
    t.write("OLD.BA", "abc");
    Tpdd d(Tpdd::Model::Tpdd2);
    ASSERT(d.setRootDirectory(t.path.string()));
    xfer(d, 0x00, nameField("OLD   .BA", 0));
    std::vector<uint8_t> to = nameField("NEW   .BA", 0);
    to.resize(25);                          // name + attribute
    ASSERT_EQ(err(xfer(d, 0x0D, to)), 0x00);
    ASSERT(fs::exists(t.path / "NEW.BA"));
    ASSERT(!fs::exists(t.path / "OLD.BA"));
    // Bank 1 ($40 + type) sees the same directory on this emulation
    auto r = xfer(d, 0x40, nameField("NEW   .BA", 0));
    ASSERT_EQ(entryName(r).substr(0, 9), "NEW   .BA");
    ASSERT_EQ(err(xfer(d, 0x05)), 0x00);
    ASSERT(!fs::exists(t.path / "NEW.BA"));
    ASSERT_EQ(err(xfer(d, 0x05)), 0x30);   // the reference went with it
    xfer(d, 0x00, nameField("GONE  .BA", 0));
    ASSERT_EQ(err(xfer(d, 0x05)), 0x10);
}

// TS-DOS sub-directories and the DME probe
TEST_CASE(tpdd_tsdos_directories) {
    TempDir t("tsdos");
    fs::create_directory(t.path / "GAMES");
    t.write("GAMES/CHESS.BA", "x");
    Tpdd d;
    ASSERT(d.setRootDirectory(t.path.string()));
    // DME: "M1\r" then request $08 -> the current directory's name
    sendStr(d, "M1\r");
    auto r = xfer(d, 0x08);
    ASSERT_EQ(r.size(), 14u);
    ASSERT_EQ(r[0], 0x12);
    ASSERT_EQ(r[1], 0x0B);
    ASSERT_EQ(std::string(r.begin() + 3, r.begin() + 13), "ROOT  .<> ");
    ASSERT(!d.fdcMode());
    // Enter GAMES
    xfer(d, 0x00, nameField("GAMES .<>", 0));
    ASSERT_EQ(err(xfer(d, 0x01, {0x03})), 0x00);
    ASSERT_EQ(d.currentDirectory(), "GAMES");
    r = xfer(d, 0x00, nameField("", 1));
    ASSERT_EQ(entryName(r).substr(0, 9), "PARENT.<>");
    r = xfer(d, 0x00, nameField("", 2));
    ASSERT_EQ(entryName(r).substr(0, 9), "CHESS .BA");
    sendStr(d, "M1\r");
    r = xfer(d, 0x08);
    ASSERT_EQ(std::string(r.begin() + 3, r.begin() + 13), "GAMES .<> ");
    // Back up, then make a directory
    xfer(d, 0x00, nameField("PARENT.<>", 0));
    ASSERT_EQ(err(xfer(d, 0x01, {0x03})), 0x00);
    ASSERT_EQ(d.currentDirectory(), "");
    xfer(d, 0x00, nameField("WORK  .<>", 0));
    ASSERT_EQ(err(xfer(d, 0x01, {0x01})), 0x00);
    ASSERT(fs::is_directory(t.path / "WORK"));
}

// TPDD1 FDC mode on a sector image: format, sector and ID access, search
TEST_CASE(tpdd_fdc_mode) {
    Tpdd d;
    ASSERT(xfer(d, 0x08).empty());          // into FDC mode: no answer
    ASSERT(d.fdcMode());
    sendStr(d, "D\r");
    ASSERT_EQ(d.takeOutput(), (std::vector<uint8_t>{'0', '0', '0', '0', '0', '0', '0', '0'}));
    sendStr(d, "F0\r");                     // 64-byte logical sectors
    auto r = d.takeOutput();
    ASSERT_EQ(std::string(r.begin(), r.end()), "00000000");
    sendStr(d, "W 3,2\r");
    r = d.takeOutput();
    ASSERT_EQ(std::string(r.begin(), r.end()), "00030040");
    std::vector<uint8_t> data(64);
    for (int i = 0; i < 64; ++i) data[i] = (uint8_t)(i * 3);
    sendAll(d, data);
    r = d.takeOutput();
    ASSERT_EQ(std::string(r.begin(), r.end()), "00030040");
    ASSERT_EQ(d.sectors()[3].data[64 + 10], 30);
    sendStr(d, "R3,2\r");
    r = d.takeOutput();
    ASSERT_EQ(std::string(r.begin(), r.end()), "00030040");
    sendStr(d, "\r");
    ASSERT_EQ(d.takeOutput(), data);
    // IDs: write, read, search
    sendStr(d, "B7\r");
    r = d.takeOutput();
    ASSERT_EQ(std::string(r.begin(), r.end()), "00070000");
    const std::vector<uint8_t> id = {0xFF, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    sendAll(d, id);
    d.takeOutput();
    sendStr(d, "A7\r");
    r = d.takeOutput();
    ASSERT_EQ(std::string(r.begin(), r.end()), "00070040");
    sendStr(d, "\r");
    ASSERT_EQ(d.takeOutput(), id);
    sendStr(d, "S\r");
    d.takeOutput();
    sendAll(d, id);
    r = d.takeOutput();
    ASSERT_EQ(std::string(r.begin(), r.end()), "00070040");
    sendStr(d, "S\r");
    d.takeOutput();
    sendAll(d, std::vector<uint8_t>(12, 0xAB));
    r = d.takeOutput();
    ASSERT_EQ(std::string(r.begin(), r.end()), "3CFF0000");
    // Range errors
    sendStr(d, "R80,1\r");
    r = d.takeOutput();
    ASSERT_EQ(std::string(r.begin(), r.begin() + 2), "13");
    sendStr(d, "R3,21\r");
    r = d.takeOutput();
    ASSERT_EQ(std::string(r.begin(), r.begin() + 2), "12");
    sendStr(d, "Q\r");
    r = d.takeOutput();
    ASSERT_EQ(std::string(r.begin(), r.begin() + 2), "C0");
    d.setWriteProtect(true);
    sendStr(d, "W3,1\r");
    r = d.takeOutput();
    ASSERT_EQ(std::string(r.begin(), r.begin() + 2), "B0");
    sendStr(d, "D\r");
    r = d.takeOutput();
    ASSERT_EQ(std::string(r.begin(), r.end()), "00200000");
    // Back to operation mode
    sendStr(d, "M1\r");
    ASSERT(!d.fdcMode());
    ASSERT_EQ(err(xfer(d, 0x07)), 0x00);
}

// Sector images: .pdd1 round trip, raw dumps, and a new image file
TEST_CASE(tpdd_images) {
    TempDir t("img");
    Tpdd d;
    d.sectors()[5].lsc = 6;
    d.sectors()[5].id[0] = 0x42;
    d.sectors()[5].data[1279] = 0x99;
    const std::string img = (t.path / "disk.pdd1").string();
    ASSERT(d.saveImage(img));
    ASSERT_EQ(fs::file_size(img), (uintmax_t)(80 * (1 + 12 + 1280)));
    Tpdd e;
    ASSERT(e.loadImage(img));
    ASSERT_EQ(e.sectors()[5].lsc, 6);
    ASSERT_EQ(e.sectors()[5].id[0], 0x42);
    ASSERT_EQ(e.sectors()[5].data[1279], 0x99);
    // FDC writes go back to the file
    xfer(e, 0x08);
    sendStr(e, "W5,1\r");
    e.takeOutput();
    sendAll(e, std::vector<uint8_t>(1280, 0x5A));
    Tpdd f;
    ASSERT(f.loadImage(img));
    ASSERT_EQ(f.sectors()[5].data[0], 0x5A);
    // Raw: 102400 bytes, no IDs
    const std::string raw = (t.path / "disk.bin").string();
    ASSERT(f.saveImage(raw, true));
    ASSERT_EQ(fs::file_size(raw), (uintmax_t)102400);
    Tpdd g;
    ASSERT(g.loadImage(raw));
    ASSERT_EQ(g.sectors()[5].data[0], 0x5A);
    // A TPDD2 image is twice the size; a TPDD1 can't load it
    Tpdd h(Tpdd::Model::Tpdd2);
    ASSERT_EQ(h.sectors().size(), 160u);
    ASSERT(!h.loadImage(img));
    // A new .pdd1 is created on the first write; other missing names fail
    ASSERT(!g.loadImage((t.path / "missing.bin").string()));
    Tpdd n;
    ASSERT(n.loadImage((t.path / "new.pdd1").string()));
    xfer(n, 0x08);
    sendStr(n, "F6\r");
    ASSERT(fs::exists(t.path / "new.pdd1"));
}

// TPDD2: version, system info, the sector cache and its memory access
TEST_CASE(tpdd2_sector_cache) {
    Tpdd d(Tpdd::Model::Tpdd2);
    auto r = xfer(d, 0x23);
    ASSERT_EQ(r.size(), 18u);
    ASSERT_EQ(r[0], 0x14);
    ASSERT_EQ(r[13], 0xE1);                // the model code pdd.sh checks
    ASSERT_EQ(r[17], 0x2A);                // pdd.sh's recorded checksum
    r = xfer(d, 0x33);
    ASSERT_EQ(r[0], 0x3A);
    ASSERT_EQ(err(xfer(d, 0x08)), 0x36);   // no FDC mode on a TPDD2
    // Fill the cache, commit it to track 3 sector 1 (physical 7)
    ASSERT_EQ(xfer(d, 0x31, {0x00, 0x00, 0x10, 0xDE, 0xAD}), (std::vector<uint8_t>{0x38, 0x01, 0x00, 0xC6}));
    xfer(d, 0x31, {0x01, 0x80, 0x04, 0x77});   // the ID's first byte
    r = xfer(d, 0x30, {0x01, 0x00, 0x03, 0x00, 0x01});
    ASSERT_EQ(r[0], 0x38);
    ASSERT_EQ(r[2], 0x00);
    ASSERT_EQ(d.sectors()[7].data[0x10], 0xDE);
    ASSERT_EQ(d.sectors()[7].id[0], 0x77);
    // Load another sector, then that one back, and read the cache
    xfer(d, 0x30, {0x00, 0x00, 0x00, 0x00, 0x00});
    xfer(d, 0x30, {0x00, 0x00, 0x03, 0x00, 0x01});
    r = xfer(d, 0x32, {0x00, 0x00, 0x10, 0x02});
    ASSERT_EQ(r[0], 0x39);
    ASSERT_EQ(r[1], 5);
    ASSERT_EQ(r[5], 0xDE);
    ASSERT_EQ(r[6], 0xAD);
    r = xfer(d, 0x32, {0x01, 0x80, 0x04, 0x01});
    ASSERT_EQ(r[5], 0x77);
    // Bad track
    r = xfer(d, 0x30, {0x00, 0x00, 0x50, 0x00, 0x00});
    ASSERT_EQ(r[2], 0x36);
    // A TPDD1 doesn't answer the version request
    Tpdd one;
    ASSERT(xfer(one, 0x23).empty());
}

// End to end: the Model 100's UART talking to the drive at 19200 baud
TEST_CASE(tpdd_through_im6402) {
    TempDir t("uart");
    t.write("HELLO.BA", "10 PRINT");
    Tpdd::registerSchemes();
    Im6402 uart;
    uart.writeControl(Im6402::CTRL_CLS1 | Im6402::CTRL_CLS2 | Im6402::CTRL_PI);   // 8N1
    ASSERT(uart.attachSerial("tpdd:" + t.path.string()));
    ASSERT(!uart.attachSerial("tpdd:" + (t.path / "nonexistent.zzz").string()));
    const auto req = Tpdd::request(0x00, nameField("", 1));
    std::vector<uint8_t> got;
    size_t next = 0;
    for (int step = 0; step < 200000 && got.size() < 31; ++step) {
        if (next < req.size() && uart.tbre()) uart.writeTbr(req[next++]);
        uart.tick16x(16);
        if (uart.dr()) got.push_back(uart.readData());
    }
    ASSERT_EQ(got.size(), 31u);
    ASSERT_EQ(got[0], 0x11);
    ASSERT_EQ(std::string(got.begin() + 2, got.begin() + 11), "HELLO .BA");
    ASSERT(!uart.oe());
    ASSERT_EQ(uart.endpoint()->describe().substr(0, 5), "tpdd:");
}
