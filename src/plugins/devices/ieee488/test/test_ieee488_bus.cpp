#include "test_harness.h"
#include "plugins/devices/ieee488/main/ieee488_bus.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {
// The PET's side of the bus, through the endpoints its PIAs and VIA use
struct Pet {
    Ieee488Bus bus;
    IPortDevice* dataOut = bus.namedPort("dataOut");
    IPortDevice* dataIn  = bus.namedPort("dataIn");
    IPortDevice* via     = bus.namedPort("via");
    ISignalLine* ndacOut = bus.getSignalLine("ndacOut");
    ISignalLine* davOut  = bus.getSignalLine("davOut");
    ISignalLine* eoiOut  = bus.getSignalLine("eoiOut");
    uint8_t viaOut = 0xFF;

    Pet(const std::string& folder) {
        bus.reset();
        ASSERT(bus.mountDisk(8, folder));
        via->setDdr(0x06);                          // PB1 NRFD, PB2 ATN out
        dataOut->setDdr(0xFF);
    }
    void viaWrite() { via->writePort(viaOut); }
    void atn(bool asserted)  { viaOut = asserted ? (viaOut & ~0x04) : (viaOut | 0x04); viaWrite(); }
    void nrfd(bool asserted) { viaOut = asserted ? (viaOut & ~0x02) : (viaOut | 0x02); viaWrite(); }
    bool davIn()  { return via->readPort() & 0x80; }
    bool nrfdIn() { return via->readPort() & 0x40; }
    bool ndacIn() { return via->readPort() & 0x01; }

    // Talker: one byte (inverted on the lines), EOI on the last
    void send(uint8_t b, bool last = false) {
        ASSERT(nrfdIn());                           // the drive is ready
        dataOut->writePort((uint8_t)~b);
        if (last) eoiOut->set(false);
        davOut->set(false);
        ASSERT(ndacIn());                           // accepted
        davOut->set(true);
        eoiOut->set(true);
        dataOut->writePort(0xFF);
    }
    void command(std::initializer_list<uint8_t> bytes) {
        atn(true);
        for (uint8_t b : bytes) send(b);
    }
    // Listener: one byte; false if the drive sends nothing
    bool receive(uint8_t& b, bool& eoi) {
        nrfd(false);                                // ready
        ndacOut->set(false);
        if (davIn()) return false;                  // no byte on the bus
        b = (uint8_t)~dataIn->readPort();
        eoi = !bus.eoi();
        nrfd(true);                                 // busy, then accept
        ndacOut->set(true);
        ASSERT(davIn());                            // the drive took it away
        ndacOut->set(false);
        return true;
    }
    std::vector<uint8_t> receiveAll() {
        std::vector<uint8_t> out;
        uint8_t b; bool eoi = false;
        while (!eoi && receive(b, eoi)) out.push_back(b);
        return out;
    }
    // After TALK + secondary: release ATN and become the listener
    void turnAround() {
        nrfd(true);
        ndacOut->set(false);
        atn(false);
    }
    void open(int sa, const std::string& name) {
        command({0x28, (uint8_t)(0xF0 | sa)});
        atn(false);
        for (size_t i = 0; i < name.size(); i++) send((uint8_t)name[i], i + 1 == name.size());
        command({0x3F});
        atn(false);
    }
    void close(int sa) { command({0x28, (uint8_t)(0xE0 | sa), 0x3F}); atn(false); }
    void talk(int sa) { command({0x48, (uint8_t)(0x60 | sa)}); turnAround(); }
    void untalk() { ndacOut->set(true); nrfd(false); command({0x5F}); atn(false); }
};

std::string tempFolder(const char* tag) {
    fs::path p = fs::temp_directory_path() / (std::string("mmemu_ieee_") + tag);
    fs::remove_all(p);
    fs::create_directories(p);
    return p.string();
}
}

TEST_CASE(ieee488_drive_loads_a_file_with_eoi) {
    std::string dir = tempFolder("load");
    { std::ofstream(dir + "/HELLO", std::ios::binary) << std::string("\x01\x04\xAA\xBB", 4); }
    Pet pet(dir);
    pet.open(0, "HELLO");
    pet.talk(0);
    auto bytes = pet.receiveAll();
    ASSERT_EQ((int)bytes.size(), 4);
    ASSERT_EQ((int)bytes[0], 0x01);
    ASSERT_EQ((int)bytes[3], 0xBB);                 // the last one came with EOI
    pet.untalk();
    pet.close(0);
    fs::remove_all(dir);
}

