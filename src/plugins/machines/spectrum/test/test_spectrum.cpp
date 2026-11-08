#include "test_harness.h"
#include "plugins/machines/spectrum/main/machine_spectrum.h"
#include "libcore/main/machine_desc.h"
#include "libdevices/main/io_registry.h"
#include "libdevices/main/ikeyboard_matrix.h"
#include "libdevices/main/ivideo_output.h"
#include "libdevices/main/iaudio_output.h"
#include "libmem/main/ibus.h"
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>

namespace {
uint64_t run(MachineDescriptor* m, uint64_t tstates) {
    uint64_t t = 0;
    while (t < tstates) t += (uint64_t)m->schedulerStep(*m);
    return t;
}
template <typename T> T* find(MachineDescriptor* m) {
    std::vector<IOHandler*> hs;
    m->ioRegistry->enumerate(hs);
    for (auto* h : hs) if (auto* p = dynamic_cast<T*>(h)) return p;
    return nullptr;
}
MachineDescriptor* make(spectrum::Model model) {
    std::string err;
    MachineDescriptor* m = spectrum::create(model, &err);
    if (!m) printf("  (skipped: %s)\n", err.c_str());
    return m;
}
// A text row of the screen read back through the 48K ROM's character set
// (at $3D00 in the 48 BASIC ROM); '?' where no character matches
std::string screenRow(MachineDescriptor* m, int row) {
    IBus* bus = m->buses[0].bus;
    static std::vector<uint8_t> font;
    if (font.empty()) {
        FILE* f = std::fopen("roms/spectrum/48.rom", "rb");
        std::vector<uint8_t> rom(0x4000);
        if (f) { size_t n = std::fread(rom.data(), 1, rom.size(), f); (void)n; std::fclose(f); }
        font.assign(rom.begin() + 0x3D00, rom.end());
    }
    std::string s;
    for (int col = 0; col < 32; ++col) {
        uint8_t cell[8];
        for (int l = 0; l < 8; ++l) {
            const int y = row * 8 + l;
            const uint16_t a = (uint16_t)(0x4000 | ((y & 0xC0) << 5) | ((y & 7) << 8) | ((y & 0x38) << 2) | col);
            cell[l] = bus->peek8(a);
        }
        char c = '?';
        for (int ch = 0; ch < 96; ++ch) {
            bool same = true, inverse = true;
            for (int l = 0; l < 8; ++l) {
                same = same && cell[l] == font[ch * 8 + l];
                inverse = inverse && cell[l] == (uint8_t)~font[ch * 8 + l];
            }
            if (same || inverse) { c = (char)(0x20 + ch); break; }
        }
        s += c;
    }
    return s;
}
std::string screenText(MachineDescriptor* m) {
    std::string s;
    for (int r = 0; r < 24; ++r) s += screenRow(m, r) + "\n";
    return s;
}
}  // namespace

// Power on: the 48K's RAM test and the copyright message on the bottom line
TEST_CASE(spectrum48_boots) {
    MachineDescriptor* m = make(spectrum::Model::S48);
    if (!m) return;
    run(m, 3500000 * 3);
    const std::string bottom = screenRow(m, 23);
    if (bottom.find("1982 Sinclair Research Ltd") == std::string::npos) printf("%s", screenText(m).c_str());
    EXPECT_TRUE(bottom.find("1982 Sinclair Research Ltd") != std::string::npos);
    // RAMTOP: all 48 KB found
    IBus* bus = m->buses[0].bus;
    EXPECT_EQ(bus->peek8(23732) | bus->peek8(23733) << 8, 0xFFFF);   // P_RAMT
    // The picture: white paper and border (index 7: $C0C0C0)
    auto* v = find<IVideoOutput>(m);
    const auto d = v->getDimensions();
    std::vector<uint32_t> px((size_t)d.width * d.height);
    v->renderFrame(px.data());
    EXPECT_EQ(px[0] & 0xFFFFFF, 0xC0C0C0u);
    delete m;
}

