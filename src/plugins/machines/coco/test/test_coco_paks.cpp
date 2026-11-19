// Bank-switched ROM Paks (GMC, MAME's banked Pak) and the Delta / Cumana
// disk cartridges, with hand-built stand-in images (no real ROMs).

#include "test_harness.h"
#include "coco_test_rom.h"
#include "plugins/machines/coco/main/coco_paks.h"
#include "plugins/devices/sn76489/main/sn76489.h"
#include "plugins/devices/wd17xx/main/wd17xx.h"
#include <cstdio>
#include <fstream>
#include <unistd.h>

using namespace cocotest;

namespace {

// `banks` banks of 16 KB, each byte its bank number, offset $0001 of bank b = $B0 + b
std::vector<uint8_t> bankedImage(int banks) {
    std::vector<uint8_t> v((size_t)banks * 0x4000);
    for (size_t i = 0; i < v.size(); ++i) v[i] = (uint8_t)(i / 0x4000);
    for (int b = 0; b < banks; ++b) v[(size_t)b * 0x4000 + 1] = (uint8_t)(0xB0 + b);
    return v;
}

std::string writeTemp(const char* stem, const std::vector<uint8_t>& data) {
    std::string t = std::string("/tmp/") + stem + "_XXXXXX";
    std::vector<char> buf(t.begin(), t.end());
    buf.push_back(0);
    const int fd = mkstemp(buf.data());
    if (fd >= 0) close(fd);
    std::ofstream f(buf.data(), std::ios::binary);
    f.write((const char*)data.data(), (std::streamsize)data.size());
    return buf.data();
}

uint8_t cts(coco::Cart& c, uint16_t a) {
    uint8_t d = 0;
    c.read(a, true, false, d);
    return d;
}

}  // namespace

// The GMC (XRoar's gmc.c): even $FF40-$FF5F writes select a 16 KB bank,
// masked to the image; odd ones go to the SN76489; reset is bank 0
TEST_CASE(coco_gmc_banks_and_sound) {
    coco::BankedPak gmc(bankedImage(4), "test", coco::BankedPak::Scheme::Gmc);
    ASSERT_EQ(gmc.kind(), std::string("gmc"));
    gmc.reset();
    ASSERT_EQ(cts(gmc, 0xC001), 0xB0);
    gmc.write(0xFF40, false, true, 2);
    ASSERT_EQ(cts(gmc, 0xC001), 0xB2);
    ASSERT_EQ(cts(gmc, 0xFEFF), 2);
    gmc.write(0xFF5E, false, true, 3);              // any even address
    ASSERT_EQ(cts(gmc, 0xC001), 0xB3);
    gmc.write(0xFF40, false, true, 5);              // 5 & 3 (64 KB): bank 1
    ASSERT_EQ(cts(gmc, 0xC001), 0xB1);
    gmc.write(0xFF40, false, false, 0);             // not /SCS: ignored
    ASSERT_EQ(cts(gmc, 0xC001), 0xB1);
    // The SN76489: tone 0 at full volume
    gmc.write(0xFF41, false, true, 0x8E);           // tone 0 low bits
    gmc.write(0xFF41, false, true, 0x01);           // high bits: N = $1E
    gmc.write(0xFF41, false, true, 0x90);           // attenuation 0
    ASSERT_EQ(gmc.psg()->tone(0), 0x1E);
    ASSERT_EQ(gmc.psg()->attenuation(0), 0);
    float lo = 1.0f, hi = -1.0f;
    for (int i = 0; i < 2000; ++i) {
        gmc.clock(160);
        lo = std::min(lo, gmc.audio());
        hi = std::max(hi, gmc.audio());
    }
    ASSERT(hi - lo > 0.1f);
    gmc.reset();
    ASSERT_EQ(cts(gmc, 0xC001), 0xB0);
    ASSERT_EQ(gmc.psg()->attenuation(0), 15);
}

// MAME's banked Pak: only $FF40, bank D modulo the image; an odd image pads with $FF
TEST_CASE(coco_banked_pak_tandy_scheme) {
    std::vector<uint8_t> img = bankedImage(3);       // 48 KB: padded to 64 KB
    coco::BankedPak pak(img, "three", coco::BankedPak::Scheme::Tandy);
    pak.reset();
    ASSERT_EQ(pak.size(), (size_t)0x10000);
    pak.write(0xFF40, false, true, 2);
    ASSERT_EQ(cts(pak, 0xC001), 0xB2);
    pak.write(0xFF42, false, true, 1);               // not decoded
    ASSERT_EQ(cts(pak, 0xC001), 0xB2);
    pak.write(0xFF40, false, true, 3);               // the padding
    ASSERT_EQ(cts(pak, 0xC001), 0xFF);
    pak.write(0xFF40, false, true, 5);               // 5 x 16K mod 64K: bank 1
    ASSERT_EQ(cts(pak, 0xC001), 0xB1);
}