TEST_CASE(ieee488_drive_saves_a_file) {
    std::string dir = tempFolder("save");
    Pet pet(dir);
    pet.open(1, "OUT");
    pet.command({0x28, 0x61});                      // LISTEN, data to channel 1
    pet.atn(false);
    pet.send(0x01); pet.send(0x04); pet.send(0x42, true);
    pet.command({0x3F});
    pet.atn(false);
    pet.close(1);
    std::ifstream f(dir + "/OUT", std::ios::binary);
    std::string got((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    ASSERT_EQ(got, std::string("\x01\x04\x42", 3));
    fs::remove_all(dir);
}

// The PET lets go of the bus before UNTALK by releasing NDAC with NRFD high:
// that byte has not arrived and comes again at the next TALK
TEST_CASE(ieee488_drive_resends_a_byte_cut_off_by_untalk) {
    std::string dir = tempFolder("resend");
    { std::ofstream(dir + "/F", std::ios::binary) << std::string("ABCD"); }
    Pet pet(dir);
    pet.open(2, "F");
    pet.talk(2);
    uint8_t b; bool eoi;
    ASSERT(pet.receive(b, eoi)); ASSERT_EQ((int)b, 'A');
    ASSERT(pet.receive(b, eoi)); ASSERT_EQ((int)b, 'B');
    pet.nrfd(false);                                // ready, and 'C' is put on the bus
    ASSERT(!pet.davIn());
    pet.untalk();                                   // NDAC released with NRFD high, then ATN
    pet.talk(2);
    ASSERT(pet.receive(b, eoi)); ASSERT_EQ((int)b, 'C');
    ASSERT(pet.receive(b, eoi)); ASSERT_EQ((int)b, 'D');
    ASSERT(eoi);
    fs::remove_all(dir);
}

TEST_CASE(ieee488_drive_missing_file_and_error_channel) {
    std::string dir = tempFolder("missing");
    Pet pet(dir);
    pet.open(0, "NOPE");
    pet.talk(0);
    uint8_t b; bool eoi;
    ASSERT(!pet.receive(b, eoi));                   // silent: the PET's read times out
    pet.untalk();
    pet.talk(15);                                   // the error channel
    auto msg = pet.receiveAll();
    std::string s(msg.begin(), msg.end());
    ASSERT_EQ(s.substr(0, 18), std::string("62, FILE NOT FOUND"));   // as CBM DOS words it
    pet.untalk();
    fs::remove_all(dir);
}

// Another unit: after ATN nobody listens, so NRFD and NDAC both float high
// (the PET's "device not present")
TEST_CASE(ieee488_drive_other_unit_not_present) {
    std::string dir = tempFolder("other");
    Pet pet(dir);
    pet.command({0x29});                            // LISTEN 9
    ASSERT(!pet.ndacIn());                          // under ATN everyone handshakes
    pet.atn(false);
    ASSERT(pet.nrfdIn());
    ASSERT(pet.ndacIn());
    int t, s; bool led;
    ASSERT(pet.bus.getDiskStatus(8, t, s, led));
    ASSERT(!pet.bus.getDiskStatus(9, t, s, led));
    fs::remove_all(dir);
}

// ---------------------------------------------------------------------------
// Disk images
// ---------------------------------------------------------------------------

namespace {
// A D64 with "HELLO" (PRG, 1 block) at 1/0 and "NOTES" (SEQ) at 1/1
std::string makeD64(const char* tag) {
    static const int spt[35] = {21,21,21,21,21,21,21,21,21,21,21,21,21,21,21,21,21,
                                19,19,19,19,19,19,19,18,18,18,18,18,18,17,17,17,17,17};
    auto off = [](int t, int s) { size_t o = 0; for (int i = 1; i < t; ++i) o += spt[i - 1]; return (o + s) * 256; };
    std::vector<uint8_t> img(174848, 0);
    size_t h = off(18, 0);
    img[h] = 18; img[h + 1] = 1; img[h + 2] = 0x41;
    for (int i = 0; i < 27; ++i) img[h + 0x90 + i] = 0xA0;
    std::string name = "TEST DISK";
    for (size_t i = 0; i < name.size(); ++i) img[h + 0x90 + i] = (uint8_t)name[i];
    img[h + 0xA2] = 'A'; img[h + 0xA3] = 'B'; img[h + 0xA5] = '2'; img[h + 0xA6] = 'A';
    for (int t = 1; t <= 35; ++t) img[h + 4 + (t - 1) * 4] = (uint8_t)(t == 1 ? spt[0] - 2 : t == 18 ? spt[17] - 2 : spt[t - 1]);
    auto file = [&](int s, const std::string& data) {
        size_t o = off(1, s);
        img[o] = 0; img[o + 1] = (uint8_t)(data.size() + 1);
        for (size_t i = 0; i < data.size(); ++i) img[o + 2 + i] = (uint8_t)data[i];
    };
    file(0, std::string("\x01\x04\xEA\x60", 4));
    file(1, "LINE\r");
    size_t d = off(18, 1);
    img[d] = 0; img[d + 1] = 0xFF;
    auto entry = [&](int i, uint8_t type, int s, const std::string& n) {
        size_t e = d + i * 32;
        img[e + 2] = type; img[e + 3] = 1; img[e + 4] = (uint8_t)s;
        for (int k = 0; k < 16; ++k) img[e + 5 + k] = 0xA0;
        for (size_t k = 0; k < n.size(); ++k) img[e + 5 + k] = (uint8_t)n[k];
        img[e + 30] = 1;
    };
    entry(0, 0x82, 0, "HELLO");
    entry(1, 0x81, 1, "NOTES");
    fs::path p = fs::temp_directory_path() / (std::string("mmemu_ieee_") + tag + ".d64");
    std::ofstream(p, std::ios::binary).write((const char*)img.data(), (std::streamsize)img.size());
    return p.string();
}
}

TEST_CASE(ieee488_drive_loads_from_a_disk_image) {
    std::string img = makeD64("img");
    Pet pet(".");
    ASSERT(pet.bus.mountDisk(8, img));
    ASSERT_EQ(pet.bus.getMountedDiskPath(8), img);
    pet.open(0, "0:HEL*");                          // drive prefix and a wildcard
    pet.talk(0);
    auto bytes = pet.receiveAll();
    ASSERT_EQ((int)bytes.size(), 4);
    ASSERT_EQ((int)bytes[2], 0xEA);
    pet.untalk();
    pet.close(0);
    pet.open(2, "NOTES,S,R");                       // a SEQ file
    pet.talk(2);
    auto seq = pet.receiveAll();
    ASSERT_EQ(std::string(seq.begin(), seq.end()), std::string("LINE\r"));
    pet.untalk();
    pet.close(2);
    fs::remove(img);
}

TEST_CASE(ieee488_disk_image_directory_as_the_drive_sends_it) {
    std::string img = makeD64("dir");
    Pet pet(".");
    ASSERT(pet.bus.mountDisk(8, img));
    pet.open(0, "$");
    pet.talk(0);
    auto d = pet.receiveAll();
    std::string s(d.begin(), d.end());
    ASSERT_EQ((int)d[0], 0x01);                     // loaded at $0401
    ASSERT_EQ((int)d[1], 0x04);
    ASSERT(s.find("\x12\"TEST DISK       \" AB 2A") != std::string::npos);
    ASSERT(s.find("\"HELLO\"            PRG") != std::string::npos);
    ASSERT(s.find("\"NOTES\"            SEQ") != std::string::npos);
    ASSERT(s.find("BLOCKS FREE.") != std::string::npos);
    // The first line's link points at the second line ($0401 + 2 + link + line + text + 0)
    uint16_t link = (uint16_t)(d[2] | d[3] << 8);
    size_t firstLen = s.find('\0', 6) - 2;          // from the link to the line's 0, inclusive
    ASSERT_EQ((int)link, 0x0401 + (int)firstLen + 1);
    // Blocks free: 664 - 2 (track 18 left out, as CBM DOS counts)
    size_t bf = s.find("BLOCKS FREE.");
    ASSERT_EQ((int)(uint8_t)s[bf - 2] | (int)(uint8_t)s[bf - 1] << 8, 662);
    pet.untalk();
    fs::remove(img);
}

// An image file the host will not let us write is write protected
TEST_CASE(ieee488_disk_image_is_write_protected) {
    std::string img = makeD64("wp");
    fs::permissions(img, fs::perms::owner_read | fs::perms::group_read | fs::perms::others_read);
    Pet pet(".");
    ASSERT(pet.bus.mountDisk(8, img));
    pet.open(1, "NEW");                             // SAVE opens channel 1
    pet.talk(15);
    auto msg = pet.receiveAll();
    ASSERT_EQ(std::string(msg.begin(), msg.end()), std::string("26,WRITE PROTECT ON,00,00\r"));
    pet.untalk();
    pet.open(0, "NOPE");
    pet.talk(15);
    msg = pet.receiveAll();
    ASSERT_EQ(std::string(msg.begin(), msg.end()), std::string("62,FILE NOT FOUND,00,00\r"));
    pet.untalk();
    pet.talk(15);                                   // read once, then OK
    msg = pet.receiveAll();
    ASSERT_EQ(std::string(msg.begin(), msg.end()), std::string("00, OK,00,00\r"));
    pet.untalk();
    pet.bus.ejectDisk(8);                           // back to the host folder
    ASSERT(pet.bus.getMountedDiskPath(8) != img);
    fs::permissions(img, fs::perms::owner_read | fs::perms::owner_write);
    fs::remove(img);
}

namespace {
// makeD64's image with real BAM bitmaps (it sets only the counts): every
// sector free except 1/0, 1/1 (the two files) and 18/0, 18/1
void fillBam(const std::string& img) {
    static const int spt[35] = {21,21,21,21,21,21,21,21,21,21,21,21,21,21,21,21,21,
                                19,19,19,19,19,19,19,18,18,18,18,18,18,17,17,17,17,17};
    std::fstream f(img, std::ios::binary | std::ios::in | std::ios::out);
    size_t h = 0;
    for (int i = 1; i < 18; ++i) h += spt[i - 1];
    h *= 256;
    for (int t = 1; t <= 35; ++t) {
        uint32_t bits = (1u << spt[t - 1]) - 1;
        if (t == 1 || t == 18) bits &= ~3u;
        uint8_t b[3] = {(uint8_t)bits, (uint8_t)(bits >> 8), (uint8_t)(bits >> 16)};
        f.seekp((std::streamoff)(h + 4 + (t - 1) * 4 + 1));
        f.write((const char*)b, 3);
    }
}
std::string errorChannel(Pet& pet) {
    pet.talk(15);
    auto msg = pet.receiveAll();
    pet.untalk();
    return std::string(msg.begin(), msg.end());
}
void saveFile(Pet& pet, int sa, const std::string& name, const std::vector<uint8_t>& data) {
    pet.open(sa, name);
    pet.command({0x28, (uint8_t)(0x60 | sa)});
    pet.atn(false);
    for (size_t i = 0; i < data.size(); ++i) pet.send(data[i], i + 1 == data.size());
    pet.command({0x3F});
    pet.atn(false);
    pet.close(sa);
}
std::vector<uint8_t> loadFile(Pet& pet, const std::string& name) {
    pet.open(0, name);
    pet.talk(0);
    auto bytes = pet.receiveAll();
    pet.untalk();
    pet.close(0);
    return bytes;
}
}  // namespace

// SAVE and OPEN for writing on an image, as CBM DOS: blocks allocated from
// the BAM (first next to the directory track), a directory entry, the free
// count; @: replaces, ,A appends, S: scratches, R: renames
TEST_CASE(ieee488_disk_image_writes_files) {
    std::string img = makeD64("write");
    fillBam(img);
    Pet pet(".");
    ASSERT(pet.bus.mountDisk(8, img));
    std::vector<uint8_t> prog = {0x01, 0x04};
    for (int i = 0; i < 600; ++i) prog.push_back((uint8_t)i);   // 3 blocks
    saveFile(pet, 1, "BIG", prog);
    ASSERT_EQ(errorChannel(pet), std::string("00, OK,00,00\r"));
    ASSERT(loadFile(pet, "BIG") == prog);
    saveFile(pet, 1, "BIG", {1, 4, 9});
    ASSERT_EQ(errorChannel(pet), std::string("63,FILE EXISTS,00,00\r"));
    saveFile(pet, 1, "@0:BIG", {1, 4, 9});                     // replace
    ASSERT_EQ(errorChannel(pet), std::string("00, OK,00,00\r"));
    ASSERT(loadFile(pet, "BIG") == std::vector<uint8_t>({1, 4, 9}));
    saveFile(pet, 2, "LOG,S,W", {'A', 'B'});                    // a SEQ file, then append
    saveFile(pet, 2, "LOG,A", {'C'});
    ASSERT(loadFile(pet, "LOG") == std::vector<uint8_t>({'A', 'B', 'C'}));
    // The directory and the BAM: 664 - 2 (HELLO, NOTES) - 1 (BIG) - 1 (LOG)
    pet.open(0, "$");
    pet.talk(0);
    auto dir = pet.receiveAll();
    pet.untalk();
    pet.close(0);
    std::string text(dir.begin(), dir.end());
    ASSERT(text.find("\"BIG\"") != std::string::npos);
    ASSERT(text.find("SEQ") != std::string::npos);
    size_t free = text.find("BLOCKS FREE");
    ASSERT(free != std::string::npos);
    ASSERT_EQ((int)(uint8_t)dir[free - 2] | (int)(uint8_t)dir[free - 1] << 8, 660);   // the line number
    // Rename and scratch
    pet.open(15, "R0:NEW=BIG");
    ASSERT_EQ(errorChannel(pet), std::string("00, OK,00,00\r"));
    ASSERT(loadFile(pet, "NEW") == std::vector<uint8_t>({1, 4, 9}));
    pet.open(15, "S0:NEW,LO*");
    ASSERT_EQ(errorChannel(pet), std::string("01,FILES SCRATCHED,02,00\r"));
    pet.open(0, "NEW");
    ASSERT_EQ(errorChannel(pet), std::string("62,FILE NOT FOUND,00,00\r"));
    pet.bus.ejectDisk(8);
    fs::remove(img);
}
