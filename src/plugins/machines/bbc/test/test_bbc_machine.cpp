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
