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

// The 128K's AY: tone A at period 424 (1773450 / (16 * 424) = 261.4 Hz),
// enabled in the mixer at full volume, from 128 BASIC; register 0 reads
// back through $FFFD
TEST_CASE(spectrum128_ay_tone) {
    MachineDescriptor* m = make(spectrum::Model::S128);
    if (!m) return;
    run(m, 3546900 * 3);
    auto* kbd = find<IKeyboardMatrix>(m);
    auto* audio = find<IAudioOutput>(m);
    for (const char* k : {"down", "return"}) {
        kbd->pressKeyByName(k, true);
        run(m, 3546900 / 10);
        kbd->pressKeyByName(k, false);
        run(m, 3546900 / 2);
    }
    run(m, 3546900);
    float drain[8192];
    while (audio->pullSamples(drain, 8192) > 0) {}
    // (in a program that keeps running: 128 BASIC silences the AY when a command ends)
    kbd->enqueueText("10 OUT 65533,0: OUT 49149,168: OUT 65533,1: OUT 49149,1: OUT 65533,7: OUT 49149,62: "
                     "OUT 65533,8: OUT 49149,15: OUT 65533,0: PRINT IN 65533\n20 GO TO 20\nRUN\n");
    run(m, 3546900 * 10);                               // the 128 editor takes about 30 characters a second
    while (audio->pullSamples(drain, 8192) > 0) {}
    std::vector<float> s;
    for (int i = 0; i < 30; ++i) {
        run(m, 354690);
        float buf[8192];
        int n;
        while ((n = audio->pullSamples(buf, 8192)) > 0) s.insert(s.end(), buf, buf + n);
    }
    int best = 0;
    for (size_t at = 0; at + 22050 < s.size(); at += 4410) {
        float mean = 0;
        for (size_t i = at; i < at + 22050; ++i) mean += s[i];
        mean /= 22050;
        int n = 0;
        for (size_t i = at + 1; i < at + 22050; ++i) if (s[i - 1] < mean && s[i] >= mean) ++n;
        best = std::max(best, n);
    }
    printf("    AY: %d cycles in half a second\n", best);
    EXPECT_TRUE(std::abs(best - 131) <= 3);
    const std::string text = screenText(m);
    if (text.find("168") == std::string::npos) printf("%s", text.c_str());
    EXPECT_TRUE(text.find("168") != std::string::npos);
    delete m;
}

// The SD card in SPI mode (libspectrum mmc.c): idle after CMD0; CMD8 echoes
// the check pattern; ACMD41 with HCS (after CMD8) leaves idle; CMD58's OCR
// says SDHC; CMD17 reads a block (data token $FE, 512 bytes, CRC); CMD24
// writes one to the file; CMD9's CSD gives the size.
#include "plugins/machines/spectrum/main/spi_sd_card.h"
#include "plugins/machines/spectrum/main/divmmc.h"
#include <filesystem>
#include <fstream>
TEST_CASE(spectrum_spi_sd_card) {
    const std::string path = (std::filesystem::temp_directory_path() / "mmemu_sd_test.img").string();
    {
        std::ofstream f(path, std::ios::binary);
        for (int i = 0; i < 1024 * 1024; ++i) f.put((char)((i >> 9) + i));   // 1 MB, a pattern per block
    }
    spectrum::SpiSdCard card;
    std::string err;
    ASSERT(card.insert(path, err));
    EXPECT_EQ(card.sectors(), 2048u);
    auto cmd = [&](uint8_t c, uint32_t arg) {
        card.write((uint8_t)(0x40 | c));
        for (int s = 24; s >= 0; s -= 8) card.write((uint8_t)(arg >> s));
        card.write(0x95);
        return card.read();                          // R1
    };
    EXPECT_EQ(card.read(), 0xFF);                    // nothing to say yet
    EXPECT_EQ(cmd(0, 0), 0x01);                      // idle
    EXPECT_EQ(cmd(17, 0), 0x05);                     // a read while idle: illegal
    EXPECT_EQ(cmd(8, 0x1AA), 0x01);
    EXPECT_EQ(card.read(), 0x00); EXPECT_EQ(card.read(), 0x00);
    EXPECT_EQ(card.read(), 0x01); EXPECT_EQ(card.read(), 0xAA);
    EXPECT_EQ(cmd(55, 0), 0x01);
    EXPECT_EQ(cmd(41, 0x40000000), 0x00);            // ready
    EXPECT_EQ(cmd(58, 0), 0x00);
    EXPECT_EQ(card.read(), 0xC0);                    // powered up, SDHC
    for (int i = 0; i < 3; ++i) card.read();
    EXPECT_EQ(cmd(9, 0), 0x00);
    uint8_t csd[18];
    for (auto& b : csd) b = card.read();
    EXPECT_EQ(csd[0], 0xFE);
    EXPECT_EQ(csd[1], 0x40);                         // CSD 2.0
    EXPECT_EQ((csd[9] << 8) | csd[10], 1);           // C_SIZE: (1 + 1) x 512 KB
    EXPECT_EQ(cmd(17, 3), 0x00);
    EXPECT_EQ(card.read(), 0xFE);
    bool same = true;
    for (int i = 0; i < 512; ++i) if (card.read() != (uint8_t)(3 + 3 * 512 + i)) same = false;
    EXPECT_TRUE(same);
    EXPECT_EQ(cmd(17, 4096), 0x40);                  // past the end: parameter error
    // CMD24: a block to sector 5
    EXPECT_EQ(cmd(24, 5), 0x00);
    card.write(0xFF);                                // gap before the token
    card.write(0xFE);
    for (int i = 0; i < 512; ++i) card.write((uint8_t)(0xA5 ^ i));
    card.write(0); card.write(0);
    EXPECT_EQ(card.read() & 0x1F, 0x05);             // data accepted
    EXPECT_EQ(card.blocksWritten(), 1u);
    card.eject();
    {
        std::ifstream f(path, std::ios::binary);
        f.seekg(5 * 512);
        bool written = true;
        for (int i = 0; i < 512; ++i) if ((uint8_t)f.get() != (uint8_t)(0xA5 ^ i)) written = false;
        EXPECT_TRUE(written);
    }
    std::filesystem::remove(path);
}

