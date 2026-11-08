#include "test_harness.h"
#include "plugins/machines/bbc/main/machine_bbc.h"
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
// Mode 7's screen at &7C00: 25 rows of 40 characters
std::string textRow(MachineDescriptor* m, int row) {
    IBus* bus = m->buses[0].bus;
    std::string s;
    for (int i = 0; i < 40; ++i) {
        const char c = (char)(bus->peek8((uint16_t)(0x7C00 + row * 40 + i)) & 0x7F);
        s += (c >= 0x20 && c < 0x7F) ? c : ' ';
    }
    return s;
}
bool screenHas(MachineDescriptor* m, const std::string& text) {
    for (int r = 0; r < 25; ++r)
        if (textRow(m, r).find(text) != std::string::npos) return true;
    return false;
}
template <typename T> T* find(MachineDescriptor* m) {
    std::vector<IOHandler*> hs;
    m->ioRegistry->enumerate(hs);
    for (auto* h : hs) if (auto* p = dynamic_cast<T*>(h)) return p;
    return nullptr;
}
MachineDescriptor* make() {
    std::string err;
    MachineDescriptor* m = bbc::create(bbc::Model::B, &err);
    if (!m) printf("  (skipped: %s)\n", err.c_str());
    return m;
}
}  // namespace

// Power on: the MOS beeps, shows "BBC Computer 32K" and BASIC's ">" prompt
// in Mode 7; a typed line runs
TEST_CASE(bbc_boots_basic) {
    MachineDescriptor* m = make();
    if (!m) return;
    run(m, 4000000);
    ASSERT(screenHas(m, "BBC Computer 32K"));
    ASSERT(screenHas(m, "BASIC"));
    ASSERT(screenHas(m, ">"));
    auto* kbd = find<IKeyboardMatrix>(m);
    ASSERT(kbd);
    kbd->enqueueText("PRINT 6*7\n");
    run(m, 6000000);
    EXPECT_TRUE(screenHas(m, "42"));
    delete m;
}

// BREAK is a soft reset: a BASIC program survives it (OLD brings it back)
TEST_CASE(bbc_break_keeps_program) {
    MachineDescriptor* m = make();
    if (!m) return;
    run(m, 4000000);
    auto* kbd = find<IKeyboardMatrix>(m);
    ASSERT(kbd);
    kbd->enqueueText("10 PRINT \"HI\"\n");
    run(m, 4000000);
    m->onKey("break", true);
    m->onKey("break", false);
    run(m, 4000000);
    ASSERT(screenHas(m, "BBC Computer"));
    kbd->enqueueText("OLD\nLIST\n");
    run(m, 6000000);
    EXPECT_TRUE(screenHas(m, "10 PRINT \"HI\""));
    delete m;
}

// Mode 1: the 6845 and ULA draw a 320 x 256 four-colour screen; a filled
// rectangle shows in the picture (white on black)
TEST_CASE(bbc_mode1_graphics) {
    MachineDescriptor* m = make();
    if (!m) return;
    run(m, 4000000);
    auto* kbd = find<IKeyboardMatrix>(m);
    ASSERT(kbd);
    kbd->enqueueText("MODE 1\nGCOL 0,3:MOVE 0,0:MOVE 1279,0:PLOT 85,0,1023:PLOT 85,1279,1023\n");
    run(m, 16000000);
    auto* video = find<IVideoOutput>(m);
    ASSERT(video);
    const auto d = video->getDimensions();
    std::vector<uint32_t> px((size_t)d.width * d.height);
    video->renderFrame(px.data());
    size_t white = 0;
    for (uint32_t p : px) if ((p & 0xFFFFFF) == 0xFFFFFF) ++white;
    // 640 x 256 picture pixels, each drawn on two lines
    EXPECT_TRUE(white > 600u * 250u * 2u);
    delete m;
}

// Typed text keeps its case: Caps Lock (on at power-on) is turned off for
// lower case, as a typist would (Shift doesn't reverse Caps Lock unless
// SHIFT+CAPS LOCK set that mode)
TEST_CASE(bbc_typed_text_case) {
    MachineDescriptor* m = make();
    if (!m) return;
    run(m, 3000000);
    auto* kbd = find<IKeyboardMatrix>(m);
    ASSERT(kbd);
    kbd->enqueueText("PRINT \"Mixed Case\"\n");
    run(m, 8000000);
    EXPECT_TRUE(screenHas(m, "Mixed Case"));
    delete m;
}

