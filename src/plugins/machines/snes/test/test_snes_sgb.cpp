// The Super Game Boy, as Mesen: a Game Boy cartridge loaded into the SNES runs in the SGB BIOS
// (roms/snes/SGB1.sfc) with a Game Boy clocked from the master clock; the ICD2's registers, the
// header packets the boot ROM sends, the LCD row buffers, the joypad, the sound. The BIOS here is a
// hand-built stand-in (Nintendo's is not shipped) and so is the Game Boy program.

#include "test_harness.h"
#include "plugins/machines/snes/main/machine_snes.h"
#include "plugins/machines/snes/main/snes_apu.h"
#include "plugins/machines/gameboy/main/gameboy.h"
#include "libcore/main/machine_desc.h"
#include "libdevices/main/iaudio_output.h"
#include "libdevices/main/io_registry.h"
#include "libmem/main/ibus.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace {

const std::string DIR = "/tmp/mmemu_sgb_test";

void writeFile(const std::string& path, const std::vector<uint8_t>& d) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write((const char*)d.data(), (std::streamsize)d.size());
}
std::vector<uint8_t> readAll(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

/** A stand-in SGB BIOS: LoROM, chipset $E3; it sets every player's buttons released and lets the Game Boy run. */
std::vector<uint8_t> bios(size_t size, const char* title) {
    std::vector<uint8_t> r(size, 0);
    char t[22];
    std::snprintf(t, sizeof t, "%-21s", title);
    std::memcpy(r.data() + 0x7FC0, t, 21);
    r[0x7FD5] = 0x20; r[0x7FD6] = 0xE3; r[0x7FD7] = 0x08; r[0x7FD8] = 0x00; r[0x7FD9] = 0x01;
    r[0x7FDC] = 0xFF; r[0x7FDD] = 0xFF; r[0x7FDE] = 0x00; r[0x7FDF] = 0x00;
    r[0x7FFC] = 0x00; r[0x7FFD] = 0x80;
    const uint8_t prog[] = {
        0x78,                   // SEI
        0xA9, 0xFF,             // LDA #$FF
        0x8D, 0x04, 0x60,       // STA $6004
        0xA9, 0x81,             // LDA #$81: run, divider 5
        0x8D, 0x03, 0x60,       // STA $6003
        0x80, 0xFE,             // BRA *
    };
    std::memcpy(r.data(), prog, sizeof prog);
    return r;
}

/** A Game Boy cartridge (32 KB, ROM only, SGB flag): a 512 Hz tone, a striped screen, the pad read into $C000. */
std::vector<uint8_t> gbCart() {
    std::vector<uint8_t> r(0x8000, 0);
    const uint8_t entry[] = {0x00, 0xC3, 0x50, 0x01};
    std::memcpy(&r[0x100], entry, 4);
    for (int i = 0; i < 0x30; ++i) r[0x104 + i] = (uint8_t)(0x11 * i + 3);        // (no logo check)
    std::memcpy(&r[0x134], "SGBTEST", 7);
    r[0x146] = 0x03;
    const uint8_t prog[] = {
        0x3E, 0x80, 0xE0, 0x26,           // NR52: sound on
        0x3E, 0x77, 0xE0, 0x24,           // NR50
        0x3E, 0xFF, 0xE0, 0x25,           // NR51
        0x3E, 0x80, 0xE0, 0x11,           // NR11: 50% duty
        0x3E, 0xF0, 0xE0, 0x12,           // NR12: volume 15
        0x3E, 0x00, 0xE0, 0x13,
        0x3E, 0x87, 0xE0, 0x14,           // NR14: trigger, 512 Hz
        0xAF, 0xE0, 0x40,                 // LCD off
        0x21, 0x00, 0x80, 0x06, 0x08,     // tile 0: rows $F0 $CC
        0x3E, 0xF0, 0x22, 0x3E, 0xCC, 0x22, 0x05, 0x20, 0xF7,
        0x21, 0x00, 0x98, 0x01, 0x00, 0x04,   // the map: all tile 0
        0xAF, 0x22, 0x0B, 0x78, 0xB1, 0x20, 0xF9,
        0x3E, 0xE4, 0xE0, 0x47,           // BGP
        0xAF, 0xE0, 0x42, 0xE0, 0x43,     // SCY, SCX
        0x3E, 0x91, 0xE0, 0x40,           // LCD on
        0x3E, 0x20, 0xE0, 0x00, 0xF0, 0x00, 0xEA, 0x00, 0xC0, 0x18, 0xF5,   // the pad into $C000, forever
    };
    std::memcpy(&r[0x150], prog, sizeof prog);
    return r;
}

void runFor(MachineDescriptor* m, double sec) {
    const uint64_t to = snes::masterClock(m) + (uint64_t)(21477272.0 * sec);
    while (snes::masterClock(m) < to) m->schedulerStep(*m);
}

IAudioOutput* audioOf(MachineDescriptor* m) {
    std::vector<IOHandler*> hs;
    m->ioRegistry->enumerate(hs);
    for (auto* h : hs) if (auto* a = dynamic_cast<IAudioOutput*>(h)) return a;
    return nullptr;
}

}  // namespace

