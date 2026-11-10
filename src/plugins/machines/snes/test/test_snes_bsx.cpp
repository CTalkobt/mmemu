// The Satellaview (BS-X), as Mesen: the BS-X cartridge's registers and the map they lay out, the flash
// memory pack's commands, and the Satellaview's streams (the time channel and a channel's files). The
// BIOS here is hand-built (Nintendo's is not shipped).

#include "test_harness.h"
#include "plugins/machines/snes/main/machine_snes.h"
#include "libcore/main/machine_desc.h"
#include "libmem/main/ibus.h"
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace {

const std::string DIR = "/tmp/mmemu_bsx_test";

void writeFile(const std::string& path, const std::vector<uint8_t>& d) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write((const char*)d.data(), (std::streamsize)d.size());
}
/** A 1 MB LoROM BS-X BIOS: chipset $E5, 32 KB RAM, a loop at $8000. */
std::vector<uint8_t> bios() {
    std::vector<uint8_t> r(0x100000);
    for (size_t i = 0; i < r.size(); ++i) r[i] = (uint8_t)(i * 5 + (i >> 15));
    std::memcpy(r.data() + 0x7FC0, "Satellaview BS-X     ", 21);
    r[0x7FD5] = 0x20; r[0x7FD6] = 0xE5; r[0x7FD7] = 0x0A; r[0x7FD8] = 0x05; r[0x7FD9] = 0x00;
    r[0x7FFC] = 0x00; r[0x7FFD] = 0x80;
    r[0] = 0x80; r[1] = 0xFE;
    return r;
}
MachineDescriptor* setup() {
    mkdir(DIR.c_str(), 0755);
    mkdir((DIR + "/satellaview").c_str(), 0755);
    setenv("MMEMU_SNES_FIRMWARE", DIR.c_str(), 1);
    std::remove((DIR + "/bsx.srm").c_str());
    std::string err;
    return snes::create(false, &err);
}
void waitClocks(MachineDescriptor* m, uint64_t n) {
    const uint64_t to = snes::masterClock(m) + n;
    while (snes::masterClock(m) < to) m->buses[0].bus->read8(0x004300);
}

}  // namespace

