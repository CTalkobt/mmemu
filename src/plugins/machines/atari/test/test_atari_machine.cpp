#include "test_harness.h"
#include "plugins/machines/atari/main/machine_atari.h"
#include "libcore/main/machine_desc.h"
#include "libdevices/main/io_registry.h"
#include "libdevices/main/ikeyboard_matrix.h"
#include "libdevices/main/ivideo_output.h"
#include "plugins/devices/gtia/main/gtia.h"
#include "plugins/devices/pokey/main/pokey.h"
#include "plugins/machines/atari/main/atari_cart.h"
#include "plugins/machines/atari/main/atari_sio.h"
#include <filesystem>
#include <fstream>
#include <memory>
#include <unistd.h>
#include <string>
#include <vector>

// Atari 8-bit machines (#10): boot to BASIC, typing and BASIC's BCD
// arithmetic, XL banking, the rendered screen.

namespace {

using Machine = std::unique_ptr<MachineDescriptor>;

IBus* bus(MachineDescriptor& m) { return m.buses[0].bus; }

/** The 40-column screen as text (ANTIC internal codes to ASCII). */
std::string screenText(MachineDescriptor& m, int rows = 24) {
    IBus* b = bus(m);
    uint16_t sav = (uint16_t)(b->peek8(0x58) | (b->peek8(0x59) << 8));
    std::string s;
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < 40; ++c) {
            uint8_t v = b->peek8((uint16_t)(sav + r * 40 + c)) & 0x7F;
            s += v < 0x40 ? (char)(v + 0x20) : v >= 0x60 ? (char)v : '.';
        }
        s += '\n';
    }
    return s;
}

void runCycles(MachineDescriptor& m, uint64_t cycles) {
    for (uint64_t c = 0; c < cycles; ) c += (uint64_t)m.schedulerStep(m);
}

/** Run until `text` is on the screen (or a time limit in emulated seconds). */
bool runUntil(MachineDescriptor& m, const std::string& text, double seconds) {
    for (double t = 0; t < seconds; t += 0.1) {
        runCycles(m, atari::CLOCK_HZ / 10);
        if (screenText(m).find(text) != std::string::npos) return true;
    }
    return false;
}

IKeyboardMatrix* keyboard(MachineDescriptor& m) {
    std::vector<IOHandler*> hs;
    m.ioRegistry->enumerate(hs);
    for (auto* h : hs) if (auto* k = dynamic_cast<IKeyboardMatrix*>(h)) return k;
    return nullptr;
}

}  // namespace

TEST_CASE(atari800xl_boots_to_basic_and_computes_in_bcd) {
    std::string err;
    Machine m(atari::create(atari::Model::Atari800XL, &err));
    ASSERT(m != nullptr);
    ASSERT(runUntil(*m, "READY", 8.0));                  // ~4.3 s: the disk boot times out
    ASSERT_EQ((int)bus(*m)->peek8(0x2E4), 0xA0);         // RAMTOP: 40 KB below BASIC

    auto* kbd = keyboard(*m);
    ASSERT(kbd != nullptr);
    kbd->enqueueText("PRINT 2+3*4;\"/\";7*6;\"/\";100-1;\"/\";1/4\n");
    ASSERT(runUntil(*m, "14/42/99/0.25", 5.0));          // was ">": no decimal mode in the 6502

    // The screen: BASIC's blue (COLPF2 $94) behind the text
    std::vector<uint32_t> frame(384 * 240);
    IVideoOutput* video = nullptr;
    std::vector<IOHandler*> hs;
    m->ioRegistry->enumerate(hs);
    for (auto* h : hs) if ((video = dynamic_cast<IVideoOutput*>(h))) break;
    ASSERT(video != nullptr);
    ASSERT_EQ(video->getDimensions().width, 384);
    video->renderFrame(frame.data());
    ASSERT_EQ(frame[200 * 384 + 192], GTIA::getARGB(0x94));
    ASSERT_EQ(frame[200 * 384 + 4], GTIA::getARGB(0x00));   // border
}