TEST_CASE(snes_sgb) {
    mkdir(DIR.c_str(), 0755);
    setenv("MMEMU_SNES_FIRMWARE", DIR.c_str(), 1);
    std::remove((DIR + "/SGB1.sfc").c_str());
    std::remove((DIR + "/SGB2.sfc").c_str());
    for (const char* n : {"/sgb_boot.bin", "/sgb2_boot.bin", "/sgb_bios.bin"}) std::remove((DIR + n).c_str());
    const auto cart = gbCart();
    const std::string gbPath = DIR + "/test.gb";
    writeFile(gbPath, cart);

    std::string err;
    MachineDescriptor* m = snes::create(false, &err);
    ASSERT(m);
    // No BIOS: a message naming it
    EXPECT_TRUE(!snes::insertCartridge(m, cart, "test.gb", err));
    EXPECT_TRUE(err.find("SGB1.sfc") != std::string::npos);

    writeFile(DIR + "/SGB1.sfc", bios(0x40000, "Super GAMEBOY"));
    ASSERT(snes::insertCartridge(m, cart, "test.gb", err, "", gbPath));
    EXPECT_TRUE(snes::cartridgeInfo(m).find("Super Game Boy: SGBTEST, ROM, 32K ROM") != std::string::npos);
    EXPECT_EQ(snes::sgbCartridge(m), gbPath);
    gb::Gameboy* g = snes::sgbGameboy(m);
    ASSERT(g);
    IBus* bus = m->buses[0].bus;
    EXPECT_EQ(bus->peek8(0x00600F), 0x21);           // the ICD2's version
    EXPECT_EQ(bus->peek8(0x80600F), 0x21);           // and in the upper banks
    EXPECT_EQ(bus->peek8(0x008000), 0x78);           // the BIOS's code (LoROM)

    // The boot ROM's first packet: $F1, the checksum and the first 14 bytes of the header from $0104
    // (polled every instruction, as the BIOS does: the next packet follows within a couple of milliseconds)
    while (bus->peek8(0x006002) == 0 && snes::masterClock(m) < 21477272 / 4) m->schedulerStep(*m);
    ASSERT(bus->peek8(0x006002) == 1);
    uint8_t sum = 0;
    for (int i = 0; i < 14; ++i) sum = (uint8_t)(sum + cart[0x104 + i]);
    EXPECT_EQ(bus->peek8(0x007000), 0xF1);
    EXPECT_EQ(bus->peek8(0x007001), sum);
    for (int i = 0; i < 14; ++i) EXPECT_EQ(bus->peek8((uint32_t)(0x007002 + i)), cart[0x104 + i]);
    (void)bus->read8(0x007000);                       // read: no longer waiting
    EXPECT_EQ(bus->peek8(0x006002), 0);

    // The Game Boy runs at the master clock / 5
    runFor(m, 0.5);                                   // past the boot ROM
    EXPECT_EQ(g->cpu().state().pc >= 0x0150, true);
    const uint64_t c0 = g->cycles(), m0 = snes::masterClock(m);
    runFor(m, 0.1);
    const double ratio = (double)(snes::masterClock(m) - m0) / (double)(g->cycles() - c0);
    EXPECT_NEAR(ratio, 5.0, 0.001);

    // The LCD's rows: tile rows $F0 (plane 0) and $CC (plane 1), 20 tiles of 8 lines, then $FF
    runFor(m, 0.05);
    bus->write8(0x006001, 0x02);
    bool rowsOk = true;
    for (int i = 0; i < 320; ++i) {
        const uint8_t v = bus->read8(0x007800);
        if (v != ((i & 1) ? 0xCC : 0xF0)) rowsOk = false;
    }
    EXPECT_TRUE(rowsOk);
    EXPECT_EQ(bus->read8(0x007800), 0xFF);
    // $6000: the row (bits 3-7, 0-17 x 8) and the buffer being written
    const uint8_t row = bus->peek8(0x006000);
    EXPECT_TRUE((row >> 3) < 18 || (row >> 3) == 18);

    // The joypad: player 1's buttons as $FF00 reads them
    EXPECT_EQ(g->peek(0xC000), 0xEF);                 // the pad, nothing pressed
    bus->write8(0x006004, 0xFE);                      // right
    runFor(m, 0.01);
    EXPECT_EQ(g->peek(0xC000), 0xEE);
    bus->write8(0x006004, 0xF7);                      // down
    runFor(m, 0.01);
    EXPECT_EQ(g->peek(0xC000), 0xE7);

    // The sound: the Game Boy's 512 Hz square in the SNES's output, once the S-DSP isn't muted (as Mesen)
    auto* audio = audioOf(m);
    ASSERT(audio);
    std::vector<float> buf(8192);
    auto rms = [&](double sec) {
        while (audio->pullSamples(buf.data(), (int)buf.size()) > 0) {}
        runFor(m, sec);
        double s = 0;
        int n = 0, k;
        while ((k = audio->pullSamples(buf.data(), (int)buf.size())) > 0)
            for (int i = 0; i < k; ++i) { s += buf[i] * buf[i]; ++n; }
        return n ? std::sqrt(s / n) : 0.0;
    };
    EXPECT_TRUE(rms(0.05) < 0.001);                   // FLG's mute is set at power on
    auto* apu = dynamic_cast<snes::Apu*>(audio);
    ASSERT(apu);
    apu->dsp().write(0x6C, 0x20);                     // not muted
    const double loud = rms(0.1);
    std::printf("  Super Game Boy tone: RMS %.3f\n", loud);
    EXPECT_TRUE(loud > 0.02);

    // The divider: $6003 bits 0-1 = 3 is 9
    bus->write8(0x006003, 0x83);
    const uint64_t c1 = g->cycles(), m1 = snes::masterClock(m);
    runFor(m, 0.05);
    EXPECT_NEAR((double)(snes::masterClock(m) - m1) / (double)(g->cycles() - c1), 9.0, 0.01);
    // Bit 7 clear holds the Game Boy
    bus->write8(0x006003, 0x01);
    const uint64_t held = g->cycles();
    runFor(m, 0.02);
    EXPECT_TRUE(g->cycles() - held < 32);

    // The slot: taken out and put back through the expansion slot
    auto* sl = m->expansionSlot("gameboy");
    ASSERT(sl);
    ASSERT(sl->install("", "", err));
    EXPECT_TRUE(snes::cartridgeInfo(m).find("no Game Boy cartridge") != std::string::npos);
    ASSERT(sl->install("cart", gbPath, err));
    EXPECT_EQ(sl->info(), "cart (" + gbPath + ")");
    EXPECT_TRUE(snes::cartridgeInfo(m).find("SGBTEST") != std::string::npos);
    // The slot without the SGB: an error
    snes::removeCartridge(m);
    EXPECT_TRUE(!snes::insertSgbCartridge(m, gbPath, err));
    delete m;
    unsetenv("MMEMU_SNES_FIRMWARE");
}

