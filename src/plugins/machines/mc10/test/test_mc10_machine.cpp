// TRS-80 MC-10 and Matra Alice (#333), with hand-assembled 6803 stand-ins
// for Micro Color BASIC (the real ROM is copyright and not in the
// repository): the memory map, the keyboard matrix, the VDG through the
// $BFFF latch, a stand-in that boots and prints, the cassette both ways,
// the serial printer, the expansion port and snapshots.

#include "test_harness.h"
#include "plugins/machines/mc10/main/machine_mc10.h"
#include "plugins/machines/mc10/main/mc10_system.h"
#include "plugins/6801/main/cpu6801.h"
#include "plugins/6801/main/asm6801.h"
#include "libcore/main/machine_desc.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

/**
 * A two-pass assembler over the 6803 line assembler: "name:" defines a
 * label, L_name uses one. Returns the 8 KB ROM for $E000 with the reset
 * vector at `org`.
 */
std::vector<uint8_t> romFrom(const std::vector<std::string>& lines, uint16_t org = 0xE000) {
    Assembler6801 as;
    std::map<std::string, uint16_t> labels;
    std::vector<uint8_t> rom(0x2000, 0x01);
    for (int pass = 0; pass < 2; ++pass) {
        uint16_t pc = org;
        for (std::string line : lines) {
            const auto colon = line.find(':');
            if (colon != std::string::npos && line.find(' ') > colon) {
                labels[line.substr(0, colon)] = pc;
                line = line.substr(colon + 1);
            }
            // Labels by value (a far placeholder in the first pass keeps sizes)
            for (size_t p; (p = line.find("L_")) != std::string::npos;) {
                size_t e = p + 2;
                while (e < line.size() && (std::isalnum((unsigned char)line[e]) || line[e] == '_')) ++e;
                const std::string name = line.substr(p + 2, e - p - 2);
                char v[8];
                const uint16_t at = pass == 0 ? (uint16_t)(pc + 2) : labels.at(name);
                std::snprintf(v, sizeof v, "$%04X", at);
                line.replace(p, e - p, v);
            }
            uint8_t buf[64];
            const int n = as.assembleLine(line, buf, sizeof buf, pc);
            if (n < 0) { std::fprintf(stderr, "mc10 test asm: '%s'\n", line.c_str()); std::abort(); }
            for (int i = 0; i < n; ++i) rom[(size_t)(pc - 0xE000 + i) & 0x1FFF] = buf[i];
            pc = (uint16_t)(pc + n);
        }
    }
    rom[0x1FFE] = (uint8_t)(org >> 8);
    rom[0x1FFF] = (uint8_t)org;
    return rom;
}

struct Machine {
    std::unique_ptr<MachineDescriptor> desc;
    mc10::System* sys = nullptr;
    M6801* cpu = nullptr;
    explicit Machine(const std::vector<uint8_t>& rom, mc10::Model model = mc10::Model::MC10,
                     mc10::Expansion exp = mc10::Expansion::RamPack16K, std::vector<uint8_t> mcx = {}) {
        mc10::Roms roms;
        roms.basic = rom;
        roms.mcx128 = std::move(mcx);
        std::string err;
        desc.reset(mc10::create(model, &err, exp, &roms));
        sys = mc10::systemOf(desc.get());
        cpu = sys ? sys->cpu() : nullptr;
    }
    void run(uint64_t cycles) {
        const uint64_t end = sys->cycles() + cycles;
        while (sys->cycles() < end) sys->step();
    }
};

// Screen codes for text: upper case and digits, not inverted
std::vector<uint8_t> screenText(const std::string& s) {
    std::vector<uint8_t> v;
    for (char c : s) v.push_back((uint8_t)((c & 0x3F) | 0x40));
    return v;
}

const std::vector<std::string> SPIN = {"start: BRA L_start"};

}  // namespace