// A typed BASIC line (keywords entered as tokens) runs: PRINT, BORDER, POKE
TEST_CASE(spectrum48_typed_basic) {
    MachineDescriptor* m = make(spectrum::Model::S48);
    if (!m) return;
    run(m, 3500000 * 3);
    auto* kbd = find<IKeyboardMatrix>(m);
    ASSERT(kbd);
    kbd->enqueueText("10 PRINT \"HELLO\";6*7: BORDER 2: POKE 32768,CODE \"Q\"\nRUN\n");
    run(m, 3500000 * 6);
    IBus* bus = m->buses[0].bus;
    const std::string text = screenText(m);
    if (text.find("HELLO42") == std::string::npos) printf("%s", text.c_str());
    EXPECT_TRUE(text.find("HELLO42") != std::string::npos);
    EXPECT_EQ(bus->peek8(32768), (uint8_t)'Q');
    // BORDER 2: red in the picture's corner
    auto* v = find<IVideoOutput>(m);
    const auto d = v->getDimensions();
    std::vector<uint32_t> px((size_t)d.width * d.height);
    v->renderFrame(px.data());
    EXPECT_EQ(px[0] & 0xFFFFFF, 0xC00000u);
    delete m;
}

// BEEP: a square wave on the beeper near the note's pitch (BEEP 0.5,0: middle C, 261.6 Hz)
TEST_CASE(spectrum48_beep) {
    MachineDescriptor* m = make(spectrum::Model::S48);
    if (!m) return;
    run(m, 3500000 * 3);
    auto* kbd = find<IKeyboardMatrix>(m);
    auto* audio = find<IAudioOutput>(m);
    float drain[4096];
    while (audio->pullSamples(drain, 4096) > 0) {}
    kbd->enqueueText("BEEP 1,0\n");
    std::vector<float> s;
    for (int i = 0; i < 30; ++i) {                    // 3 seconds, a tenth at a time
        run(m, 350000);
        float buf[8192];
        int n;
        while ((n = audio->pullSamples(buf, 8192)) > 0) s.insert(s.end(), buf, buf + n);
    }
    // Count the rising crossings in the loudest half second
    int best = 0;
    for (size_t at = 0; at + 22050 < s.size(); at += 4410) {
        int n = 0;
        float mean = 0;
        for (size_t i = at; i < at + 22050; ++i) mean += s[i];
        mean /= 22050;
        for (size_t i = at + 1; i < at + 22050; ++i) if (s[i - 1] < mean && s[i] >= mean) ++n;
        best = std::max(best, n);
    }
    printf("    beeper: %d cycles in half a second\n", best);
    EXPECT_TRUE(std::abs(best - 131) <= 6);
    delete m;
}

#include "plugins/z80/main/cpu_z80.h"

// Contention (Fuse spectrum.c, the 48K's 6,5,4,3,2,1,0,0 from T-state
// 14336 of the frame): a NOP fetched from $4000-$7FFF waits for the ULA in
// the screen's lines; from $8000 it never does
TEST_CASE(spectrum48_contention) {
    MachineDescriptor* m = make(spectrum::Model::S48);
    if (!m) return;
    auto* cpu = dynamic_cast<Z80*>(m->cpus[0].cpu);
    ASSERT(cpu);
    IBus* bus = m->buses[0].bus;
    bus->write8(0x6000, 0x00);
    bus->write8(0x8000, 0x00);
    cpu->setIrqLine(false);
    struct { int t; int want; } cases[] = {
        {14334, 4}, {14335, 4}, {14336, 10}, {14337, 9}, {14338, 8}, {14339, 7}, {14340, 6},
        {14341, 5}, {14342, 4}, {14343, 4}, {14344, 10},
        {14336 + 125, 5}, {14336 + 126, 4},              // the line's last contended T-states
        {14336 + 224, 10},                                // the next line
        {14336 + 191 * 224, 10}, {14336 + 192 * 224, 4},  // the last screen line, then the border
    };
    for (auto& c : cases) {
        cpu->setPc(0x6000);
        cpu->setCycles((uint64_t)c.t);
        const int got = cpu->step();
        if (got != c.want) printf("    NOP at T %d: %d T-states, want %d\n", c.t, got, c.want);
        EXPECT_EQ(got, c.want);
        cpu->setPc(0x8000);
        cpu->setCycles((uint64_t)c.t);
        EXPECT_EQ(cpu->step(), 4);
    }
    delete m;
}