TEST_CASE(snes_sgb_battery_and_sgb2) {
    mkdir(DIR.c_str(), 0755);
    setenv("MMEMU_SNES_FIRMWARE", DIR.c_str(), 1);
    std::remove((DIR + "/SGB1.sfc").c_str());
    // The SGB2: 512 KB, its own crystal (20.97 MHz / 5)
    writeFile(DIR + "/SGB2.sfc", bios(0x80000, "Super GAMEBOY2"));
    // An MBC1 cartridge with 8 KB of battery RAM; its program writes $5A to $A000 then loops
    auto cart = gbCart();
    cart[0x147] = 0x03; cart[0x149] = 0x02;
    const uint8_t prog[] = {0x3E, 0x0A, 0xEA, 0x00, 0x00, 0x3E, 0x5A, 0xEA, 0x00, 0xA0, 0x18, 0xFE};
    std::memcpy(&cart[0x150], prog, sizeof prog);
    const std::string gbPath = DIR + "/bat.gb", save = DIR + "/bat.srm";
    std::remove(save.c_str());
    writeFile(gbPath, cart);

    std::string err;
    MachineDescriptor* m = snes::create(false, &err);
    ASSERT(m);
    ASSERT(snes::insertCartridge(m, cart, "bat.gb", err, save, gbPath));
    EXPECT_TRUE(snes::cartridgeInfo(m).find("Super Game Boy 2: SGBTEST, MBC1, 32K ROM, 8K RAM, battery") != std::string::npos);
    gb::Gameboy* g = snes::sgbGameboy(m);
    ASSERT(g);
    runFor(m, 0.6);
    EXPECT_EQ(g->cartRam()[0], 0x5A);
    const uint64_t c0 = g->cycles(), m0 = snes::masterClock(m);
    runFor(m, 0.1);
    EXPECT_NEAR((double)(snes::masterClock(m) - m0) / (double)(g->cycles() - c0), 5.0 * 21477272.0 / 20971520.0, 0.001);
    snes::removeCartridge(m);
    const auto saved = readAll(save);
    ASSERT(saved.size() == 0x2000);
    EXPECT_EQ(saved[0], 0x5A);
    // And it comes back
    ASSERT(snes::insertCartridge(m, cart, "bat.gb", err, save, gbPath));
    EXPECT_EQ(snes::sgbGameboy(m)->cartRam()[0], 0x5A);
    delete m;
    std::remove((DIR + "/SGB2.sfc").c_str());
    unsetenv("MMEMU_SNES_FIRMWARE");
}

