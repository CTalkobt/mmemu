#include "test_harness.h"
#include "coco_test_rom.h"
#include "plugins/machines/coco/main/coco_addons.h"
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <string>
#include <unistd.h>
#include <vector>

// The Speech/Sound Cartridge and the CoCo Max pack (#355). The SSC runs a
// hand-built stand-in for its TMS7040 firmware (not the real one, which is
// copyrighted): on each byte at $FF7E (INT3) it reads port A; under $40 it
// is an allophone for the SP0256 (port D, /ALD pulsed), $80-$8F sets the
// AY's tone A (period $40, the mixer $3E, the volume the low nibble); then
// it raises BUSY's reset. The SP0256's stand-in "ROM" makes every
// allophone a pause of 5 (about 38 ms).

using namespace cocotest;

namespace {

struct Tms {
    std::vector<uint8_t> b = std::vector<uint8_t>(0x1000, 0x00);
    uint16_t pc = 0xF000;
    void emit(std::initializer_list<int> v) { for (int x : v) b[(size_t)(pc++ - 0xF000)] = (uint8_t)x; }
    void movp(int v, int p) { emit({0xA2, v, p}); }
    void vec(uint16_t at, uint16_t t) { b[at - 0xF000] = (uint8_t)(t >> 8); b[at - 0xF000 + 1] = (uint8_t)t; }
};

std::vector<uint8_t> sscFirmware() {
    Tms a;
    a.movp(0xFF, 0x0B);                 // DDR D out
    a.movp(0xFF, 0x09);                 // DDR C out
    a.movp(0xFF, 0x08);                 // port C: all deselected, BUSY reset high
    a.movp(0x10, 0x00);                 // INT3 on
    a.emit({0x05});                     // EINT
    const uint16_t idle = a.pc;
    a.emit({0x01});                     // IDLE
    a.emit({0xE0, (int)(uint8_t)(idle - (a.pc + 2))});
    // INT3
    a.pc = 0xF100;
    a.emit({0x80, 0x04});               // MOVP P4,A
    a.emit({0x2D, 0x80});               // CMP %>80,A
    const uint16_t jhs = a.pc;
    a.emit({0xE3, 0x00});               // JC sound (A >= $80; patched)
    a.emit({0x82, 0x0A});               // MOVP A,P10 (the allophone on D)
    a.movp(0x5F, 0x08);                 // /ALD and BUSY low
    a.movp(0xFF, 0x08);                 // both high: speak, not busy
    a.emit({0x0B});                     // RETI
    const uint16_t sound = a.pc;
    a.b[jhs + 1 - 0xF000] = (uint8_t)(sound - (jhs + 2));
    auto ay = [&](int reg, int v, bool fromA) {
        a.movp(reg, 0x0A);  a.movp(0x39, 0x08); a.movp(0x70, 0x08);     // latch the register
        if (fromA) a.emit({0x82, 0x0A}); else a.movp(v, 0x0A);
        a.movp(0x38, 0x08); a.movp(0x70, 0x08);                         // write it
    };
    ay(0, 0x40, false);
    ay(1, 0x00, false);
    ay(7, 0x3E, false);
    a.emit({0x23, 0x0F});               // AND %>0F,A
    ay(8, 0, true);
    a.movp(0xFF, 0x08);
    a.emit({0x0B});
    a.vec(0xFFF8, 0xF100);
    a.vec(0xFFFE, 0xF000);
    return a.b;
}

std::vector<uint8_t> speechStandIn() {
    std::vector<uint8_t> r(0x800, 0x00);
    for (int n = 0; n < 64; ++n) r[2 * n] = 0xF5;     // PAUSE 5, RTS
    return r;
}

std::string tempDir() {
    char t[] = "/tmp/coco_addons_XXXXXX";
    return mkdtemp(t);
}

void writeFile(const std::string& path, const std::vector<uint8_t>& bytes) {
    std::ofstream f(path, std::ios::binary);
    f.write((const char*)bytes.data(), (std::streamsize)bytes.size());
}

}  // namespace

