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
