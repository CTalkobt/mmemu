#include "test_harness.h"
#include "plugins/machines/bbc/main/machine_electron.h"
#include "plugins/machines/bbc/main/bbc_tape.h"
#include "libcore/main/machine_desc.h"
#include "libdevices/main/io_registry.h"
#include "libdevices/main/ikeyboard_matrix.h"
#include "libdevices/main/ivideo_output.h"
#include "libdevices/main/iaudio_output.h"
#include "libmem/main/ibus.h"
#include <cstdio>
#include <string>
#include <vector>

namespace {
uint64_t run(MachineDescriptor* m, uint64_t cycles) {
    uint64_t t = 0;
    while (t < cycles) t += (uint64_t)m->schedulerStep(*m);
    return t;
}
template <typename T> T* find(MachineDescriptor* m) {
    std::vector<IOHandler*> hs;
    m->ioRegistry->enumerate(hs);
    for (auto* h : hs) if (auto* p = dynamic_cast<T*>(h)) return p;
    return nullptr;
}
MachineDescriptor* make() {
    std::string err;
    MachineDescriptor* m = electron::create(&err);
    if (!m) printf("  (skipped: %s)\n", err.c_str());
    return m;
}
// The OS keeps the text cursor's screen position in zero page; tests look at
// the picture instead: the count of lit pixels
size_t litPixels(MachineDescriptor* m) {
    auto* v = find<IVideoOutput>(m);
    const auto d = v->getDimensions();
    std::vector<uint32_t> px((size_t)d.width * d.height);
    v->renderFrame(px.data());
    size_t n = 0;
    for (uint32_t p : px) if (p & 0xFFFFFF) ++n;
    return n;
}
}  // namespace

// Power on: the OS prints "Acorn Electron" and "BASIC" and the prompt in
// Mode 6; the picture shows white text on black
TEST_CASE(electron_boots_basic) {
    MachineDescriptor* m = make();
    if (!m) return;
    run(m, 6000000);
    IBus* bus = m->buses[0].bus;
    // Mode 6's screen at &6000: 40 x 25 characters of 8 x 8 pixels (10 lines a row)
    EXPECT_TRUE(litPixels(m) > 500);
    // BASIC's PAGE is &E00 and the prompt is waiting at the keyboard
    EXPECT_EQ(bus->peek8(0x18), 0x0E);
    delete m;
}

// Typed text runs and keeps its case: a BASIC line storing results where
// the test can read them (the Electron's screen holds only pixels)
TEST_CASE(electron_typed_basic) {
    MachineDescriptor* m = make();
    if (!m) return;
    run(m, 4000000);
    auto* kbd = find<IKeyboardMatrix>(m);
    ASSERT(kbd);
    kbd->enqueueText("A$=\"Mixed\":?&70=ASC(MID$(A$,2,1)):?&71=6*7\n");
    run(m, 16000000);
    IBus* bus = m->buses[0].bus;
    EXPECT_EQ(bus->peek8(0x70), (uint8_t)'i');
    EXPECT_EQ(bus->peek8(0x71), 42);
    delete m;
}

// Mode 2: sixteen colours; a white rectangle fills the picture's middle
TEST_CASE(electron_mode2_graphics) {
    MachineDescriptor* m = make();
    if (!m) return;
    run(m, 4000000);
    auto* kbd = find<IKeyboardMatrix>(m);
    kbd->enqueueText("MODE 2:GCOL 0,7:MOVE 0,0:MOVE 1279,0:PLOT 85,0,1023:PLOT 85,1279,1023\n");
    run(m, 20000000);
    auto* v = find<IVideoOutput>(m);
    const auto d = v->getDimensions();
    std::vector<uint32_t> px((size_t)d.width * d.height);
    v->renderFrame(px.data());
    size_t white = 0;
    for (uint32_t p : px) if ((p & 0xFFFFFF) == 0xFFFFFF) ++white;
    EXPECT_TRUE(white > 600u * 480u);
    delete m;
}

// SOUND on the ULA's square wave: pitch 89 near 440 Hz
TEST_CASE(electron_sound_tone) {
    MachineDescriptor* m = make();
    if (!m) return;
    run(m, 4000000);
    auto* audio = find<IAudioOutput>(m);
    ASSERT(audio);
    std::vector<float> buf(44100);
    auto* kbd = find<IKeyboardMatrix>(m);
    kbd->enqueueText("SOUND 1,-15,89,40\n");
    run(m, 3000000);
    while (audio->pullSamples(buf.data(), (int)buf.size()) > 0) {}
    run(m, 1000000);
    const int n = audio->pullSamples(buf.data(), (int)buf.size());
    ASSERT(n > 20000);
    int crossings = 0;
    for (int i = 1; i < n; i++) if ((buf[i - 1] < 0) != (buf[i] < 0)) ++crossings;
    const double hz = crossings / 2.0 / (n / 44100.0);
    printf("  tone: %.1f Hz\n", hz);
    EXPECT_TRUE(hz > 400 && hz < 480);
    delete m;
}