// $4000 RAM (mirrored at $6000, A12 high floats), the RAM pack at $5000-$8FFF,
// the ROM at $E000 (mirrored at $C000), the floating bus, internal RAM
TEST_CASE(mc10_memory_map) {
    Machine m(romFrom(SPIN));
    ASSERT(m.sys != nullptr);
    EXPECT_EQ(m.desc->machineId, std::string("mc10"));
    EXPECT_EQ(m.cpu->mode(), 2);
    EXPECT_EQ(m.cpu->pc(), 0xE000u);
    m.sys->write8(0x4123, 0x5A);
    EXPECT_EQ(m.sys->peek8(0x4123), 0x5A);
    EXPECT_EQ(m.sys->ram(0x123), 0x5A);
    m.sys->write8(0x5000, 0x11);
    m.sys->write8(0x8FFF, 0x22);
    EXPECT_EQ(m.sys->peek8(0x5000), 0x11);
    EXPECT_EQ(m.sys->peek8(0x8FFF), 0x22);
    EXPECT_EQ(m.sys->ram(0x000), 0x00);               // the pack, not the internal RAM
    EXPECT_EQ(m.sys->peek8(0xE000), 0x20);            // BRA
    EXPECT_EQ(m.sys->peek8(0xC000), 0x20);
    EXPECT_EQ(m.sys->peek8(0x1234), 0x34);            // nothing there: the address's low byte
    m.sys->write8(0x0090, 0x77);                      // the 6803's own RAM
    EXPECT_EQ(m.cpu->internalRam()[0x10], 0x77);
    EXPECT_EQ(m.sys->peek8(0x0090), 0x77);

    // Unexpanded: 4K, $5000 floats
    Machine b(romFrom(SPIN), mc10::Model::MC10, mc10::Expansion::None);
    b.sys->write8(0x5000, 0x11);
    EXPECT_EQ(b.sys->peek8(0x5000), 0x00);
    b.sys->write8(0x4FFF, 0x99);
    EXPECT_EQ(b.sys->peek8(0x4FFF), 0x99);
    EXPECT_EQ(b.sys->peek8(0x6FFF), 0x99);           // A13 not decoded: a mirror at $6000
    // The expansion slot: the pack in and out
    auto& slot = b.desc->expansionSlots[0];
    std::string err;
    EXPECT_TRUE(slot.install("ram16k", "", err));
    b.sys->write8(0x7000, 0x42);
    EXPECT_EQ(b.sys->peek8(0x7000), 0x42);
    EXPECT_EQ(slot.info().substr(0, 6), std::string("ram16k"));
    EXPECT_TRUE(slot.install("", "", err));
    EXPECT_TRUE(!b.sys->cart());
}

// Port 1 strobes columns low; $BFFF bits 0-5 are rows 0-5; P21 is row 6
// (CONTROL, BREAK, SHIFT); typing; the Alice's AZERTY positions
TEST_CASE(mc10_keyboard) {
    Machine m(romFrom({
        "start: LDS #$00FF",
        "LDAA #$FF",
        "STAA $00",
        "loop: LDAA $4100",
        "STAA $02",
        "LDAA $BFFF",
        "STAA $4101",
        "LDAA $03",
        "STAA $4102",
        "BRA L_loop",
    }));
    auto scan = [&](uint8_t strobe) {
        m.sys->setRam(0x100, strobe);
        m.run(200);
        return std::make_pair(m.sys->ram(0x101), m.sys->ram(0x102));
    };
    EXPECT_EQ(scan(0x00).first & 0x3F, 0x3F);        // nothing pressed
    m.sys->keyboard.key("a", true);                   // row 0, column 1
    EXPECT_EQ(scan(0xFD).first & 0x3F, 0x3E);
    EXPECT_EQ(scan(0xFB).first & 0x3F, 0x3F);         // another column
    m.sys->keyboard.key("a", false);
    m.sys->keyboard.key("return", true);              // row 3, column 6
    EXPECT_EQ(scan(0xBF).first & 0x3F, 0x37);
    m.sys->keyboard.key("return", false);
    m.sys->keyboard.key("shift", true);               // row 6, column 7: P21
    EXPECT_EQ(scan(0x7F).second & 0x02, 0);
    EXPECT_EQ(scan(0xFE).second & 0x02, 0x02);
    m.sys->keyboard.key("shift", false);
    m.sys->keyboard.key("break", true);               // row 6, column 2
    EXPECT_EQ(scan(0xFB).second & 0x02, 0);
    m.sys->keyboard.key("break", false);
    m.sys->keyboard.key("left", true);                // CONTROL + A
    EXPECT_EQ(scan(0xFE).second & 0x02, 0);
    EXPECT_EQ(scan(0xFD).first & 0x01, 0);
    m.sys->keyboard.key("left", false);
    // Typing: '+' is SHIFT and ; (row 5, column 3)
    m.sys->keyboard.enqueueText("+");
    m.run(2000);
    EXPECT_EQ(scan(0xF7).first & 0x20, 0);
    EXPECT_EQ(scan(0x7F).second & 0x02, 0);
    m.run(200000);
    EXPECT_TRUE(!m.sys->keyboard.typing());
    EXPECT_EQ(scan(0x00).first & 0x3F, 0x3F);

    // The Alice: A where the MC-10's Q is (row 2, column 1), M where ; is
    Machine a(romFrom({"start: LDS #$00FF", "LDAA #$FF", "STAA $00", "loop: LDAA $4100", "STAA $02",
                       "LDAA $BFFF", "STAA $4101", "BRA L_loop"}), mc10::Model::Alice);
    EXPECT_EQ(a.desc->machineId, std::string("alice"));
    a.sys->keyboard.key("a", true);
    a.sys->setRam(0x100, 0xFD);
    a.run(200);
    EXPECT_EQ(a.sys->ram(0x101) & 0x3F, 0x3B);
    a.sys->keyboard.key("a", false);
    a.sys->keyboard.key("m", true);
    a.sys->setRam(0x100, 0xF7);
    a.run(200);
    EXPECT_EQ(a.sys->ram(0x101) & 0x3F, 0x1F);
}