// The floating bus (Fuse spectrum_unattached_port, after Ramsoft): an
// unattached port reads what the ULA is fetching: bitmap, attribute,
// bitmap, attribute, then four idle ($FF) T-states, 8 per two columns
TEST_CASE(spectrum48_floating_bus) {
    MachineDescriptor* m = make(spectrum::Model::S48);
    if (!m) return;
    auto* cpu = dynamic_cast<Z80*>(m->cpus[0].cpu);
    IBus* bus = m->buses[0].bus;
    bus->write8(0x4000, 0xAA); bus->write8(0x4001, 0x55);
    bus->write8(0x5800, 0x47); bus->write8(0x5801, 0x16);
    bus->write8(0x8000, 0xDB); bus->write8(0x8001, 0xFF);   // IN A,($FF)
    cpu->setIrqLine(false);
    // The port is read 10 T-states into IN A,(n): M1 4, n 3, I/O 1 + 2
    const struct { int t; uint8_t want; } cases[] = {
        {14326, 0xFF}, {14327, 0xFF}, {14328, 0xAA}, {14329, 0x47}, {14330, 0x55}, {14331, 0x16},
        {14332, 0xFF}, {14335, 0xFF}, {14320 - 10, 0xFF},
    };
    for (auto& c : cases) {
        cpu->regWrite(Z80::R_AF, 0x0000);               // A = 0: port $00FF
        cpu->setPc(0x8000);
        cpu->setCycles((uint64_t)c.t);
        cpu->step();
        EXPECT_EQ((int)cpu->a(), (int)c.want);
    }
    delete m;
}

// The 128K: the menu, 128 BASIC from it, $7FFD paging (RAM 7 at $C000)
TEST_CASE(spectrum128_boots_and_pages) {
    MachineDescriptor* m = make(spectrum::Model::S128);
    if (!m) return;
    run(m, 3546900 * 3);
    const std::string text = screenText(m);
    if (text.find("128 BASIC") == std::string::npos) printf("%s", text.c_str());
    EXPECT_TRUE(text.find("128 BASIC") != std::string::npos);
    EXPECT_TRUE(text.find("1986 Sinclair Research Ltd") != std::string::npos);
    // Down to 128 BASIC, ENTER; a line that pages RAM 1 in at $C000, stores
    // there, pages RAM 0 back, and reads the bank directly
    auto* kbd = find<IKeyboardMatrix>(m);
    for (const char* k : {"down", "return"}) {          // keys on the matrix
        kbd->pressKeyByName(k, true);
        run(m, 3546900 / 10);
        kbd->pressKeyByName(k, false);
        run(m, 3546900 / 2);
    }
    run(m, 3546900);
    // CLEAR moves the machine stack below $C000 first; bit 4 keeps the 48
    // BASIC ROM, which runs the program; BANKM (23388) keeps the last
    // $7FFD value for the ROM's own paging
    kbd->enqueueText("5 CLEAR 32767\n"
                     "10 POKE 23388,17: OUT 32765,17: POKE 49152,7: POKE 23388,16: OUT 32765,16: POKE 49152,5\n"
                     "20 POKE 23388,17: OUT 32765,17: LET A=PEEK 49152: POKE 23388,16: OUT 32765,16\n"
                     "30 PRINT A*100+PEEK 49152\nRUN\n");
    run(m, 3546900 * 12);
    const std::string after = screenText(m);
    if (after.find("705") == std::string::npos) printf("%s", after.c_str());
    EXPECT_TRUE(after.find("705") != std::string::npos);
    delete m;
}