TEST_CASE(atari800xl_portb_banking) {
    Machine m(atari::create(atari::Model::Atari800XL));
    ASSERT(m != nullptr);
    IBus* b = bus(*m);
    uint8_t osByte = b->peek8(0xC002);
    // Set up port B as outputs (PBCTL $D303 bit 2 off: DDRB; on: data)
    b->write8(0xD303, 0x00); b->write8(0xD301, 0xFF); b->write8(0xD303, 0x04);
    b->write8(0xD301, 0xFD);                              // BASIC on
    uint8_t basicByte = b->peek8(0xA000);
    ASSERT_EQ((int)basicByte, 0xA5);                      // Atari BASIC rev C starts LDA $CA
    b->write8(0xD301, 0xFF);                              // OS on, BASIC off, self test off
    b->write8(0xA000, (uint8_t)~basicByte);
    ASSERT_EQ((int)b->peek8(0xA000), (int)(uint8_t)~basicByte);   // RAM now
    b->write8(0xD301, 0xFD);                              // BASIC on
    ASSERT_EQ((int)b->peek8(0xA000), (int)basicByte);
    b->write8(0xD301, 0xFE);                              // OS off: RAM at $C000
    b->write8(0xC002, (uint8_t)~osByte);
    ASSERT_EQ((int)b->peek8(0xC002), (int)(uint8_t)~osByte);
    b->write8(0xD301, 0x7F);                              // OS on + self test at $5000
    ASSERT_EQ((int)b->peek8(0xC002), (int)osByte);
    uint8_t st = b->peek8(0x5001);
    b->write8(0xD301, 0xFF);
    b->write8(0x5001, (uint8_t)~st);
    ASSERT_EQ((int)b->peek8(0x5001), (int)(uint8_t)~st);  // RAM again without self test
}

TEST_CASE(atari800_boots_to_basic) {
    Machine m(atari::create(atari::Model::Atari800));
    ASSERT(m != nullptr);
    ASSERT(runUntil(*m, "READY", 8.0));
    ASSERT_EQ((int)bus(*m)->peek8(0x2E4), 0xA0);         // 48 KB, BASIC cartridge at $A000
    ASSERT_EQ((int)bus(*m)->peek8(0xC000), 0xFF);        // nothing at $C000 on an 800
}

TEST_CASE(atari_keys_by_name) {
    Machine m(atari::create(atari::Model::Atari800XL));
    ASSERT(m != nullptr);
    ASSERT(runUntil(*m, "READY", 8.0));
    for (const char* k : {"l", "i", "s", "t", "return"}) {
        ASSERT(m->onKey(k, true));
        runCycles(*m, atari::CLOCK_HZ / 15);
        ASSERT(m->onKey(k, false));
        runCycles(*m, atari::CLOCK_HZ / 15);
    }
    ASSERT(!m->onKey("no_such_key", true));
    ASSERT(runUntil(*m, "LIST", 1.0));
    ASSERT(m->onKey("f4", true));                         // START (console key, GTIA)
    uint8_t consol = bus(*m)->peek8(0xD01F);
    ASSERT_EQ((int)(consol & GTIA::CONSOL_START), 0);
    m->onKey("f4", false);
}