// The latch at $BFFF: VDG mode pins (A/G, GM2-0, CSS), GM2 as INT/EXT, the sound bit;
// alphanumerics and the semigraphics drawn from the internal RAM; the Alice's 312 lines
TEST_CASE(mc10_vdg_latch) {
    Machine m(romFrom({"start: LDS #$00FF", "loop: LDAA $4100", "STAA $BFFF", "BRA L_loop"}));
    auto latch = [&](uint8_t v) { m.sys->setRam(0x100, v); m.run(100); };
    latch(0x00);
    EXPECT_EQ(m.sys->vdg.mode(), 0x00);
    latch(0x20 | 0x04 | 0x10);                        // A/G, GM2, GM0: RG6-ish (CG/RG modes by GM)
    EXPECT_EQ(m.sys->vdg.mode(), 0x80 | 0x40 | 0x10);
    latch(0x40 | 0x08);                               // CSS, GM1
    EXPECT_EQ(m.sys->vdg.mode(), 0x08 | 0x20);
    latch(0x80);                                      // the sound bit
    EXPECT_EQ(m.sys->latch(), 0x80);
    EXPECT_TRUE(m.sys->soundLevel() > 0.2f);
    latch(0x00);

    // Text: a normal space, an inverse space, 'H'
    for (int i = 0; i < 512; ++i) m.sys->setRam((uint16_t)i, 0x60);
    m.sys->setRam(1, 0x20);
    m.sys->setRam(2, 0x48);
    m.run(2 * 15000);
    const uint8_t normal = m.sys->vdg.activeDot(3, 5), inverse = m.sys->vdg.activeDot(8 + 3, 5);
    EXPECT_TRUE(normal != inverse);
    bool two = false;
    for (int y = 0; y < 12; ++y)
        for (int x = 16; x < 24; ++x)
            if (m.sys->vdg.activeDot(x, y) != m.sys->vdg.activeDot(16, 0)) two = true;
    EXPECT_TRUE(two);
    // Semigraphics 4 (D7 set: A/S): $BF is all four blocks lit in red
    m.sys->setRam(3, 0xBF);
    m.run(2 * 15000);
    EXPECT_EQ(m.sys->vdg.activeDot(24, 0), m.sys->vdg.activeDot(31, 11));
    EXPECT_TRUE(m.sys->vdg.activeDot(24, 0) != normal);

    Machine a(romFrom(SPIN), mc10::Model::Alice);
    EXPECT_TRUE(a.sys->vdg.isPal());
    EXPECT_EQ(a.sys->vdg.frameLines(), 312);
}