// The cassette through the ULA: SAVE onto a recording (the ULA's transmit
// interrupt paces the bytes), saved as a UEF; a fresh Electron CHAINs it
TEST_CASE(electron_tape_save_and_chain) {
    MachineDescriptor* m = make();
    if (!m) return;
    const std::string path = "/tmp/mmemu_test_electron_tape.uef";
    run(m, 4000000);
    auto* tape = find<bbc::UefTape>(m);
    auto* kbd = find<IKeyboardMatrix>(m);
    ASSERT(tape && kbd);
    ASSERT(tape->startTapeRecord());
    kbd->enqueueText("10 ?&70=42\nSAVE \"PROG\"\n");
    run(m, 16000000);
    kbd->enqueueText("\n");                 // RECORD then RETURN
    run(m, 40000000);
    tape->stopTapeRecord();
    ASSERT(tape->saveTapeRecording(path));
    ASSERT(tape->recording().size() > 40);   // carrier, then the header and data block
    delete m;

    m = make();
    run(m, 4000000);
    tape = find<bbc::UefTape>(m);
    kbd = find<IKeyboardMatrix>(m);
    ASSERT(tape->mountTape(path));
    kbd->enqueueText("CHAIN \"\"\n");
    IBus* bus = m->buses[0].bus;
    bool ok = false;
    for (int i = 0; i < 120 && !ok; i++) { run(m, 1000000); ok = bus->peek8(0x70) == 42; }
    EXPECT_TRUE(ok);
    delete m;
    if (ok) std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// The Plus 1
// ---------------------------------------------------------------------------
namespace {
MachineDescriptor* makePlus1(const std::string& printer) {
    MachineDescriptor* m = make();
    if (!m) return nullptr;
    auto* slot = m->expansionSlot("edge");
    std::string err;
    if (!slot || !slot->install("plus1", printer, err)) {
        printf("  (skipped: %s)\n", err.c_str());
        delete m;
        return nullptr;
    }
    m->onReset(*m);
    return m;
}
}  // namespace

// The Plus 1's ROM answers ADVAL: joystick 0 held left and fire, ADVAL(1)
// near the top, ADVAL(2) centred, ADVAL(0)'s fire bit set
TEST_CASE(electron_plus1_joystick_adval) {
    MachineDescriptor* m = makePlus1("");
    if (!m) return;
    EXPECT_EQ(m->joystickPorts, 2);
    run(m, 4000000);
    m->onJoystick(0, 0x1F & ~0x04 & ~0x10);         // left, fire
    run(m, 2000000);
    auto* kbd = find<IKeyboardMatrix>(m);
    kbd->enqueueText("A%=ADVAL(1):B%=ADVAL(2):?&70=A% DIV 256:?&71=B% DIV 256:?&72=ADVAL(0) AND 3\n");
    run(m, 20000000);
    IBus* bus = m->buses[0].bus;
    EXPECT_TRUE(bus->peek8(0x70) >= 0xF0);
    EXPECT_TRUE(bus->peek8(0x71) >= 0x70 && bus->peek8(0x71) <= 0x90);
    EXPECT_EQ(bus->peek8(0x72), 1);
    delete m;
}

// VDU 2 sends text to the Plus 1's printer port, here a file
TEST_CASE(electron_plus1_printer) {
    const std::string path = "/tmp/mmemu_test_electron_printer.txt";
    std::remove(path.c_str());
    MachineDescriptor* m = makePlus1(path);
    if (!m) return;
    run(m, 4000000);
    find<IKeyboardMatrix>(m)->enqueueText("VDU2:PRINT \"PLUS ONE\":VDU3\n");
    run(m, 20000000);
    delete m;
    std::string out;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        char buf[256];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
        std::fclose(f);
    }
    EXPECT_TRUE(out.find("PLUS ONE") != std::string::npos);
    std::remove(path.c_str());
}

// A cartridge in slot 1: a service ROM (page 0) the OS finds at power-on and
// calls; its service entry counts the calls at &70
TEST_CASE(electron_plus1_cartridge) {
    MachineDescriptor* m = make();
    if (!m) return;
    std::string err;
    EXPECT_TRUE(!m->expansionSlot("cart1")->install("rom", "x", err));     // no Plus 1 yet
    delete m;
    m = makePlus1("");
    if (!m) return;
    std::vector<uint8_t> rom(0x4000, 0xFF);
    const uint8_t head[] = {0x00, 0x00, 0x00,           // no language
                            0x4C, 0x20, 0x80,           // JMP &8020: service
                            0x82, 0x0E, 0x01,           // service ROM; copyright at &800E; version
                            'T', 'E', 'S', 'T', 0x00,
                            0x00, '(', 'C', ')', 0x00};
    std::copy(std::begin(head), std::end(head), rom.begin());
    const uint8_t svc[] = {0xE6, 0x70, 0x60};           // INC &70: RTS
    std::copy(std::begin(svc), std::end(svc), rom.begin() + 0x20);
    const std::string path = "/tmp/mmemu_test_electron_cart.rom";
    if (FILE* f = std::fopen(path.c_str(), "wb")) { std::fwrite(rom.data(), 1, rom.size(), f); std::fclose(f); }
    ASSERT(m->expansionSlot("cart1")->install("rom", path, err));
    EXPECT_EQ(m->expansionSlot("cart1")->info(), "rom (" + path + ")");
    m->buses[0].bus->write8(0x70, 0);
    m->onReset(*m);
    run(m, 6000000);
    EXPECT_TRUE(m->buses[0].bus->peek8(0x70) > 0);
    // BASIC still starts (PAGE &E00)
    EXPECT_EQ(m->buses[0].bus->peek8(0x18), 0x0E);
    delete m;
    std::remove(path.c_str());
}