// SIO and .xex loading: the generated boot disk is read over the serial bus
// by the OS (its own disk boot) and the 6502 loader on it; INITAD and RUNAD.
TEST_CASE(atari_boots_xex_over_sio) {
    std::vector<uint8_t> xex = {
        0xFF, 0xFF,
        0x00, 0x20, 0x0A, 0x20,                           // $2000-$200A: main
        0xA9, 0x34, 0x8D, 0xC6, 0x02,                     //   LDA #$34  STA COLOR2 ($2C6)
        0xEE, 0x81, 0x06,                                 //   INC $0681
        0x4C, 0x08, 0x20,                                 //   JMP *
        0x00, 0x21, 0x05, 0x21,                           // $2100-$2105: init
        0xA9, 0x55, 0x8D, 0x80, 0x06, 0x60,               //   LDA #$55  STA $0680  RTS
        0xE2, 0x02, 0xE3, 0x02, 0x00, 0x21,               // INITAD = $2100
        0xFF, 0xFF,                                       // (a repeated header is allowed)
        0xE0, 0x02, 0xE1, 0x02, 0x00, 0x20,               // RUNAD = $2000
    };
    for (auto model : {atari::Model::Atari800XL, atari::Model::Atari800}) {
        Machine m(atari::create(model));
        ASSERT(m != nullptr);
        std::string err;
        ASSERT(atari::bootExecutable(m.get(), xex, "test.xex", err));
        IBus* b = bus(*m);
        bool ran = false;
        for (int i = 0; i < 100 && !ran; ++i) {           // up to 10 s
            runCycles(*m, atari::CLOCK_HZ / 10);
            ran = b->peek8(0x0681) == 1;
        }
        ASSERT(ran);
        ASSERT_EQ((int)b->peek8(0x0680), 0x55);           // INIT ran first
        ASSERT_EQ((int)b->peek8(0x02C6), 0x34);
    }

    std::vector<uint8_t> atr;
    std::string err;
    ASSERT(!atari::bootExecutable(nullptr, {0x12, 0x34, 0, 0, 0, 0}, "x", err));
    ASSERT(err.find("Not an Atari executable") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Cartridges, saved disk writes, PAL
// ---------------------------------------------------------------------------
namespace {
/** A .car image: header and `kb` KB, each 8 KB bank filled with its number. */
std::vector<uint8_t> carImage(int type, int kb) {
    std::vector<uint8_t> v = {'C', 'A', 'R', 'T', 0, 0, 0, (uint8_t)type, 0, 0, 0, 0, 0, 0, 0, 0};
    for (int i = 0; i < kb * 1024; ++i) v.push_back((uint8_t)(i / 0x2000));
    return v;
}

/** Send a byte string down the SIO bus as POKEY would. */
void sioSend(atari::AtariSio& sio, const std::vector<uint8_t>& bytes) {
    for (uint8_t b : bytes) sio.serialOut(b);
}
uint8_t sioSum(const std::vector<uint8_t>& d) {
    unsigned s = 0;
    for (uint8_t b : d) { s += b; if (s > 0xFF) s = (s & 0xFF) + 1; }
    return (uint8_t)s;
}
}  // namespace

TEST_CASE(atari_cartridge_banking) {
    std::string err;
    atari::Cartridge raw;
    std::vector<uint8_t> rom8(8192);
    for (int i = 0; i < 8192; ++i) rom8[i] = (uint8_t)i;
    ASSERT(raw.load(rom8, err));
    ASSERT_EQ(raw.type(), (int)atari::Cartridge::STD_8);
    ASSERT(raw.rd5() && !raw.rd4());
    ASSERT_EQ((int)raw.read(0xA005), 5);

    atari::Cartridge xegs;                         // $8000 switched by data, $A000 the last bank
    ASSERT(xegs.load(carImage(atari::Cartridge::XEGS_32, 32), err));
    ASSERT(xegs.rd4() && xegs.rd5());
    ASSERT_EQ((int)xegs.read(0x8000), 0);
    ASSERT_EQ((int)xegs.read(0xA000), 3);
    xegs.writeCctl(0xD500, 2);
    ASSERT_EQ((int)xegs.read(0x9FFF), 2);

    atari::Cartridge sw;
    ASSERT(sw.load(carImage(atari::Cartridge::SWXEGS_32, 32), err));
    sw.writeCctl(0xD500, 0x80);                    // switched off
    ASSERT(!sw.rd4() && !sw.rd5());

    atari::Cartridge will;                         // bank by the address read
    ASSERT(will.load(carImage(atari::Cartridge::WILL_64, 64), err));
    ASSERT(!will.rd4());
    will.readCctl(0xD503);
    ASSERT_EQ((int)will.read(0xA000), 3);
    will.readCctl(0xD508);
    ASSERT(!will.rd5());

    atari::Cartridge sdx;                          // $D5E0-$D5E7: bank 7 down to 0
    ASSERT(sdx.load(carImage(atari::Cartridge::SDX_64, 64), err));
    sdx.readCctl(0xD5E0);
    ASSERT_EQ((int)sdx.read(0xA000), 7);
    sdx.writeCctl(0xD5E5, 0);
    ASSERT_EQ((int)sdx.read(0xA000), 2);
    sdx.readCctl(0xD500);                          // not its page: no change
    ASSERT_EQ((int)sdx.read(0xA000), 2);
    sdx.readCctl(0xD5E8);
    ASSERT(!sdx.rd5());

    atari::Cartridge bad;
    ASSERT(!bad.load(carImage(99, 8), err));
    ASSERT(err.find("Unsupported cartridge type 99") != std::string::npos);
    ASSERT(!bad.load(std::vector<uint8_t>(5000), err));
}

TEST_CASE(atari_cartridge_boots_and_trig3) {
    // An 8 KB cartridge: stores $42 at $0600 and TRIG3 at $0601, then loops
    std::vector<uint8_t> rom(8192, 0xFF);
    const uint8_t code[] = {
        0xA9, 0x42, 0x8D, 0x00, 0x06,     // LDA #$42  STA $0600
        0xAD, 0x13, 0xD0, 0x8D, 0x01, 0x06, // LDA TRIG3 STA $0601
        0x4C, 0x0B, 0xA0 };               // JMP *
    std::copy(std::begin(code), std::end(code), rom.begin());
    rom[0x10] = 0x60;                     // $A010: init, RTS
    rom[0x1FFA] = 0x00; rom[0x1FFB] = 0xA0;   // start $A000
    rom[0x1FFC] = 0x00;                   // a cartridge is present
    rom[0x1FFD] = 0x04;                   // start it (no disk boot)
    rom[0x1FFE] = 0x10; rom[0x1FFF] = 0xA0;   // init $A010

    for (auto model : {atari::Model::Atari800XL, atari::Model::Atari800}) {
        Machine m(atari::create(model));
        ASSERT(m != nullptr);
        std::string err;
        ASSERT(atari::insertCartridge(m.get(), rom, "test.rom", err));
        ASSERT(atari::cartridgeInfo(m.get()).find("Standard 8 KB") != std::string::npos);
        IBus* b = bus(*m);
        bool ran = false;
        for (int i = 0; i < 30 && !ran; ++i) {
            runCycles(*m, atari::CLOCK_HZ / 10);
            ran = b->peek8(0x0600) == 0x42;
        }
        ASSERT(ran);
        if (model == atari::Model::Atari800XL) ASSERT_EQ((int)b->peek8(0x0601), 1);   // RD5: present

        atari::removeCartridge(m.get());
        ASSERT(atari::cartridgeInfo(m.get()).empty());
        ASSERT(runUntil(*m, "READY", 8.0));   // BASIC again
    }
}

TEST_CASE(atari_sio_saves_disk_writes) {
    namespace fs = std::filesystem;
    fs::path p = fs::temp_directory_path() / "mmemu_test_atari_write.atr";
    {
        std::vector<uint8_t> atr(16 + 720 * 128, 0);
        atr[0] = 0x96; atr[1] = 0x02; atr[2] = (uint8_t)(720 * 8); atr[3] = (uint8_t)((720 * 8) >> 8); atr[4] = 128;
        std::ofstream f(p, std::ios::binary);
        f.write(reinterpret_cast<const char*>(atr.data()), (std::streamsize)atr.size());
    }
    auto writeSector5 = [](atari::AtariSio& sio, uint8_t fill) {
        std::vector<uint8_t> cmd = {0x31, 'W', 5, 0};
        cmd.push_back(sioSum(cmd));
        sio.commandLine()->set(false);
        sioSend(sio, cmd);
        sio.commandLine()->set(true);
        sio.tick(3000);
        std::vector<uint8_t> data(128, fill);
        data.push_back(sioSum(std::vector<uint8_t>(128, fill)));
        sioSend(sio, data);
        sio.tick(10000);
    };
    auto fileSector5 = [&]() {
        std::ifstream f(p, std::ios::binary);
        f.seekg(16 + 4 * 128);
        std::vector<uint8_t> v(128);
        f.read(reinterpret_cast<char*>(v.data()), 128);
        return v;
    };

    POKEY pokey;
    atari::AtariSio sio(&pokey);
    ASSERT(sio.mountDisk(1, p.string()));
    ASSERT(!sio.writeProtected(0));
    ASSERT_EQ(sio.getMountedDiskPath(1), p.string());
    writeSector5(sio, 0x5A);
    ASSERT(fileSector5() == std::vector<uint8_t>(128, 0x5A));     // written through
    ASSERT_EQ((int)sio.image(0)[16 + 4 * 128], 0x5A);
    int t, sec; bool led;
    ASSERT(sio.getDiskStatus(1, t, sec, led));
    ASSERT_EQ(sec, 5);
    ASSERT(!sio.getDiskStatus(8, t, sec, led));

    if (getuid() != 0) {                                          // root ignores permissions
        fs::permissions(p, fs::perms::owner_read | fs::perms::group_read | fs::perms::others_read);
        ASSERT(sio.mountDisk(1, p.string()));
        ASSERT(sio.writeProtected(0));
        writeSector5(sio, 0x11);
        ASSERT(fileSector5() == std::vector<uint8_t>(128, 0x5A));
        ASSERT_EQ((int)sio.image(0)[16 + 4 * 128], 0x5A);
        fs::permissions(p, fs::perms::owner_write, fs::perm_options::add);
    }
    sio.ejectDisk(1);
    ASSERT(!sio.mounted(0));
    fs::remove(p);
}

TEST_CASE(atari800xl_pal_boots) {
    std::string err;
    Machine m(atari::create(atari::Model::Atari800XL, &err, true));
    ASSERT(m != nullptr);
    ASSERT_EQ(m->machineId, std::string("atari800xl-pal"));
    ASSERT_EQ((unsigned)m->clockHz, atari::CLOCK_HZ_PAL);
    ASSERT(runUntil(*m, "READY", 8.0));
    IBus* b = bus(*m);
    ASSERT_EQ((int)(b->peek8(0xD014) & 0x0E), 0);                  // GTIA: PAL
    ASSERT_EQ((int)b->peek8(0x0062), 1);                            // PALNTS: the OS saw PAL (NTSC: 0)
    int maxV = 0;
    for (int i = 0; i < 400; ++i) { runCycles(*m, 114); maxV = std::max(maxV, (int)b->peek8(0xD40B)); }
    ASSERT_EQ(maxV, 155);                                           // 312 lines
}

// The Atari's hardware panes: no VIC graphics viewer, tape or SID scope
TEST_CASE(atari_ui_panes) {
    Machine m(atari::create(atari::Model::Atari800XL));
    ASSERT(m != nullptr);
    ASSERT(m->uiPanesSet);
    ASSERT(m->hasPane("drive") && m->hasPane("cartridge"));
    ASSERT(!m->hasPane("graphics-vic") && !m->hasPane("tape") && !m->hasPane("sidscope"));
}