// Auto-detection (XRoar's heuristic): over 16 KB is banked; a CoCo inserting one
// gets a GMC that switches banks and still autostarts
TEST_CASE(coco_rom_cart_auto_detect) {
    ASSERT_EQ(coco::romCartType(std::vector<uint8_t>(0x2000, 1)), std::string("rompak"));
    ASSERT_EQ(coco::romCartType(std::vector<uint8_t>(0x4000, 1)), std::string("rompak"));
    ASSERT_EQ(coco::romCartType(std::vector<uint8_t>(0x8000, 1)), std::string("gmc"));
    ASSERT_EQ(coco::crc32((const uint8_t*)"123456789", 9), 0xCBF43926u);
    std::string err;
    ASSERT(!coco::makeRomCart(std::vector<uint8_t>(16, 0), "x", "nope", err));

    MachineDescriptor* m = make();
    ASSERT(m);
    std::vector<uint8_t> img = bankedImage(8);
    std::vector<uint8_t> boot = romPak(0x77, 0x4000);
    std::copy(boot.begin(), boot.begin() + 16, img.begin());   // bank 0's start runs
    ASSERT(coco::insertRomPak(m, img, "big.ccc", err));
    coco::System* sys = coco::systemOf(m);
    ASSERT_EQ(sys->cart()->kind(), std::string("gmc"));
    m->onReset(*m);
    run(m, 300000);
    IBus* bus = m->buses[0].bus;
    ASSERT_EQ(bus->peek8(CART_MARK), 0x77);
    bus->write8(0xFF40, 6);
    ASSERT_EQ(bus->peek8(0xC001), 0xB6);
    ASSERT(coco::cartridgeInfo(m).find("Games Master") != std::string::npos);
    coco::removeRomPak(m);
    ASSERT(!sys->cart());
    // Explicitly, through the cartridge slot
    const std::string path = writeTemp("banked", bankedImage(2));
    ASSERT(m->expansionSlot("cart")->install("banked", path, err));
    ASSERT_EQ(sys->cart()->kind(), std::string("banked"));
    ASSERT(m->expansionSlot("cart")->install("gmc", path, err));
    ASSERT_EQ(sys->cart()->kind(), std::string("gmc"));
    ASSERT(!m->expansionSlot("cart")->install("deltados", path, err));     // a Dragon's
    std::remove(path.c_str());
    delete m;
}

// DeltaDOS (XRoar deltados.c, MAME premier_fdc): a WD2791 (inverted bus) at
// $FF40-$FF43, the latch where A2 is set: drive, side, 8", density
TEST_CASE(dragon_deltados_controller) {
    std::vector<uint8_t> rom(0x2000, 0x12);
    rom[0] = 0xDE;
    coco::DeltaDos dos(rom);
    bool nmi = false, cartLow = false;
    dos.setNmi = [&](bool on) { nmi = nmi || on; };
    dos.setCart = [&](bool level) { cartLow = cartLow || !level; };
    dos.reset();
    ASSERT_EQ(cts(dos, 0xC000), 0xDE);
    ASSERT_EQ(cts(dos, 0xE000), 0xDE);
    ASSERT(dos.controller().variant() == Wd17xx::Variant::WD2791);
    dos.write(0xFF42, false, true, 0xF6);              // sector 9, complemented
    ASSERT_EQ(dos.controller().sectorRegister(), 0x09);
    uint8_t d = 0;
    ASSERT(dos.read(0xFF42, false, true, d));
    ASSERT_EQ(d, 0xF6);
    ASSERT(!dos.read(0xFF44, false, true, d));         // the latch is write only
    dos.write(0xFF44, false, true, 0x16);              // drive 2, side 1, double density
    ASSERT_EQ(dos.latch(), 0x16);
    ASSERT_EQ(dos.controller().selectedDrive(), 2);
    ASSERT(dos.controller().doubleDensity());
    ASSERT(dos.controller().motor());
    dos.write(0xFF44, false, true, 0x01);
    ASSERT(!dos.controller().doubleDensity());
    // A Restore runs with no DRQ / INTRQ wiring to the CPU
    dos.write(0xFF40, false, true, (uint8_t)~0x00);
    for (int i = 0; i < 20000; ++i) dos.clock(1000);
    ASSERT(dos.controller().intrq());
    ASSERT(!nmi);
    ASSERT(!cartLow);
}
