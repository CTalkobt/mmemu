// The Sufami Turbo (as Mesen): the adapter's BIOS from roms/snes, slot A's cartridge and its RAM, slot B
// through the expansion slot, both RAMs saved. The BIOS here is hand-built (Bandai's is not shipped).

#include "test_harness.h"
#include "plugins/machines/snes/main/machine_snes.h"
#include "libcore/main/machine_desc.h"
#include "libmem/main/ibus.h"
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace {

const std::string DIR = "/tmp/mmemu_sufami_test";

void writeFile(const std::string& path, const std::vector<uint8_t>& d) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write((const char*)d.data(), (std::streamsize)d.size());
}
std::vector<uint8_t> readAll(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}
/** A Sufami Turbo cartridge of `size` bytes; `marker` names the game (and so its RAM). */
std::vector<uint8_t> stCart(size_t size, const char* marker, uint8_t seed) {
    std::vector<uint8_t> c(size);
    for (size_t i = 0; i < size; ++i) c[i] = (uint8_t)(i * 7 + (i >> 12) + seed);
    std::memcpy(c.data(), "BANDAI SFC-ADX  ", 16);
    std::memcpy(c.data() + 0x10, marker, std::strlen(marker));
    return c;
}

}  // namespace

TEST_CASE(snes_sufami_turbo) {
    mkdir(DIR.c_str(), 0755);
    const std::string a = DIR + "/a.st", b = DIR + "/b.st", aSave = DIR + "/a.srm", bSave = DIR + "/b.srm";
    std::remove(aSave.c_str());
    std::remove(bSave.c_str());
    const auto slotA = stCart(0x20000, "POIPOI.Ver", 0x11);   // 2 KB RAM
    const auto slotB = stCart(0x10000, "SDBATTLE ", 0x55);    // 2 KB RAM
    writeFile(b, slotB);

    std::string err;
    MachineDescriptor* m = snes::create(false, &err);
    ASSERT(m);
    // No BIOS: a message naming it
    setenv("MMEMU_SNES_FIRMWARE", DIR.c_str(), 1);
    std::remove((DIR + "/sufamiturbo.sfc").c_str());
    EXPECT_TRUE(!snes::insertCartridge(m, slotA, "a.st", err, aSave));
    EXPECT_TRUE(err.find("sufamiturbo.sfc") != std::string::npos);
    // Slot B with nothing in slot A
    EXPECT_TRUE(!snes::insertSufamiSlotB(m, b, err));

    // The BIOS: a LoROM header and a loop at $8000
    std::vector<uint8_t> bios(0x40000);
    for (size_t i = 0; i < bios.size(); ++i) bios[i] = (uint8_t)(i * 3 + (i >> 15));
    std::memcpy(bios.data(), "BANDAI SFC-ADX", 14);
    std::memcpy(bios.data() + 0x7FC0, "ADD-ON BASE CASSETE  ", 21);
    bios[0x7FD5] = 0x20; bios[0x7FD7] = 0x08; bios[0x7FD8] = 0; bios[0x7FD9] = 0;
    bios[0x7FFC] = 0x00; bios[0x7FFD] = 0x80;
    bios[0] = 0x80; bios[1] = 0xFE;
    writeFile(DIR + "/sufamiturbo.sfc", bios);
    // The BIOS loaded on its own is a plain cartridge
    ASSERT(snes::insertCartridge(m, bios, "bios.sfc", err));
    EXPECT_TRUE(snes::cartridgeInfo(m).find("Sufami Turbo:") == std::string::npos);

    ASSERT(snes::insertCartridge(m, slotA, "a.st", err, aSave));
    EXPECT_TRUE(snes::cartridgeInfo(m).find("Sufami Turbo: slot A 128K ROM, 2K RAM") != std::string::npos);
    IBus* bus = m->buses[0].bus;
    // The BIOS in $00-$1F (and $80-$9F); 4 KB pages wrap at its size
    EXPECT_EQ(bus->peek8(0x039234), bios[0x19234]);
    EXPECT_EQ(bus->peek8(0x839234), bios[0x19234]);
    EXPECT_EQ(bus->peek8(0x1F9234), bios[0x39234]);
    // Slot A's ROM in $20-$3F
    EXPECT_EQ(bus->peek8(0x218000), slotA[0x8000]);
    EXPECT_EQ(bus->peek8(0xA2C123), slotA[0x14123]);
    EXPECT_EQ(bus->peek8(0x3F8000), slotA[0x18000]);
    bus->write8(0x218000, (uint8_t)~slotA[0x8000]);
    EXPECT_EQ(bus->peek8(0x218000), slotA[0x8000]);
    // Its RAM in $60-$63, 2 KB mirrored
    bus->write8(0x608001, 0x5A);
    EXPECT_EQ(bus->peek8(0x608001), 0x5A);
    EXPECT_EQ(bus->peek8(0xE3F801), 0x5A);

    // Slot B through the expansion slot: its ROM at $40-$5F, its RAM at $70-$73
    auto* sl = m->expansionSlot("slotb");
    ASSERT(sl);
    ASSERT(sl->install("cart", b, err));
    EXPECT_EQ(snes::sufamiSlotB(m), b);
    EXPECT_EQ(sl->info(), "cart (" + b + ")");
    EXPECT_EQ(bus->peek8(0x408005), slotB[0x0005]);
    EXPECT_EQ(bus->peek8(0xC18005), slotB[0x8005]);   // 64 KB: bank $41's upper half is its second 32 KB
    EXPECT_EQ(bus->peek8(0x428005), slotB[0x0005]);   // and bank $42 wraps
    bus->write8(0x708002, 0xA5);
    EXPECT_EQ(bus->peek8(0xF38802), 0xA5);
    EXPECT_EQ(bus->peek8(0x608001), 0x5A);            // slot A's RAM kept (it powered on again)

    // Taking the cartridge out saves both RAMs
    snes::removeCartridge(m);
    const auto sa = readAll(aSave), sb = readAll(bSave);
    ASSERT(sa.size() == 0x800 && sb.size() == 0x800);
    EXPECT_EQ(sa[1], 0x5A);
    EXPECT_EQ(sb[2], 0xA5);
    // And they come back
    ASSERT(snes::insertCartridge(m, slotA, "a.st", err, aSave));
    ASSERT(snes::insertSufamiSlotB(m, b, err));
    EXPECT_EQ(bus->peek8(0x608001), 0x5A);
    EXPECT_EQ(bus->peek8(0x708002), 0xA5);
    // Slot B out
    ASSERT(sl->install("", "", err));
    EXPECT_EQ(snes::sufamiSlotB(m), std::string(""));
    // The same game in both slots: slot B saves to a_SlotB.srm
    std::remove((DIR + "/a_SlotB.srm").c_str());
    writeFile(a, slotA);
    ASSERT(snes::insertSufamiSlotB(m, a, err));
    bus->write8(0x708003, 0x3C);
    snes::removeCartridge(m);
    const auto sab = readAll(DIR + "/a_SlotB.srm");
    ASSERT(sab.size() == 0x800);
    EXPECT_EQ(sab[3], 0x3C);
    EXPECT_EQ(readAll(aSave)[1], 0x5A);
    delete m;
    unsetenv("MMEMU_SNES_FIRMWARE");
}
