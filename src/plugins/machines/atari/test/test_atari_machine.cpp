#include "test_harness.h"
#include "plugins/machines/atari/main/machine_atari.h"
#include "libcore/main/machine_desc.h"
#include "libdevices/main/io_registry.h"
#include "libdevices/main/ikeyboard_matrix.h"
#include "libdevices/main/ivideo_output.h"
#include "plugins/devices/gtia/main/gtia.h"
#include <memory>
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
