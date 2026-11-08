// ZX Spectrum tapes and snapshots (#242)

#include "test_harness.h"
#include "plugins/machines/spectrum/main/machine_spectrum.h"
#include "plugins/machines/spectrum/main/spectrum_tape.h"
#include "libcore/main/machine_desc.h"
#include "libdevices/main/io_registry.h"
#include "libdevices/main/ikeyboard_matrix.h"
#include "libdevices/main/ivideo_output.h"
#include "libmem/main/ibus.h"
#include "plugins/z80/main/cpu_z80.h"
#include <cstdio>
#include <cstring>
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
// The screen's text through the 48K ROM font (as test_spectrum.cpp)
std::string screenText(MachineDescriptor* m) {
    IBus* bus = m->buses[0].bus;
    static std::vector<uint8_t> font;
    if (font.empty()) {
        FILE* f = std::fopen("roms/spectrum/48.rom", "rb");
        std::vector<uint8_t> rom(0x4000);
        if (f) { size_t n = std::fread(rom.data(), 1, rom.size(), f); (void)n; std::fclose(f); }
        font.assign(rom.begin() + 0x3D00, rom.end());
    }
    std::string s;
    for (int row = 0; row < 24; ++row) {
        for (int col = 0; col < 32; ++col) {
            uint8_t cell[8];
            for (int l = 0; l < 8; ++l) {
                const int y = row * 8 + l;
                cell[l] = bus->peek8((uint16_t)(0x4000 | ((y & 0xC0) << 5) | ((y & 7) << 8) | ((y & 0x38) << 2) | col));
            }
            char c = '?';
            for (int ch = 0; ch < 96; ++ch) {
                bool same = true, inv = true;
                for (int l = 0; l < 8; ++l) {
                    same = same && cell[l] == font[ch * 8 + l];
                    inv = inv && cell[l] == (uint8_t)~font[ch * 8 + l];
                }
                if (same || inv) { c = (char)(0x20 + ch); break; }
            }
            s += c;
        }
        s += '\n';
    }
    return s;
}
std::vector<uint8_t> tapOf(const std::vector<std::vector<uint8_t>>& blocks) {
    std::vector<uint8_t> out;
    for (auto& b : blocks) {
        out.push_back((uint8_t)b.size());
        out.push_back((uint8_t)(b.size() >> 8));
        out.insert(out.end(), b.begin(), b.end());
    }
    return out;
}
// A program SAVEd through the ROM trap: its header and data blocks
std::vector<std::vector<uint8_t>> saveProgram(const char* line) {
    MachineDescriptor* m = make(spectrum::Model::S48);
    if (!m) return {};
    run(m, 3500000 * 3);
    auto* kbd = find<IKeyboardMatrix>(m);
    kbd->enqueueText(std::string(line) + "\nSAVE \"T\"\n ");   // a key after "Start tape..."
    run(m, 3500000 * 4);
    auto saved = find<spectrum::SpectrumTape>(m)->saved();
    delete m;
    return saved;
}
}  // namespace

// SAVE through the trap gives a header (flag 0, type 0 "program", name
// "T") and the data block; LOAD "" through the trap puts the program back
TEST_CASE(spectrum48_tape_trap_save_load) {
    auto saved = saveProgram("10 PRINT 42*2");
    if (saved.empty()) return;
    ASSERT_EQ(saved.size(), (size_t)2);
    ASSERT_EQ(saved[0].size(), (size_t)19);
    EXPECT_EQ(saved[0][0], 0x00);                       // header flag
    EXPECT_EQ(saved[0][1], 0x00);                       // program
    EXPECT_EQ(saved[0][2], (uint8_t)'T');
    EXPECT_EQ(saved[1][0], 0xFF);                       // data flag
    uint8_t parity = 0;
    for (uint8_t v : saved[1]) parity ^= v;
    EXPECT_EQ(parity, 0);                               // the checksum byte

    MachineDescriptor* m = make(spectrum::Model::S48);
    run(m, 3500000 * 3);
    auto* tape = find<spectrum::SpectrumTape>(m);
    ASSERT(tape->load(tapOf(saved), false));
    auto* kbd = find<IKeyboardMatrix>(m);
    kbd->enqueueText("LOAD \"\"\nRUN\n");
    run(m, 3500000 * 3);
    const std::string text = screenText(m);
    if (text.find("84") == std::string::npos) printf("%s", text.c_str());
    EXPECT_TRUE(text.find("84") != std::string::npos);
    EXPECT_EQ(tape->blockIndex(), (size_t)2);
    delete m;
}