// SOUND 1,-15,P,D: channel 1 at full volume; pitch 89 is A. OS 1.20 writes
// the period 278 for it, so the SN76489 (4 MHz / 32 / N) plays 449.6 Hz, a
// little sharp of 440: the output crosses zero twice a cycle
TEST_CASE(bbc_sound_tone) {
    MachineDescriptor* m = make();
    if (!m) return;
    run(m, 3000000);
    auto* audio = find<IAudioOutput>(m);
    ASSERT(audio);
    std::vector<float> buf(44100);
    while (audio->pullSamples(buf.data(), (int)buf.size()) > 0) {}
    auto* kbd = find<IKeyboardMatrix>(m);
    kbd->enqueueText("SOUND 1,-15,89,40\n");
    run(m, 3000000);                       // typing: about 1.2 s
    while (audio->pullSamples(buf.data(), (int)buf.size()) > 0) {}
    run(m, 1000000);                       // half a second of the note
    const int n = audio->pullSamples(buf.data(), (int)buf.size());
    ASSERT(n > 20000);
    float mean = 0;
    for (int i = 0; i < n; i++) mean += buf[i];
    mean /= n;
    int crossings = 0;
    for (int i = 1; i < n; i++)
        if ((buf[i - 1] - mean < 0) != (buf[i] - mean < 0)) ++crossings;
    const double hz = crossings / 2.0 / (n / 44100.0);
    printf("  tone: %.1f Hz\n", hz);
    EXPECT_TRUE(hz > 445 && hz < 454);
    delete m;
}

// Joysticks: the sticks through the ADC (ADVAL(1) is joystick 1's X:
// high to the left), the fire buttons on the System VIA (ADVAL(0) bits 0-1)
TEST_CASE(bbc_joystick_adc) {
    MachineDescriptor* m = make();
    if (!m) return;
    run(m, 3000000);
    auto* kbd = find<IKeyboardMatrix>(m);
    ASSERT(kbd);
    m->onJoystick(0, (uint8_t)(0x1F & ~0x04 & ~0x10));      // left, fire
    run(m, 200000);
    kbd->enqueueText("PRINT ADVAL(1) DIV 256;\"/\";ADVAL(2) DIV 256;\"/\";ADVAL(0) AND 3\n");
    run(m, 14000000);                                       // typing: 70 ms a key
    EXPECT_TRUE(screenHas(m, "255/127/1"));
    m->onJoystick(0, (uint8_t)(0x1F & ~0x08));               // right, no fire
    run(m, 200000);
    kbd->enqueueText("PRINT ADVAL(1);\"/\";ADVAL(0) AND 3\n");
    run(m, 8000000);
    EXPECT_TRUE(screenHas(m, "0/0"));
    delete m;
}