// The Game Boy's boot ROM from roms/snes/sgb_boot.bin (Mesen's name; sgb2_boot.bin for the SGB2, and
// sgb_bios.bin for either) in place of SameBoy's: a stand-in that marks $C001, turns itself off at $00FE
// and so falls into the cartridge at $0100.
TEST_CASE(snes_sgb_boot_rom_file) {
    mkdir(DIR.c_str(), 0755);
    setenv("MMEMU_SNES_FIRMWARE", DIR.c_str(), 1);
    for (const char* n : {"/SGB2.sfc", "/sgb_boot.bin", "/sgb2_boot.bin", "/sgb_bios.bin"}) std::remove((DIR + n).c_str());
    writeFile(DIR + "/SGB1.sfc", bios(0x40000, "Super GAMEBOY"));
    std::vector<uint8_t> boot(256, 0x00);
    const uint8_t prog[] = {0x31, 0xFE, 0xFF, 0x3E, 0x42, 0xEA, 0x01, 0xC0};   // LD SP,$FFFE; LD A,$42; LD ($C001),A
    std::memcpy(boot.data(), prog, sizeof prog);
    const uint8_t off[] = {0x3E, 0x01, 0xE0, 0x50};                          // LD A,1; LDH ($50),A
    std::memcpy(&boot[0xFC], off, sizeof off);
    const auto cart = gbCart();
    std::string err;

    // The wrong size is ignored: SameBoy's runs
    writeFile(DIR + "/sgb_boot.bin", std::vector<uint8_t>(255, 0));
    MachineDescriptor* m = snes::create(false, &err);
    ASSERT(m);
    ASSERT(snes::insertCartridge(m, cart, "test.gb", err));
    EXPECT_TRUE(snes::cartridgeInfo(m).find("boot ROM") == std::string::npos);
    delete m;

    // sgb_bios.bin, then sgb_boot.bin before it
    std::remove((DIR + "/sgb_boot.bin").c_str());
    writeFile(DIR + "/sgb_bios.bin", boot);
    m = snes::create(false, &err);
    ASSERT(snes::insertCartridge(m, cart, "test.gb", err));
    EXPECT_TRUE(snes::cartridgeInfo(m).find("Super Game Boy (boot ROM sgb_bios.bin): SGBTEST") != std::string::npos);
    delete m;
    writeFile(DIR + "/sgb_boot.bin", boot);
    m = snes::create(false, &err);
    ASSERT(snes::insertCartridge(m, cart, "test.gb", err));
    EXPECT_TRUE(snes::cartridgeInfo(m).find("Super Game Boy (boot ROM sgb_boot.bin): SGBTEST") != std::string::npos);
    gb::Gameboy* g = snes::sgbGameboy(m);
    ASSERT(g);
    runFor(m, 0.05);
    EXPECT_EQ(g->peek(0xC001), 0x42);                   // the file's boot ROM ran
    EXPECT_EQ(g->peek(0xC000), 0xEF);                   // and the cartridge after it (the pad, nothing pressed)
    EXPECT_EQ(g->peek(0x0000), cart[0]);                // the boot ROM is off
    delete m;

    for (const char* n : {"/sgb_boot.bin", "/sgb_bios.bin"}) std::remove((DIR + n).c_str());
    unsetenv("MMEMU_SNES_FIRMWARE");
}
