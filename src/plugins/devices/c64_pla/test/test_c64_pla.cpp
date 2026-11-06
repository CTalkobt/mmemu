#include "test_harness.h"
#include "plugins/devices/c64_pla/main/c64_pla.h"
#include "libdevices/main/simple_signal_line.h"
#include <vector>

namespace {
// A PLA on 64 KB of RAM with marked ROMs; `port` sets LORAM/HIRAM/CHAREN
// as the 6510's $01 bits 0-2 would
struct Rig {
    SimpleSignalLine loram, hiram, charen;
    std::vector<uint8_t> ram = std::vector<uint8_t>(0x10000, 0x11);
    std::vector<uint8_t> basic = std::vector<uint8_t>(0x2000, 0xBA), kernal = std::vector<uint8_t>(0x2000, 0xEE),
                         chars = std::vector<uint8_t>(0x1000, 0xC4), roml = std::vector<uint8_t>(0x2000, 0x81),
                         romh = std::vector<uint8_t>(0x2000, 0xA1);
    C64PLA pla;
    Rig() {
        pla.setSignals(&loram, &hiram, &charen);
        pla.setBasicRom(basic.data(), 0x2000);
        pla.setKernalRom(kernal.data(), 0x2000);
        pla.setCharRom(chars.data(), 0x1000);
        pla.setRamData(ram.data());
        port(7);
    }
    void port(int bits) { loram.set(bits & 1); hiram.set(bits & 2); charen.set(bits & 4); }
    void cart(bool exrom, bool game) { pla.setCartridgePort(roml.data(), 0x2000, romh.data(), 0x2000, exrom, game); }
    /** What a read sees: the PLA's byte, -1 for the I/O chips, -2 for the bus (RAM below $1000). */
    int read(uint16_t a) {
        uint8_t v = 0;
        if (pla.ioRead(nullptr, a, &v)) return v;
        return a >= 0xD000 && a < 0xE000 ? -1 : -2;
    }
};
}  // namespace

// No cartridge: the five usual $01 settings (VICE c64meminit.c)
TEST_CASE(c64_pla_port_configs) {
    Rig r;
    r.port(7);                                     // $37
    EXPECT_EQ(r.read(0xA000), 0xBA); EXPECT_EQ(r.read(0xD020), -1); EXPECT_EQ(r.read(0xE000), 0xEE);
    EXPECT_EQ(r.read(0x8000), 0x11);
    r.port(6);                                     // $36: BASIC out
    EXPECT_EQ(r.read(0xA000), 0x11); EXPECT_EQ(r.read(0xD020), -1); EXPECT_EQ(r.read(0xE000), 0xEE);
    r.port(5);                                     // $35: RAM but I/O
    EXPECT_EQ(r.read(0xA000), 0x11); EXPECT_EQ(r.read(0xD020), -1); EXPECT_EQ(r.read(0xE000), 0x11);
    r.port(3);                                     // $33: character ROM
    EXPECT_EQ(r.read(0xD000), 0xC4); EXPECT_EQ(r.read(0xA000), 0xBA);
    r.port(1);                                     // $31: character ROM, no KERNAL
    EXPECT_EQ(r.read(0xD000), 0xC4); EXPECT_EQ(r.read(0xE000), 0x11);
    r.port(4);                                     // $34: all RAM
    EXPECT_EQ(r.read(0xD020), 0x11); EXPECT_EQ(r.read(0xE000), 0x11);
    r.port(0);
    EXPECT_EQ(r.read(0xD020), 0x11);
}