// A stand-in BASIC: sets the ports up, clears the screen, prints
TEST_CASE(mc10_boot_prints) {
    std::vector<std::string> src = {
        "start: LDS #$00FF",
        "LDAA #$FF", "STAA $00",                     // port 1: the keyboard's columns out
        "LDAA #$01", "STAA $01",                     // port 2: P20 out
        "CLR $BFFF",
        "LDX #$4000", "LDAA #$60",
        "fill: STAA 0,X", "INX", "CPX #$4200", "BNE L_fill",
        "LDX #L_msg", "STX $82", "LDX #$4000", "STX $80",
        "copy: LDX $82", "LDAA 0,X", "BEQ L_done", "INX", "STX $82",
        "LDX $80", "STAA 0,X", "INX", "STX $80", "BRA L_copy",
        "done: BRA L_done",
    };
    std::string fcb = "msg: FCB ";
    for (uint8_t c : screenText("HELLO WORLD")) fcb += std::to_string(c) + ",";
    src.push_back(fcb + "0");
    Machine m(romFrom(src));
    m.run(20000);
    const auto want = screenText("HELLO WORLD");
    for (size_t i = 0; i < want.size(); ++i) EXPECT_EQ(m.sys->ram((uint16_t)i), want[i]);
    EXPECT_EQ(m.sys->ram(0x1FF), 0x60);
    EXPECT_EQ(m.cpu->port(1).ddr, 0xFF);
    m.run(2 * 15000);
    // The first character row has glyphs (two colours), the second only background
    bool row0 = false, row1 = false;
    for (int x = 0; x < 88; ++x) {
        for (int y = 0; y < 12; ++y) row0 |= m.sys->vdg.activeDot(x, y) != m.sys->vdg.activeDot(0, 0);
        for (int y = 12; y < 24; ++y) row1 |= m.sys->vdg.activeDot(x, y) != m.sys->vdg.activeDot(0, 12);
    }
    EXPECT_TRUE(row0);
    EXPECT_TRUE(!row1);
}

// Without the ROM: made all the same, a warning naming mc10.rom, the message on the screen
TEST_CASE(mc10_missing_rom) {
    const std::string dir = (std::filesystem::temp_directory_path() / "mmemu-mc10-empty").string();
    std::filesystem::create_directories(dir);
    const char* old = std::getenv("MMEMU_MC10_FIRMWARE");
    const std::string saved = old ? old : "";
    ::setenv("MMEMU_MC10_FIRMWARE", dir.c_str(), 1);
    std::string err;
    std::unique_ptr<MachineDescriptor> d(mc10::create(mc10::Model::MC10, &err));
    if (old) ::setenv("MMEMU_MC10_FIRMWARE", saved.c_str(), 1); else ::unsetenv("MMEMU_MC10_FIRMWARE");
    ASSERT(d != nullptr);
    EXPECT_TRUE(err.find("mc10.rom") != std::string::npos);
    ASSERT(!d->warnings.empty());
    mc10::System* sys = mc10::systemOf(d.get());
    for (int i = 0; i < 20000; ++i) sys->step();
    EXPECT_EQ(sys->ram(0), 0x4E);                     // 'N'
    EXPECT_EQ(sys->ram(1), 0x4F);                     // 'O'
}