// The same tape played as pulses (the trap off while it plays): the ROM's
// own loader reads EAR, so contention, the ULA port and the edges all work
TEST_CASE(spectrum48_tape_pulses_load) {
    auto saved = saveProgram("10 PRINT 6*7");
    if (saved.empty()) return;
    MachineDescriptor* m = make(spectrum::Model::S48);
    run(m, 3500000 * 3);
    auto* tape = find<spectrum::SpectrumTape>(m);
    ASSERT(tape->load(tapOf(saved), false));
    tape->controlTape("play");
    auto* kbd = find<IKeyboardMatrix>(m);
    kbd->enqueueText("LOAD \"\"\n");
    run(m, 3500000 * 16);                               // pilot, header, pilot, data at 1500 baud
    kbd->enqueueText("RUN\n");
    run(m, 3500000 * 2);
    const std::string text = screenText(m);
    if (text.find("42") == std::string::npos) printf("%s", text.c_str());
    EXPECT_TRUE(text.find("42") != std::string::npos);
    delete m;
}

// A .tzx with the same blocks as standard-speed (0x10) blocks, a text
// block (skipped) and a turbo block (0x11) after: parsed, loaded by the trap
TEST_CASE(spectrum_tzx_blocks) {
    auto saved = saveProgram("10 PRINT 7*11");
    if (saved.empty()) return;
    std::vector<uint8_t> tzx = {'Z', 'X', 'T', 'a', 'p', 'e', '!', 0x1A, 1, 20};
    const char* note = "made by test";
    tzx.push_back(0x30);
    tzx.push_back((uint8_t)std::strlen(note));
    tzx.insert(tzx.end(), note, note + std::strlen(note));
    for (auto& b : saved) {
        tzx.push_back(0x10);
        tzx.push_back(0xE8); tzx.push_back(0x03);       // 1000 ms
        tzx.push_back((uint8_t)b.size()); tzx.push_back((uint8_t)(b.size() >> 8));
        tzx.insert(tzx.end(), b.begin(), b.end());
    }
    MachineDescriptor* m = make(spectrum::Model::S48);
    run(m, 3500000 * 3);
    auto* tape = find<spectrum::SpectrumTape>(m);
    ASSERT(tape->load(tzx, true));
    EXPECT_EQ(tape->blockCount(), (size_t)2);
    find<IKeyboardMatrix>(m)->enqueueText("LOAD \"\"\nRUN\n");
    run(m, 3500000 * 3);
    EXPECT_TRUE(screenText(m).find("77") != std::string::npos);
    delete m;
}

// .sna (48K): the registers, RAM, border; PC from the stack
TEST_CASE(spectrum48_sna_snapshot) {
    MachineDescriptor* m = make(spectrum::Model::S48);
    if (!m) return;
    std::vector<uint8_t> sna(27 + 0xC000, 0);
    // A program at $8000: LD A,2: OUT ($FE),A: LD ($9000),A: JR $
    const uint8_t code[] = {0x3E, 0x02, 0xD3, 0xFE, 0x32, 0x00, 0x90, 0x18, 0xFE};
    std::memcpy(&sna[27 + 0x4000], code, sizeof code);
    sna[23] = 0xFE; sna[24] = 0xFF;                     // SP $FFFE: the PC there
    sna[27 + 0xBFFE] = 0x00; sna[27 + 0xBFFF] = 0x80;   // PC $8000
    sna[25] = 1;                                        // IM 1
    sna[26] = 5;                                        // border cyan
    sna[21] = 0x00; sna[22] = 0x12;                     // AF $1200
    ASSERT(spectrum::loadSnapshot(m, sna, false));
    auto* cpu = dynamic_cast<Z80*>(m->cpus[0].cpu);
    EXPECT_EQ(cpu->pc(), 0x8000u);
    EXPECT_EQ(cpu->sp(), 0x0000u);
    run(m, 70000);
    EXPECT_EQ(m->buses[0].bus->peek8(0x9000), 2);
    delete m;
}