// Writes to $D000-$DFFF reach the I/O chips only while I/O is mapped
TEST_CASE(c64_pla_d000_writes) {
    Rig r;
    r.port(7);
    ASSERT(!r.pla.ioWrite(nullptr, 0xD020, 0x05)); // to the VIC-II
    EXPECT_EQ(r.ram[0xD020], 0x11);
    r.port(4);
    ASSERT(r.pla.ioWrite(nullptr, 0xD020, 0x05));  // RAM, not the VIC-II
    EXPECT_EQ(r.ram[0xD020], 0x05);
    r.port(3);
    ASSERT(r.pla.ioWrite(nullptr, 0xD000, 0x42));  // RAM under the character ROM
    EXPECT_EQ(r.ram[0xD000], 0x42);
    ASSERT(!r.pla.ioWrite(nullptr, 0xA000, 0x42)); // under BASIC: the bus's RAM
}

// 8 KB game (EXROM low): ROML at $8000 with LORAM and HIRAM
TEST_CASE(c64_pla_8k_game) {
    Rig r;
    r.cart(true, false);
    r.port(7);
    EXPECT_EQ(r.pla.config(), 15);
    EXPECT_EQ(r.read(0x8000), 0x81); EXPECT_EQ(r.read(0xA000), 0xBA); EXPECT_EQ(r.read(0xE000), 0xEE);
    r.port(6);                                     // LORAM off: RAM at $8000
    EXPECT_EQ(r.read(0x8000), 0x11);
    r.port(5);
    EXPECT_EQ(r.read(0x8000), 0x11); EXPECT_EQ(r.read(0xD020), -1);
}

// 16 KB game (EXROM and GAME low): ROML and ROMH at $8000-$BFFF, no BASIC
TEST_CASE(c64_pla_16k_game) {
    Rig r;
    r.cart(true, true);
    r.port(7);
    EXPECT_EQ(r.pla.config(), 31);
    EXPECT_EQ(r.read(0x8000), 0x81); EXPECT_EQ(r.read(0xA000), 0xA1); EXPECT_EQ(r.read(0xE000), 0xEE);
    r.port(6);                                     // ROMH stays, ROML goes
    EXPECT_EQ(r.read(0x8000), 0x11); EXPECT_EQ(r.read(0xA000), 0xA1);
    r.port(5);                                     // no HIRAM: RAM, I/O
    EXPECT_EQ(r.read(0xA000), 0x11); EXPECT_EQ(r.read(0xD020), -1); EXPECT_EQ(r.read(0xE000), 0x11);
    r.port(1);                                     // VICE config 25: RAM at $D000 (no character ROM)
    EXPECT_EQ(r.read(0xD000), 0x11);
    r.port(3);                                     // config 27: character ROM
    EXPECT_EQ(r.read(0xD000), 0xC4);
}

// Ultimax (GAME low): ROML, I/O, ROMH at $E000; $1000-$7FFF and
// $A000-$CFFF open; writes there dropped; $01 does not matter
TEST_CASE(c64_pla_ultimax) {
    Rig r;
    r.cart(false, true);
    for (int p : {7, 4, 0}) {
        r.port(p);
        EXPECT_EQ(r.read(0x0800), -2);             // RAM through the bus
        EXPECT_EQ(r.read(0x1000), 0xFF);
        EXPECT_EQ(r.read(0x7FFF), 0xFF);
        EXPECT_EQ(r.read(0x8000), 0x81);
        EXPECT_EQ(r.read(0xA000), 0xFF);
        EXPECT_EQ(r.read(0xC000), 0xFF);
        EXPECT_EQ(r.read(0xD020), -1);
        EXPECT_EQ(r.read(0xE000), 0xA1);
    }
    ASSERT(r.pla.ioWrite(nullptr, 0x2000, 0x99));
    EXPECT_EQ(r.ram[0x2000], 0x11);
    ASSERT(!r.pla.ioWrite(nullptr, 0x0400, 0x99)); // $0000-$0FFF is RAM
    ASSERT(!r.pla.ioWrite(nullptr, 0xD020, 0x99)); // the VIC-II
    // Out again
    r.pla.setCartridgePort(nullptr, 0, nullptr, 0, false, false);
    r.port(7);
    EXPECT_EQ(r.read(0x1000), 0x11);
    EXPECT_EQ(r.read(0xA000), 0xBA);
}