// The cassette both ways: bytes written by the 6803 on P20 (short and long
// cycles, as BASIC writes them) decode to .c10; a .c10 played back reaches
// P24, timed by a 6803 loop with the free-running counter
TEST_CASE(mc10_cassette_round_trip) {
    const std::vector<uint8_t> payload = {0x12, 0x34, 0xAB, 0xCD};
    std::vector<uint8_t> bytes(8, 0x55);
    bytes.push_back(0x3C);
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    std::string fcb = "data: FCB ";
    for (size_t i = 0; i < bytes.size(); ++i) fcb += (i ? "," : "") + std::to_string(bytes[i]);
    Machine w(romFrom({
        "start: LDS #$00FF",
        "LDAA #$01", "STAA $01", "CLR $0003",
        "LDX #L_data",
        "nextb: LDAA 0,X", "STAA $91", "LDAA #8", "STAA $90",
        "bit: LSR $0091",
        "LDAB #43", "BCS L_one", "LDAB #81",
        "one: STAB $92",
        "LDAA #1", "STAA $03",
        "LDAB $92", "d1: DECB", "BNE L_d1",
        "CLR $0003",
        "LDAB $92", "d2: DECB", "BNE L_d2",
        "DEC $0090", "BNE L_bit",
        "INX", "CPX #L_end", "BNE L_nextb",
        "done: BRA L_done",
        fcb,
        "end: NOP",
    }));
    w.sys->tapeDeck.startTapeRecord();
    w.run(200000);
    w.sys->tapeDeck.stopTapeRecord();
    const auto cas = w.sys->tape.recordedCas();
    size_t sync = 0;
    while (sync < cas.size() && cas[sync] != 0x3C) ++sync;
    ASSERT(sync + payload.size() < cas.size());
    for (size_t i = 0; i < payload.size(); ++i) EXPECT_EQ(cas[sync + 1 + i], payload[i]);
    const std::string path = (std::filesystem::temp_directory_path() / "mmemu-mc10-test.c10").string();
    EXPECT_TRUE(w.sys->tapeDeck.saveTapeRecording(path));

    // Playback: a tape goes in paused; PLAY; the edges' times at $4400
    std::vector<uint8_t> image(16, 0x55);
    image.push_back(0x3C);
    image.insert(image.end(), payload.begin(), payload.end());
    image.push_back(0x55);                            // the last bit's second half needs an edge after it
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f.write((const char*)image.data(), (std::streamsize)image.size());
    }
    Machine r(romFrom({
        "start: LDS #$00FF",
        "LDX #$4400", "LDAA $03", "ANDA #$10", "STAA $90",
        "loop: LDAA $03", "ANDA #$10", "CMPA $90", "BEQ L_loop",
        "STAA $90", "LDD $09", "STD 0,X", "INX", "INX",
        "CPX #$4C00", "BNE L_loop",
        "done: BRA L_done",
    }));
    EXPECT_TRUE(r.sys->tapeDeck.mountTape(path));
    r.run(30000);
    EXPECT_EQ(r.cpu->x(), 0x4400);                    // paused: nothing yet
    r.sys->tapeDeck.controlTape("play");
    r.run(260000);
    const int edges = (r.cpu->x() - 0x4400) / 2;
    ASSERT(edges > 300);
    // Half-cycles in E cycles: short (a 1) under 312
    std::vector<int> cls;
    for (int i = 1; i < edges; ++i) {
        const uint16_t a = (uint16_t)(r.sys->ram((uint16_t)(0x400 + 2 * i - 2)) << 8 | r.sys->ram((uint16_t)(0x401 + 2 * i - 2)));
        const uint16_t b = (uint16_t)(r.sys->ram((uint16_t)(0x400 + 2 * i)) << 8 | r.sys->ram((uint16_t)(0x401 + 2 * i)));
        cls.push_back((uint16_t)(b - a) < 312 ? 1 : 0);
    }
    std::vector<int> bits;
    for (size_t i = 0; i + 1 < cls.size();) {
        if (cls[i] == cls[i + 1]) { bits.push_back(cls[i]); i += 2; } else ++i;
    }
    std::vector<uint8_t> got;
    for (size_t k = 0; k + 8 <= bits.size(); ++k) {
        uint8_t v = 0;
        for (int b = 0; b < 8; ++b) v |= (uint8_t)(bits[k + b] << b);
        if (v == 0x3C) {
            for (size_t j = k + 8; j + 8 <= bits.size(); j += 8) {
                uint8_t x = 0;
                for (int b = 0; b < 8; ++b) x |= (uint8_t)(bits[j + b] << b);
                got.push_back(x);
            }
            break;
        }
    }
    ASSERT(got.size() >= payload.size());
    for (size_t i = 0; i < payload.size(); ++i) EXPECT_EQ(got[i], payload[i]);
    std::remove(path.c_str());
}

// The serial port: 8N1 frames on P20 at 600 baud, bit-banged by the 6803
// as BASIC's printer driver does, captured; the TP-10's 32-column paper
TEST_CASE(mc10_serial_printer) {
    // One bit: 0.894886 MHz / 600 = 1491 E cycles; the delay loop is 5 a turn
    Machine m(romFrom({
        "start: LDS #$00FF",
        "LDAA #$01", "STAA $01", "STAA $03",          // P20 out, marking
        "LDX #L_text",
        "char: LDAA 0,X", "BEQ L_done",
        "STAA $91",
        "CLR $0003", "BSR L_wait",                    // start bit
        "LDAB #8", "STAB $90",
        "bit: LDAA $91", "ANDA #1", "STAA $03", "LSR $0091", "BSR L_wait",
        "DEC $0090", "BNE L_bit",
        "LDAA #1", "STAA $03", "BSR L_wait",          // stop bit
        "INX", "BRA L_char",
        "done: BRA L_done",
        "wait: LDAB #200",                            // 1491 cycles a bit with the loop's 34
        "w1: NOP", "DECB", "BNE L_w1",
        "LDAB #11",
        "w2: DECB", "BNE L_w2",
        "RTS",
        "text: FCB 72,73,13,0",                       // "HI", CR
    }));
    m.run(60000);
    ASSERT_EQ(m.sys->serial.sent.size(), (size_t)3);
    EXPECT_EQ(m.sys->serial.sent[0], 'H');
    EXPECT_EQ(m.sys->serial.sent[1], 'I');
    EXPECT_EQ(m.sys->serial.sent[2], 0x0D);

    // The TP-10: CR ends a line, 32 columns wrap
    mc10::SerialPort p;
    p.setTp10(true);
    const double bit = mc10::MASTER_HZ / 600;
    uint64_t t = 1000;
    auto send = [&](uint8_t v) {
        p.txLevel(t, false); t += (uint64_t)bit;
        for (int i = 0; i < 8; ++i) { p.txLevel(t, (v >> i) & 1); t += (uint64_t)bit; }
        p.txLevel(t, true); t += (uint64_t)(bit * 2);
    };
    for (int i = 0; i < 33; ++i) send('A');
    send(0x0D);
    p.txLevel(t + 100000, true);
    EXPECT_EQ(p.paper, std::string(32, 'A') + "\nA\n");
}