// .z80 version 1, compressed (ED ED nn bb runs), and version 3 for the 128K
// (pages 3-10 are RAM banks 0-7)
TEST_CASE(spectrum_z80_snapshots) {
    MachineDescriptor* m = make(spectrum::Model::S48);
    if (!m) return;
    std::vector<uint8_t> z(30, 0);
    z[6] = 0x00; z[7] = 0x80;                           // PC $8000 (version 1)
    z[8] = 0x00; z[9] = 0xF0;                           // SP
    z[12] = 0x20 | (4 << 1);                            // compressed, border green
    z[29] = 1;
    // RAM: $4000-$7FFF zero, $8000: the program, the rest zero
    std::vector<uint8_t> body = {0xED, 0xED, 0xFF, 0x00};          // runs of zeros...
    std::vector<uint8_t> ram;
    for (int i = 0; i < 0x4000 / 255; ++i) ram.insert(ram.end(), body.begin(), body.end());
    ram.push_back(0xED); ram.push_back(0xED); ram.push_back((uint8_t)(0x4000 - (0x4000 / 255) * 255)); ram.push_back(0);
    const uint8_t code[] = {0x3E, 0x07, 0x32, 0x00, 0x90, 0x18, 0xFE};
    ram.insert(ram.end(), code, code + sizeof code);
    ram.insert(ram.end(), {0x00, 0xED, 0xED, 0x00});   // the end marker
    z.insert(z.end(), ram.begin(), ram.end());
    ASSERT(spectrum::loadSnapshot(m, z, true));
    run(m, 70000);
    EXPECT_EQ(m->buses[0].bus->peek8(0x9000), 7);
    delete m;

    // Version 3, 128K: bank 7 (page 10) holds a marker, $7FFD pages it in
    m = make(spectrum::Model::S128);
    std::vector<uint8_t> v3(30 + 2 + 54, 0);
    v3[8] = 0x00; v3[9] = 0x80;                         // SP $8000
    v3[30] = 54;                                        // version 3
    v3[32] = 0x00; v3[33] = 0x80;                       // PC $8000
    v3[34] = 4;                                         // 128K
    v3[35] = 0x17;                                      // $7FFD: bank 7 at $C000, ROM 1
    auto page = [&](int p, const std::vector<uint8_t>& data) {
        v3.push_back(0xFF); v3.push_back(0xFF); v3.push_back((uint8_t)p);
        v3.insert(v3.end(), data.begin(), data.end());
    };
    std::vector<uint8_t> blank(0x4000, 0), bank2(0x4000, 0), bank7(0x4000, 0);
    // $8000 (bank 2): LD A,($C123): LD ($9000),A: JR $
    const uint8_t prog[] = {0x3A, 0x23, 0xC1, 0x32, 0x00, 0x90, 0x18, 0xFE};
    std::memcpy(bank2.data(), prog, sizeof prog);
    bank7[0x123] = 0x5A;
    for (int p = 3; p <= 10; ++p) page(p, p == 5 ? bank2 : p == 10 ? bank7 : blank);
    ASSERT(spectrum::loadSnapshot(m, v3, true));
    run(m, 70000);
    EXPECT_EQ(m->buses[0].bus->peek8(0x9000), 0x5A);
    // A 128K snapshot does not go into a 48K
    MachineDescriptor* m48 = make(spectrum::Model::S48);
    std::string err;
    EXPECT_TRUE(!spectrum::loadSnapshot(m48, v3, true, &err));
    delete m48;
    delete m;
}