// The registers at $00-$0F:5000 (bit 7; pending until $0E:5000 is written) and the map: the BIOS over
// $00-$3F/$80-$BF, the PSRAM, the BIOS's RAM at $10-$1F:5000; an earlier layout's pages stay
TEST_CASE(snes_bsx_map) {
    MachineDescriptor* m = setup();
    ASSERT(m);
    std::string err;
    const auto rom = bios();
    ASSERT(snes::insertCartridge(m, rom, "bsx.sfc", err));
    EXPECT_TRUE(snes::cartridgeInfo(m).find("BS-X, no pack") != std::string::npos);
    IBus* bus = m->buses[0].bus;
    // At reset all set but $04, $0A, $0C, $0D: HiROM, the BIOS over both halves, the PSRAM at $30 / $70
    EXPECT_EQ(bus->read8(0x005000) & 0x80, 0x80);
    EXPECT_EQ(bus->read8(0x045000) & 0x80, 0);
    EXPECT_EQ(bus->read8(0x0A5000) & 0x80, 0);
    EXPECT_EQ(bus->peek8(0x008002), rom[2]);
    EXPECT_EQ(bus->peek8(0x818000), rom[0x8000]);
    EXPECT_EQ(bus->peek8(0x3F9000), rom[0x1F9000 & 0xFFFFF]);
    // The PSRAM: $70-$77 whole banks, and $20-$3F:6000-7FFF (as Mesen: from its page 6)
    bus->write8(0x706000, 0x5C);
    EXPECT_EQ(bus->peek8(0x706000), 0x5C);
    EXPECT_EQ(bus->peek8(0x206000), 0x5C);
    // The BIOS's RAM at $10:5000 is the cartridge RAM at $78 (its LoROM map, still there)
    bus->write8(0x105000, 0x42);
    EXPECT_EQ(bus->peek8(0x780000), 0x42);
    EXPECT_EQ(bus->peek8(0x788000), 0x42);
    // A register change waits for $0E
    bus->write8(0x025000, 0x00);
    EXPECT_EQ(bus->read8(0x025000) & 0x80, 0x80);
    bus->write8(0x0E5000, 0x00);
    EXPECT_EQ(bus->read8(0x025000) & 0x80, 0x00);
    // LoROM now: the PSRAM at $60-$6F:0000-FFFF and $70-$7D:0000-7FFF
    EXPECT_EQ(bus->peek8(0x606000), 0x5C);
    EXPECT_EQ(bus->peek8(0x706000), 0x5C);
    EXPECT_EQ(bus->peek8(0x206000), 0x5C);    // HiROM's page there stays
    // The RESET button: the registers back
    m->onReset(*m);
    EXPECT_EQ(bus->read8(0x025000) & 0x80, 0x80);
    // The RAM is saved as bsx.srm beside the firmware (one for all packs, as Mesen)
    snes::removeCartridge(m);
    std::ifstream f(DIR + "/bsx.srm", std::ios::binary);
    std::vector<uint8_t> save((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    ASSERT(save.size() == 0x8000);
    EXPECT_EQ(save[0], 0x42);
    delete m;
    unsetenv("MMEMU_SNES_FIRMWARE");
}

// A .bs pack in the BIOS cartridge (roms/snes/bsx.bin): its HiROM map and the flash's commands
TEST_CASE(snes_bsx_pack) {
    MachineDescriptor* m = setup();
    ASSERT(m);
    std::string err;
    std::vector<uint8_t> pack(0x100000);
    for (size_t i = 0; i < pack.size(); ++i) pack[i] = (uint8_t)(i * 3 + (i >> 12));
    std::remove((DIR + "/bsx.bin").c_str());
    EXPECT_TRUE(!snes::insertCartridge(m, pack, "game.bs", err));
    EXPECT_TRUE(err.find("bsx.bin") != std::string::npos);
    writeFile(DIR + "/bsx.bin", bios());
    ASSERT(snes::insertCartridge(m, pack, "game.bs", err));
    EXPECT_TRUE(snes::cartridgeInfo(m).find("BS-X, 1024K pack") != std::string::npos);
    IBus* bus = m->buses[0].bus;
    EXPECT_EQ(bus->peek8(0x401234), pack[0x1234]);
    EXPECT_EQ(bus->peek8(0x458000), pack[0x58000]);
    EXPECT_EQ(bus->peek8(0xC41234), pack[0x41234]);
    // Vendor information: memory pack type 1, 1 MB
    bus->write8(0x400000, 0x75);
    EXPECT_EQ(bus->read8(0x407F00), 0x4D);
    EXPECT_EQ(bus->read8(0x407F06), 0x1A);
    bus->write8(0x400000, 0xFF);
    EXPECT_EQ(bus->read8(0x407F00), pack[0x7F00]);
    // A byte write only clears bits
    bus->write8(0x400000, 0x10);
    bus->write8(0x402000, 0x0F);
    EXPECT_EQ(bus->read8(0x402000), pack[0x2000] & 0x0F);
    bus->write8(0x402001, 0x00);                         // not a write now
    EXPECT_EQ(bus->read8(0x402001), pack[0x2001]);
    // The status register once, then data
    bus->write8(0x400000, 0x70);
    EXPECT_EQ(bus->read8(0x453333), 0x80);
    EXPECT_EQ(bus->read8(0x453333), pack[0x53333]);
    // Extended status
    bus->write8(0x400000, 0x71);
    EXPECT_EQ(bus->read8(0x400002), 0xC0);
    EXPECT_EQ(bus->read8(0x400004), 0x82);
    // Chip erase
    bus->write8(0x400000, 0xA7);
    bus->write8(0x400000, 0xD0);
    bus->write8(0x400000, 0xFF);
    EXPECT_EQ(bus->read8(0x458000), 0xFF);
    delete m;
    unsetenv("MMEMU_SNES_FIRMWARE");
}

// The Satellaview: the time channel's packet (a given date) and a channel played from BSX<ch>-<n>.bin
TEST_CASE(snes_bsx_satellaview) {
    MachineDescriptor* m = setup();
    ASSERT(m);
    setenv("TZ", "UTC", 1);
    tzset();
    std::string err;
    ASSERT(snes::insertCartridge(m, bios(), "bsx.sfc", err));
    ASSERT(snes::setSatellaviewDate(m, 1794314096));      // Tuesday 2026-11-10 12:34:56
    m->onReset(*m);
    IBus* bus = m->buses[0].bus;
    EXPECT_EQ(bus->read8(0x002196), 0x10);
    EXPECT_EQ(bus->read8(0x002198), 0x80);
    // Channel 0, both latches on
    bus->write8(0x002188, 0x00); bus->write8(0x002189, 0x00);
    bus->write8(0x00218B, 0x01); bus->write8(0x00218C, 0x01);
    EXPECT_EQ(bus->read8(0x00218A), 0);                   // asks for the packet
    waitClocks(m, 21477272 / 1000);                       // a packet a millisecond
    EXPECT_EQ(bus->read8(0x00218A), 1);
    EXPECT_EQ(bus->read8(0x00218B), 0x90);                // first and last
    uint8_t p[22];
    for (auto& b : p) b = bus->read8(0x00218C);
    EXPECT_EQ(p[4], 0x10); EXPECT_EQ(p[5], 0x01); EXPECT_EQ(p[6], 0x01);
    EXPECT_EQ(p[10], 56); EXPECT_EQ(p[11], 34); EXPECT_EQ(p[12], 12);
    EXPECT_EQ(p[13], 3);                                  // Tuesday, from 1
    EXPECT_EQ(p[14], 10); EXPECT_EQ(p[15], 11);
    EXPECT_EQ(p[16], 0xEA); EXPECT_EQ(p[17], 0x07);       // 2026
    EXPECT_EQ(bus->read8(0x00218D), 0x90);                // the status keeps the prefixes
    // Channel $0121 on stream 2: two packets from the file
    std::vector<uint8_t> file(30);
    for (size_t i = 0; i < file.size(); ++i) file[i] = (uint8_t)(0xA0 + i);
    writeFile(DIR + "/satellaview/BSX0121-0.bin", file);
    bus->write8(0x00218E, 0x21); bus->write8(0x00218F, 0x01);
    bus->write8(0x002191, 0x01); bus->write8(0x002192, 0x01);
    EXPECT_EQ(bus->read8(0x002190), 0);
    waitClocks(m, 2 * 21477272 / 1000 + 100);
    EXPECT_EQ(bus->read8(0x002190), 2);
    EXPECT_EQ(bus->read8(0x002191), 0x10);                // first
    for (int i = 0; i < 22; ++i) EXPECT_EQ(bus->read8(0x002192), file[i]);
    EXPECT_EQ(bus->read8(0x002191), 0x80);                // last
    delete m;
    unsetenv("TZ");
    tzset();
    unsetenv("MMEMU_SNES_FIRMWARE");
}

// An ordinary cartridge with a pack slot (game code Zxxx, maker $33): LoROM, the pack at $C0-$EF (both
// halves of a bank the same 32 KB, as Mesen); an erased 1 MB pack at first, saved as <rom>.bs
TEST_CASE(snes_bsx_pack_slot) {
    MachineDescriptor* m = setup();
    ASSERT(m);
    std::vector<uint8_t> r(0x80000);
    for (size_t i = 0; i < r.size(); ++i) r[i] = (uint8_t)(i * 7);
    std::memcpy(r.data() + 0x7FC0, "SOUND NOVEL TSUKURU  ", 21);
    std::memcpy(r.data() + 0x7FB2, "ZSNJ", 4);
    r[0x7FD5] = 0x20; r[0x7FD6] = 0x02; r[0x7FD7] = 0x09; r[0x7FD8] = 0x03; r[0x7FD9] = 0x00; r[0x7FDA] = 0x33;
    r[0x7FFC] = 0x00; r[0x7FFD] = 0x80;
    r[0] = 0x80; r[1] = 0xFE;
    const std::string save = DIR + "/tsukuru.srm", packFile = DIR + "/tsukuru.bs";
    std::remove(packFile.c_str());
    std::string err;
    ASSERT(snes::insertCartridge(m, r, "tsukuru.sfc", err, save));
    EXPECT_TRUE(snes::cartridgeInfo(m).find("BS-X pack slot") != std::string::npos);
    IBus* bus = m->buses[0].bus;
    EXPECT_EQ(bus->peek8(0xC01234), 0xFF);
    EXPECT_EQ(bus->peek8(0x008000), r[0]);                // the ROM as before
    bus->write8(0xC00000, 0x40);
    bus->write8(0xC11234, 0x5A);                          // the pack's $9234
    EXPECT_EQ(bus->peek8(0xC11234), 0x5A);
    EXPECT_EQ(bus->peek8(0xC19234), 0x5A);                // the bank's upper half: the same
    snes::removeCartridge(m);
    std::ifstream f(packFile, std::ios::binary);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    ASSERT(data.size() == 0x100000);
    EXPECT_EQ(data[0x9234], 0x5A);
    // And back
    ASSERT(snes::insertCartridge(m, r, "tsukuru.sfc", err, save));
    EXPECT_EQ(bus->peek8(0xC11234), 0x5A);
    delete m;
    unsetenv("MMEMU_SNES_FIRMWARE");
}