// The MCX-128: its EPROM at $C000 (map 0), RAM (map 3), the bank register,
// $4000-$5FFF read from its RAM with the writes reaching the internal RAM too
TEST_CASE(mc10_mcx128) {
    std::vector<uint8_t> eprom(0x4000);
    for (size_t i = 0; i < eprom.size(); ++i) eprom[i] = (uint8_t)(i >> 8);
    Machine m(romFrom(SPIN), mc10::Model::MC10, mc10::Expansion::Mcx128, eprom);
    ASSERT(m.sys->cart() != nullptr);
    EXPECT_EQ(m.sys->cart()->kind(), std::string("mcx128"));
    EXPECT_EQ(m.sys->peek8(0xC100), 0x01);            // map 0: the EPROM
    EXPECT_EQ(m.sys->peek8(0xFF00), 0x3F);
    m.sys->write8(0x4010, 0x99);
    EXPECT_EQ(m.sys->ram(0x010), 0x99);               // through to the internal RAM (the screen)
    EXPECT_EQ(m.sys->peek8(0x4010), 0x99);
    m.sys->write8(0x2000, 0x5A);                      // RAM below $4000
    EXPECT_EQ(m.sys->peek8(0x2000), 0x5A);
    m.sys->write8(0xBF00, 0x01);                      // bank 1 low
    EXPECT_EQ(m.sys->peek8(0x2000), 0x00);
    m.sys->write8(0xBF00, 0x00);
    EXPECT_EQ(m.sys->peek8(0x2000), 0x5A);
    m.sys->write8(0xBF01, 0x03);                      // map 3: RAM at $C000
    m.sys->write8(0xC100, 0x42);
    EXPECT_EQ(m.sys->peek8(0xC100), 0x42);
    m.sys->write8(0xBF01, 0x02);                      // map 2: the MC-10's ROM at $E000
    EXPECT_EQ(m.sys->peek8(0xE000), 0x20);
    EXPECT_EQ(m.sys->peek8(0xBF01), 0x02);
}

// Snapshots: the board and the 6803 (registers, timer, internal RAM) come back
TEST_CASE(mc10_snapshot) {
    Machine m(romFrom({"start: LDS #$00FF", "LDAA #$33", "STAA $80", "loop: INC $4000", "BRA L_loop"}));
    m.run(5000);
    m.sys->write8(0x4123, 0x77);
    std::vector<uint8_t> bus(m.sys->stateSize()), cpu(m.cpu->stateSize());
    m.sys->saveState(bus.data());
    m.cpu->saveState(cpu.data());
    const uint8_t counter = m.sys->ram(0);
    const uint32_t pc = m.cpu->pc();
    const uint16_t timer = m.cpu->counter();
    m.run(5000);
    m.sys->write8(0x4123, 0);
    m.sys->loadState(bus.data());
    m.cpu->loadState(cpu.data());
    EXPECT_EQ(m.sys->ram(0), counter);
    EXPECT_EQ(m.sys->ram(0x123), 0x77);
    EXPECT_EQ(m.cpu->pc(), pc);
    EXPECT_EQ(m.cpu->counter(), timer);
    EXPECT_EQ(m.cpu->internalRam()[0], 0x33);
}