// Attached as the CLI's `expansion cart ssc` does; without the TMS7040's ROM it says which file
TEST_CASE(coco_ssc_firmware_files) {
    MachineDescriptor* m = make();
    ASSERT(m);
    const char* old = std::getenv("MMEMU_COCO_FIRMWARE");
    const std::string keep = old ? old : "";
    setenv("MMEMU_COCO_FIRMWARE", "/nonexistent-coco-roms", 1);
    std::string err;
    ASSERT(!m->expansionSlot("cart")->install("ssc", "", err));
    if (old) setenv("MMEMU_COCO_FIRMWARE", keep.c_str(), 1); else unsetenv("MMEMU_COCO_FIRMWARE");
    ASSERT(err.find("pic-7040-510.bin") != std::string::npos);
    // The TMS7040's alone: speech silent, says so
    const std::string dir = tempDir();
    writeFile(dir + "/mcu.bin", sscFirmware());
    writeFile(dir + "/sp.bin", speechStandIn());
    writeFile(dir + "/short.bin", std::vector<uint8_t>(100, 0));
    ASSERT(!m->expansionSlot("cart")->install("ssc", dir + "/short.bin", err));
    ASSERT(m->expansionSlot("cart")->install("ssc", dir + "/mcu.bin,/nonexistent/no-speech-rom.bin", err) == false);
    setenv("MMEMU_COCO_FIRMWARE", "/nonexistent-coco-roms", 1);
    ASSERT(m->expansionSlot("cart")->install("ssc", dir + "/mcu.bin", err));
    if (old) setenv("MMEMU_COCO_FIRMWARE", keep.c_str(), 1); else unsetenv("MMEMU_COCO_FIRMWARE");
    ASSERT(m->expansionSlot("cart")->info().find("speech silent") != std::string::npos);
    ASSERT(m->expansionSlot("cart")->install("ssc", dir + "/mcu.bin," + dir + "/sp.bin", err));
    ASSERT(m->expansionSlot("cart")->info().find("speech silent") == std::string::npos);
    ASSERT(dynamic_cast<coco::SpeechSoundPak*>(coco::systemOf(m)->cart()) != nullptr);
    delete m;
}

// The 6809's view, as the owner's and service manuals describe it: BUSY
// until the firmware takes the byte, SBY low while it speaks, the AY's tone
// heard by the sound activity circuit, $FF7D bit 0 holding it all in reset
TEST_CASE(coco_ssc_handshake_and_sound) {
    MachineDescriptor* m = make();
    ASSERT(m);
    run(m, 20000);
    coco::System* sys = coco::systemOf(m);
    auto* ssc = new coco::SpeechSoundPak(sscFirmware(), speechStandIn());
    sys->setCart(std::unique_ptr<coco::Cart>(ssc));
    IBus* bus = m->buses[0].bus;
    run(m, 1000);
    ASSERT_EQ((int)bus->read8(0xFF7E), 0xFF);   // idle: not busy, standing by, quiet

    // An allophone
    bus->write8(0xFF7E, 0x05);
    ASSERT_EQ((int)(bus->read8(0xFF7E) & 0x80), 0x00);     // BUSY
    run(m, 200);                                            // 0.2 ms: the firmware has it
    uint8_t st = bus->read8(0xFF7E);
    ASSERT_EQ((int)(st & 0x80), 0x80);
    ASSERT_EQ((int)(st & 0x40), 0x00);                      // speaking
    ASSERT(!ssc->speech().sby());
    uint64_t t = 0;
    while (!(bus->read8(0xFF7E) & 0x40) && t < 200000) t += run(m, 100);
    const double ms = (double)t / 894.886;
    ASSERT(ms > 30 && ms < 45);                             // 5-6 pause periods of 6.4 ms

    // A tone: the AY's registers through port C/D
    bus->write8(0xFF7E, 0x8C);
    run(m, 2000);
    ASSERT_EQ((int)(bus->read8(0xFF7E) & 0x80), 0x80);
    ASSERT_EQ((int)ssc->psg().reg(0), 0x40);
    ASSERT_EQ((int)ssc->psg().reg(7), 0x3E);
    ASSERT_EQ((int)ssc->psg().reg(8), 0x0C);
    ASSERT_EQ((int)(bus->read8(0xFF7E) & 0x20), 0x20);     // not yet heard
    // The owner's manual's set-up: the analog mux on, selecting the cartridge
    bus->write8(0xFF01, 0x34);                              // CA2 low
    bus->write8(0xFF03, 0x3F);                              // CB2 high
    bus->write8(0xFF23, 0x3C);                              // PIA1 CB2 high: the mux on
    float lo = 1, hi = -1;
    for (int i = 0; i < 2000; ++i) { run(m, 10); lo = std::min(lo, sys->soundLevel()); hi = std::max(hi, sys->soundLevel()); }
    ASSERT(hi - lo > 0.02f);                                // a square wave on the CoCo's sound
    ASSERT_EQ((int)(bus->read8(0xFF7E) & 0x20), 0x00);     // the sound activity circuit
    ASSERT(ssc->soundActive());

    // $FF7D bit 0: 1 then 0 resets the TMS7040 and the AY
    bus->write8(0xFF7D, 0x01);
    ASSERT_EQ((int)ssc->psg().reg(8), 0x00);
    const uint16_t heldAt = ssc->mcu().pc();
    run(m, 1000);
    ASSERT_EQ((int)ssc->mcu().pc(), heldAt);               // held in reset
    bus->write8(0xFF7D, 0x00);
    run(m, 1000);
    ASSERT_EQ((int)(bus->read8(0xFF7E) & 0x80), 0x80);
    // (the TMS7040's reset drops port C, /ALD with it: MAME's model speaks
    // port D's allophone, a pause here)
    run(m, 50000);
    ASSERT_EQ((int)(bus->read8(0xFF7E) & 0xC0), 0xC0);
    // The silence is heard after a while
    run(m, 300000);
    ASSERT_EQ((int)(bus->read8(0xFF7E) & 0x20), 0x20);
    delete m;
}