// The DivMMC's paging (Fuse divxxx.c, z80_ops.c): automapped after a fetch at
// $0000/$0008/$0038/$0066/$04C6/$0562, before one at $3Dxx, out after one at
// $1FF8-$1FFF; CONMEM maps it whatever; MAPRAM puts bank 3 at $0000
// read-only and can't be cleared but by power-off; the $2000 bank from $E3.
TEST_CASE(spectrum_divmmc_paging) {
    std::vector<uint8_t> eeprom(0x2000, 0xE5);
    spectrum::DivMmc d(eeprom);
    d.reset(true);
    EXPECT_TRUE(!d.paged());
    d.before(0x0038); EXPECT_TRUE(!d.paged());       // the late traps map after the fetch
    d.after(0x0038);  EXPECT_TRUE(d.paged());
    EXPECT_EQ(d.read(0x0100), 0xE5);
    d.after(0x1FFB);  EXPECT_TRUE(!d.paged());       // out after a fetch at $1FF8-$1FFF
    d.before(0x3D2F); EXPECT_TRUE(d.paged());        // $3Dxx maps before the fetch
    d.after(0x1FF8);
    d.after(0x0039);  EXPECT_TRUE(!d.paged());       // not a trap
    for (uint16_t t : {0x0000, 0x0008, 0x0066, 0x04C6, 0x0562}) {
        d.after(t); EXPECT_TRUE(d.paged()); d.after(0x1FFF);
    }
    // CONMEM: banks at $2000, writable; the EEPROM write-protected
    d.portWrite(0xE3, 0x80 | 5);
    EXPECT_TRUE(d.paged());
    d.write(0x2000, 0x55);
    d.write(0x0000, 0x00);
    EXPECT_EQ(d.read(0x2000), 0x55);
    EXPECT_EQ(d.read(0x0000), 0xE5);
    d.portWrite(0xE3, 0x80 | 3);
    d.write(0x2000, 0x33);                           // bank 3
    EXPECT_EQ(d.read(0x2000), 0x33);
    d.portWrite(0xE3, 0x80 | 5);
    EXPECT_EQ(d.read(0x2000), 0x55);
    // MAPRAM: bank 3 at $0000 (read-only) once automapped; it stays set
    d.portWrite(0xE3, 0x40 | 3);
    EXPECT_TRUE(!d.paged());
    d.after(0x0000);
    EXPECT_EQ(d.read(0x0000), 0x33);
    d.write(0x0000, 0x77); d.write(0x2000, 0x77);    // bank 3 read-only both ways
    EXPECT_EQ(d.read(0x0000), 0x33);
    d.portWrite(0xE3, 0x00);
    EXPECT_EQ(d.control() & 0x40, 0x40);             // MAPRAM can't be cleared
    d.reset(false);
    EXPECT_EQ(d.control(), 0x40);                    // nor by the reset line
    d.reset(true);
    EXPECT_EQ(d.control(), 0x00);                    // only by power-off
    // No card selected: $EB reads $FF
    uint8_t v = 0;
    EXPECT_TRUE(d.portRead(0x00EB, v));
    EXPECT_EQ(v, 0xFF);
    EXPECT_TRUE(!d.portRead(0x00E3, v));
}

// esxDOS (roms/spectrum/esxmmc.bin, local only) on a 48K with a blank card:
// it automaps at power-on, talks to the card over SPI and reads blocks,
// then hands back to the ROM, which is at $0000 again.
TEST_CASE(spectrum48_divmmc_esxdos) {
    MachineDescriptor* m = make(spectrum::Model::S48);
    if (!m) return;
    auto* slot = m->expansionSlot("edge");
    ASSERT(slot != nullptr);
    std::string err;
    EXPECT_TRUE(!slot->install("divmmc", "/no/such/card.img", err));
    const std::string path = (std::filesystem::temp_directory_path() / "mmemu_esx_test.img").string();
    {
        std::ofstream f(path, std::ios::binary);
        std::vector<char> z(1024 * 1024, 0);
        f.write(z.data(), (std::streamsize)z.size());
    }
    if (!slot->install("divmmc", path, err)) { printf("  (skipped: %s)\n", err.c_str()); delete m; std::filesystem::remove(path); return; }
    EXPECT_EQ(slot->info(), "divmmc (DivMMC, card " + path + ")");
    m->onReset(*m);
    spectrum::DivMmc* d = spectrum::divMmcOf(m);
    ASSERT(d);
    run(m, 20);
    EXPECT_TRUE(d->paged());                         // the reset vector's fetch mapped it
    run(m, 7000000);                                 // two seconds
    EXPECT_TRUE(d->card().blocksRead() > 0);         // it read the card (the MBR)
    run(m, 7000000);
    EXPECT_TRUE(!d->paged());
    EXPECT_EQ(m->buses[0].bus->peek8(0x0000), 0xF3); // the ROM's DI
    ASSERT(slot->install("", "", err));
    EXPECT_TRUE(spectrum::divMmcOf(m) == nullptr);
    delete m;
    std::filesystem::remove(path);
}