// Model A: 16 KB (seen twice), no DFS: Mode 7's screen ends RAM at &4000
TEST_CASE(bbc_model_a) {
    std::string err;
    MachineDescriptor* m = bbc::create(bbc::Model::A, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    run(m, 4000000);
    ASSERT(screenHas(m, "BBC Computer 16K"));
    EXPECT_TRUE(!screenHas(m, "DFS"));
    auto* kbd = find<IKeyboardMatrix>(m);
    ASSERT(kbd);
    kbd->enqueueText("PRINT ~PAGE;\" \";~HIMEM\n");
    run(m, 6000000);
    EXPECT_TRUE(screenHas(m, "E00 3C00"));
    delete m;
}

// The 6502 Second Processor on the Tube: DNFS's Tube host finds it ("Acorn
// TUBE 6502 64K"), BASIC is copied across to &8000 and runs there (PAGE
// &800, HIMEM &8000); BREAK restarts both
TEST_CASE(bbc_tube_6502) {
    std::string err;
    MachineDescriptor* m = bbc::create(bbc::Model::BTube, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    run(m, 4000000);
    ASSERT(screenHas(m, "Acorn TUBE 6502 64K"));
    ASSERT(screenHas(m, ">"));
    auto* kbd = find<IKeyboardMatrix>(m);
    ASSERT(kbd);
    kbd->enqueueText("PRINT ~PAGE;\" \";~HIMEM;\" \";6*7\n");
    run(m, 12000000);
    EXPECT_TRUE(screenHas(m, "800 8000 42"));
    // The program is in the second processor's memory, not the BBC's
    kbd->enqueueText("10 REM TUBE\n");
    run(m, 6000000);
    IBus* tube = m->buses.size() > 1 ? m->buses[1].bus : nullptr;
    ASSERT(tube);
    EXPECT_EQ(tube->peek8(0x0800), 0x0D);           // BASIC's program start
    m->onKey("break", true);
    m->onKey("break", false);
    run(m, 4000000);
    EXPECT_TRUE(screenHas(m, "Acorn TUBE 6502 64K"));
    delete m;
}

// The Z80 Second Processor (b-em z80.c, client ROM 1.21). Its client sets
// a bitmap mode, so the test reads the Z80's memory: the host's language
// (BASIC) copied over the Tube to &8000; with Acorn's CP/M system disc in
// drive 0 (b-em discs/cpm1.dsd, when the b-em reference tree is beside this
// one) CP/M 2.2 boots: the client reads the disc through OSWORD &7F and
// Tube NMI transfers. (CP/M's prompt comes later; the CLI shows DIR working.)
bool ramHas(IBus* bus, const std::string& text) {
    std::string mem;
    for (uint32_t a = 0; a < 0x10000; ++a) mem += (char)(bus->peek8(a) & 0x7F);
    return mem.find(text) != std::string::npos;
}
TEST_CASE(bbc_tube_z80_cpm) {
    std::string err;
    MachineDescriptor* m = bbc::create(bbc::Model::BZ80, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    run(m, 40000000);                                   // BASIC is copied over the Tube first
    IBus* z80mem = m->buses[1].bus;
    EXPECT_TRUE(z80mem->peek8(0x8009) == 'B' && z80mem->peek8(0x800A) == 'A');   // "BASIC"
    const char* disc = "../refs/b-em/discs/cpm1.dsd";
    if (FILE* f = std::fopen(disc, "rb")) {
        std::fclose(f);
        delete m;                                       // power on with the disc in
        m = bbc::create(bbc::Model::BZ80, &err);
        z80mem = m->buses[1].bus;
        IOHandler* fdc = m->ioRegistry->findHandler("8271 FDC");
        ASSERT(fdc && fdc->mountDisk(0, disc));
        run(m, 50000000);
        // The BIOS's banner: loaded from the disc (the client ROM has no such text)
        EXPECT_TRUE(ramHas(z80mem, "Acorn CP/M 2.2"));
    } else {
        printf("  (CP/M part skipped: no %s)\n", disc);
    }
    delete m;
}

// The 6809 Second Processor (b-em 6809tube.c, J.G. Harston's client 1.05a):
// its banner and command prompt; *HELP typed there runs on the BBC
TEST_CASE(bbc_tube_6809) {
    std::string err;
    MachineDescriptor* m = bbc::create(bbc::Model::B6809, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    run(m, 24000000);
    EXPECT_TRUE(screenHas(m, "6809 TUBE 64K"));
    EXPECT_TRUE(screenHas(m, "6809>*"));
    find<IKeyboardMatrix>(m)->enqueueText("HELP\n");
    run(m, 24000000);
    EXPECT_TRUE(screenHas(m, "OS 1.20"));
    delete m;
}

// The ARM Evaluation System (b-em arm.c, ARMeval 1.00): its banner and
// prompt; *HELP typed there lists the Supervisor and Executive
TEST_CASE(bbc_tube_arm) {
    std::string err;
    MachineDescriptor* m = bbc::create(bbc::Model::BArm, &err);
    if (!m) { printf("  (skipped: %s)\n", err.c_str()); return; }
    run(m, 24000000);
    EXPECT_TRUE(screenHas(m, "Acorn ARM Second Processor 4096K"));
    find<IKeyboardMatrix>(m)->enqueueText("HELP\n");
    run(m, 24000000);
    EXPECT_TRUE(screenHas(m, "Supervisor 1.00"));
    EXPECT_TRUE(screenHas(m, "Executive version 1.00"));
    delete m;
}