// In a Multi-Pak slot it still answers at $FF7E (no /SCS: every slot sees it)
TEST_CASE(coco_ssc_in_multipak) {
    MachineDescriptor* m = make();
    ASSERT(m);
    std::string err;
    const std::string dir = tempDir();
    writeFile(dir + "/mcu.bin", sscFirmware());
    ASSERT(m->expansionSlot("cart")->install("mpi", "4", err));
    ASSERT(m->expansionSlot("mpi2")->install("ssc", dir + "/mcu.bin", err));
    IBus* bus = m->buses[0].bus;
    run(m, 1000);
    bus->write8(0xFF7E, 0x01);
    ASSERT_EQ((int)(bus->read8(0xFF7E) & 0x80), 0x00);
    run(m, 200);
    ASSERT_EQ((int)(bus->read8(0xFF7E) & 0x80), 0x80);
    delete m;
}

// The CoCo Max pack: a read returns the last conversion and starts its
// channel's next; the host mouse moves it
TEST_CASE(coco_max_pack) {
    MachineDescriptor* m = make();
    ASSERT(m);
    std::string err;
    ASSERT(m->expansionSlot("cart")->install("cocomax", "", err));
    coco::System* sys = coco::systemOf(m);
    auto* cm = coco::findCocoMax(sys->cart());
    ASSERT(cm);
    IBus* bus = m->buses[0].bus;
    cm->setPosition(200, 17);
    ASSERT_EQ((int)bus->read8(0xFF91), 0x00);   // nothing converted yet; X starts
    ASSERT_EQ((int)bus->read8(0xFF90), 200);    // X; Y starts
    ASSERT_EQ((int)bus->read8(0xFF92), 17);     // Y; the left button starts
    ASSERT_EQ((int)bus->read8(0xFF93), 0xFF);   // not pressed; the right starts
    ASSERT_EQ((int)bus->peek8(0xFF97), 0xFF);   // (peek: no conversion)
    m->onMouse(-300, 5, 1, 0);                  // the mouse: clamped, the left button
    ASSERT_EQ(cm->x(), 0);
    ASSERT_EQ(cm->y(), 22);
    bus->read8(0xFF92);
    ASSERT_EQ((int)bus->read8(0xFF94), 0x00);   // pressed
    ASSERT_EQ(sys->axis(0, 0), (uint16_t)32768);    // the joystick port left alone
    // Through a Multi-Pak; not on a CoCo 3
    ASSERT(m->expansionSlot("cart")->install("mpi", "1", err));
    ASSERT(coco::findCocoMax(sys->cart()) == cm);
    delete m;
    MachineDescriptor* m3 = coco::create(coco::Model::CoCo3, &err, 128);
    ASSERT(m3);
    ASSERT(!m3->expansionSlot("cart")->install("cocomax", "", err));
    delete m3;
}

// The CoCo Max 3 adapter (CoCo 3 joystick ports): the ramp starts when PIA0
// PA0-PA3 are driven low; the comparator rises 500 us + position x 2624 us later
TEST_CASE(coco3_cm3_hires_adapter) {
    std::string err;
    MachineDescriptor* m = coco::create(coco::Model::CoCo3, &err, 128);
    ASSERT(m);
    coco::System* sys = coco::systemOf(m);
    ASSERT(m->expansionSlot("joyright")->install("cm3", "", err));
    ASSERT_EQ(m->expansionSlot("joyright")->info(), std::string("cm3"));
    ASSERT(sys->joyInterface(0) == coco::JoyInterface::CoCoMax3);
    ASSERT_EQ(sys->mousePort(), 0);
    MachineDescriptor* m2 = make();
    ASSERT(!m2->expansionSlot("joyright")->install("cm3", "", err));
    delete m2;

    IBus* bus = m->buses[0].bus;
    // X, right; PA0-PA3 outputs, high (the ramp held)
    bus->write8(0xFF01, 0x30); bus->write8(0xFF00, 0x0F); bus->write8(0xFF01, 0x34);
    bus->write8(0xFF03, 0x34);
    bus->write8(0xFF00, 0x0F);
    auto rampUs = [&](uint16_t pos) {
        sys->setAxis(0, 0, pos);
        bus->write8(0xFF00, 0x0F);
        run(m, 10);
        bus->write8(0xFF00, 0x00);                   // the ramp runs
        uint64_t t = 0;
        while (!(bus->read8(0xFF00) & 0x80) && t < 20000) t += run(m, 1);
        return (double)t / 0.894886;
    };
    const double lo = rampUs(0), hi = rampUs(65535);
    ASSERT(std::fabs(lo - 500) < 30);
    ASSERT(std::fabs(hi - (500 + 2624)) < 40);
    delete m;
}
