#include "test_harness.h"
#include "m100_test_rom.h"
#include "plugins/8085/main/cpu8085.h"
#include "plugins/devices/tpdd/main/tpdd.h"
#include "libdevices/main/io_registry.h"
#include "libdevices/main/ivideo_output.h"
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

using namespace m100test;
using m100::Model;

namespace {

m100::System* sysOf(MachineDescriptor* m) { return m100::systemOf(m); }
IBus* io(MachineDescriptor* m) { return m->buses[1].bus; }

/** True if the 5x7 glyph of `c` is drawn with its top-left dot at (x, y). */
bool glyphAt(m100::System* s, int x, int y, char c) {
    const std::vector<uint8_t> panel = s->isT200() ? s->lcdc.render(240, 128) : s->lcd.render();
    for (int r = 0; r < 7; ++r)
        for (int b = 0; b < 5; ++b) {
            const bool want = (Hd61830::FONT_5X7[c - 0x20][r] >> b) & 1;
            if ((panel[(size_t)(y + r) * 240 + x + b] != 0) != want) return false;
        }
    return true;
}

int litDots(m100::System* s) {
    const std::vector<uint8_t> panel = s->isT200() ? s->lcdc.render(240, 128) : s->lcd.render();
    int n = 0;
    for (uint8_t d : panel) n += d;
    return n;
}

IVideoOutput* video(MachineDescriptor* m) {
    std::vector<IOHandler*> hs;
    m->ioRegistry->enumerate(hs);
    for (auto* h : hs)
        if (auto* v = dynamic_cast<IVideoOutput*>(h)) return v;
    return nullptr;
}

IOHandler* handler(MachineDescriptor* m, const std::string& name) {
    std::vector<IOHandler*> hs;
    m->ioRegistry->enumerate(hs);
    for (auto* h : hs)
        if (name == h->name()) return h;
    return nullptr;
}

struct TempDir {
    std::filesystem::path path;
    explicit TempDir(const std::string& tag) {
        path = std::filesystem::temp_directory_path() / ("mmsim_m100_" + tag + "_" + std::to_string(::getpid()));
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

// The 81C55 set up as the ROMs do: ports A and B out, the timer at `count` in continuous square wave
std::string timerSetup(int count) {
    char b[160];
    std::snprintf(b, sizeof b,
                  "MVI A,$%02X\nOUT $B4\nMVI A,$%02X\nOUT $B5\nMVI A,$C3\nOUT $B0\n",
                  count & 0xFF, 0x40 | ((count >> 8) & 0x3F));
    return b;
}

}  // namespace

// Without a ROM: each model is made with a stand-in that writes the message on its LCD
TEST_CASE(m100_missing_rom_message) {
    const char* old = std::getenv("MMEMU_M100_FIRMWARE");
    std::string keep = old ? old : "";
    setenv("MMEMU_M100_FIRMWARE", "/nonexistent-m100-roms", 1);
    std::string e100, e102, e200;
    MachineDescriptor* m = m100::create(Model::M100, &e100);
    MachineDescriptor* m2 = m100::create(Model::M102, &e102);
    MachineDescriptor* m3 = m100::create(Model::M200, &e200);
    if (old) setenv("MMEMU_M100_FIRMWARE", keep.c_str(), 1); else unsetenv("MMEMU_M100_FIRMWARE");
    ASSERT(m && m2 && m3);
    ASSERT(e100.find("m100.rom") != std::string::npos);
    ASSERT(e100.find("MMEMU_M100_FIRMWARE") != std::string::npos);
    ASSERT(e102.find("m102.rom") != std::string::npos);
    ASSERT(e200.find("t200.rom") != std::string::npos);
    ASSERT(!m->warnings.empty());
    for (auto* x : {m, m2, m3}) {
        runFor(x, 0.2);
        auto* s = sysOf(x);
        ASSERT(s->cpu()->isHalted());
        // "NO ROM": N at (0, 4), O at (6, 4), R at (18, 4)
        ASSERT(glyphAt(s, 0, 4, 'N'));
        ASSERT(glyphAt(s, 6, 4, 'O'));
        ASSERT(glyphAt(s, 18, 4, 'R'));
        ASSERT(litDots(s) > 200);
        // The frame: the dots doubled on the greenish-grey panel
        IVideoOutput* v = video(x);
        ASSERT(v != nullptr);
        auto d = v->getDimensions();
        ASSERT_EQ(d.width, 240 * 2 + 16);
        ASSERT_EQ(d.height, s->lcdHeight() * 2 + 16);
        std::vector<uint32_t> px((size_t)d.width * d.height);
        v->renderFrame(px.data());
        ASSERT_EQ(px[0], m100::Display::BG);
        // N's top-left dot: panel (0, 4) -> frame (8, 16)
        ASSERT_EQ(px[(size_t)16 * d.width + 8], m100::Display::FG);
        ASSERT_EQ(px[(size_t)17 * d.width + 9], m100::Display::FG);
    }
    ASSERT_EQ(sysOf(m3)->lcdHeight(), 128);
    delete m;
    delete m2;
    delete m3;
}

// The stand-in through the frontends' paths: machineId, the RAM slot, the option ROM socket
TEST_CASE(m100_descriptor) {
    MachineDescriptor* m = make(Model::M100, idleRom(Model::M100));
    ASSERT_EQ(m->machineId, std::string("model100"));
    ASSERT_EQ((int)m->clockHz, 2457600);
    ASSERT(m->romSocket("optrom") != nullptr);
    ASSERT_EQ(sysOf(m)->ramKb(), 32);
    std::string err;
    ASSERT(m->expansionSlots[0].install("ram", "8K", err));
    ASSERT_EQ(sysOf(m)->ramKb(), 8);
    ASSERT(!m->expansionSlots[0].install("ram", "12K", err));
    ASSERT(handler(m, "RS-232") && handler(m, "Printer") && handler(m, "Tape") && handler(m, "Keyboard"));
    MachineDescriptor* m2 = make(Model::M102, idleRom(Model::M102));
    ASSERT(!m2->expansionSlots[0].install("ram", "8K", err));
    ASSERT(m2->expansionSlots[0].install("ram", "24K", err));
    ASSERT_EQ(sysOf(m2)->ramKb(), 24);
    MachineDescriptor* m3 = make(Model::M200, idleRom(Model::M200));
    ASSERT_EQ(sysOf(m3)->ramKb(), 72);
    ASSERT(m3->expansionSlots[0].install("ram", "48K", err));
    delete m;
    delete m2;
    delete m3;
}

// Model 100: ROM $0000-$7FFF (read only), RAM top-down to $FFFF, nothing below it
TEST_CASE(m100_memory_map) {
    auto rom = idleRom(Model::M100);
    rom[0x1234] = 0x5A;
    MachineDescriptor* m = make(Model::M100, rom, 8);
    IBus* bus = m->buses[0].bus;
    ASSERT_EQ(bus->read8(0x1234), 0x5A);
    bus->write8(0x1234, 0);
    ASSERT_EQ(bus->read8(0x1234), 0x5A);
    bus->write8(0xE000, 0x11);
    bus->write8(0xFFFF, 0x22);
    ASSERT_EQ(bus->read8(0xE000), 0x11);
    ASSERT_EQ(bus->read8(0xFFFF), 0x22);
    bus->write8(0xDFFF, 0x33);
    ASSERT_EQ(bus->read8(0xDFFF), 0xFF);       // 8K: from $E000
    ASSERT_EQ(bus->read8(0x8000), 0xFF);
    delete m;
    m = make(Model::M100, rom, 32);
    bus = m->buses[0].bus;
    bus->write8(0x8000, 0x44);
    ASSERT_EQ(bus->read8(0x8000), 0x44);
    delete m;
}

// Port $E0 bit 0 swaps the option ROM in at $0000-$7FFF (code in RAM switching, as CALL 63012's stub does)
TEST_CASE(m100_option_rom_switching) {
    auto rom = idleRom(Model::M100);
    rom[0x0010] = 0xAA;
    std::vector<uint8_t> opt(0x2000, 0x00);
    opt[0x0010] = 0x55;
    opt[0x1FFF] = 0x77;
    MachineDescriptor* m = make(Model::M100, rom, 32, opt);
    auto* s = sysOf(m);
    ASSERT(s->hasOptionRom());
    runInRam(m, 0xF000,
             "MVI A,$01\nOUT $E0\nLDA $0010\nSTA $F100\nLDA $1FFF\nSTA $F102\nLDA $2000\nSTA $F103\n"
             "XRA A\nOUT $E0\nLDA $0010\nSTA $F101\nHLT\n");
    run(m, 2000);
    IBus* bus = m->buses[0].bus;
    ASSERT_EQ(bus->read8(0xF100), 0x55);
    ASSERT_EQ(bus->read8(0xF101), 0xAA);
    ASSERT_EQ(bus->read8(0xF102), 0x77);
    ASSERT_EQ(bus->read8(0xF103), 0xFF);        // an 8 KB ROM: the rest of the socket reads $FF
    ASSERT_EQ(s->romBank(), 0);
    // The socket emptied: selecting it reads $FF; through the frontends' socket too
    std::string err;
    ASSERT(m->romSocket("optrom")->fit({}, "", err));
    ASSERT(!s->hasOptionRom());
    io(m)->write8(0xE0, 0x01);
    ASSERT_EQ(bus->read8(0x0010), 0xFF);
    ASSERT(!m->romSocket("optrom")->fit(std::vector<uint8_t>(0x8001, 0), "big", err));
    ASSERT(m->romSocket("optrom")->fit(std::vector<uint8_t>(0x8000, 0x12), "full", err));
    ASSERT_EQ(bus->read8(0x7FFF), 0x12);
    ASSERT_EQ(m->romSocket("optrom")->info(), std::string("full"));
    delete m;
}

// The keyboard: columns strobed low on ports A and B, the rows read active low at $E0
TEST_CASE(m100_keyboard_scan) {
    MachineDescriptor* m = make(Model::M100, idleRom(Model::M100));
    auto* s = sysOf(m);
    ASSERT(m->onKey("z", true));                // column 0 bit 0
    ASSERT(m->onKey("l", true));                // column 0 bit 7
    ASSERT(m->onKey("a", true));                // column 1 bit 0
    ASSERT(m->onKey("left_shift", true));       // column 8 bit 0
    ASSERT(m->onKey("f8", true));               // column 7 bit 7
    ASSERT(!m->onKey("nosuchkey", true));
    runInRam(m, 0xF000,
             "MVI A,$03\nOUT $B0\n"
             "MVI A,$FE\nOUT $B1\nMVI A,$EF\nOUT $B2\nIN $E0\nSTA $F100\n"      // column 0 (port B bit 4 low: not power-off)
             "MVI A,$FD\nOUT $B1\nIN $E0\nSTA $F101\n"                           // column 1
             "MVI A,$7F\nOUT $B1\nIN $E0\nSTA $F102\n"                           // column 7
             "MVI A,$FF\nOUT $B1\nMVI A,$EE\nOUT $B2\nIN $E0\nSTA $F103\n"      // column 8
             "MVI A,$EF\nOUT $B2\nIN $E0\nSTA $F104\n"                           // none
             "XRA A\nOUT $B1\nIN $E0\nSTA $F105\nHLT\n");                        // 0-7 together
    run(m, 4000);
    IBus* bus = m->buses[0].bus;
    ASSERT_EQ(bus->read8(0xF100), 0x7E);
    ASSERT_EQ(bus->read8(0xF101), 0xFE);
    ASSERT_EQ(bus->read8(0xF102), 0x7F);
    ASSERT_EQ(bus->read8(0xF103), 0xFE);
    ASSERT_EQ(bus->read8(0xF104), 0xFF);
    ASSERT_EQ(bus->read8(0xF105), 0x7E);
    ASSERT_EQ(s->keyStrobe(), 0x100);
    // Typed text: upper case holds SHIFT, then lets go
    m->onKey("z", false); m->onKey("l", false); m->onKey("a", false);
    m->onKey("left_shift", false); m->onKey("f8", false);
    s->keyboard.enqueueText("Q");
    runFor(m, 0.03);
    ASSERT_EQ(s->keyboard.read(0x1FB), 0xFE);   // column 2: Q
    ASSERT_EQ(s->keyboard.read(0x0FF), 0xFE);   // SHIFT
    runFor(m, 0.2);
    ASSERT(!s->keyboard.typing());
    ASSERT_EQ(s->keyboard.read(0x000), 0xFF);
    // CAPS LOCK locks
    m->onKey("caps_lock", true); m->onKey("caps_lock", false);
    ASSERT_EQ(s->keyboard.read(0x0FF), 0xDF);
    m->onKey("caps_lock", true); m->onKey("caps_lock", false);
    ASSERT_EQ(s->keyboard.read(0x0FF), 0xFF);
    delete m;
}

// RST 7.5 from the uPD1990AC's TP: the ROM's 256 Hz tick (C = 5 strobed by $E0 bit 2)
TEST_CASE(m100_rst75_from_rtc) {
    const std::string src =
        "JMP start_\n"
        "ORG 003C\n"
        "JMP tick_\n"
        "ORG 0040\n"
        "start_: LXI SP,$0000\n"
        "MVI A,$03\nOUT $B0\n"
        "MVI A,$05\nOUT $B1\n"            // C0-C2 = 5: TP 256 Hz
        "MVI A,$04\nOUT $E0\nXRA A\nOUT $E0\n"
        "LXI H,$0000\nSHLD $F100\n"
        "MVI A,$0B\nSIM\nEI\n"            // RST 7.5 unmasked
        "idle_: HLT\nJMP idle_\n"
        "tick_: PUSH H\nLHLD $F100\nINX H\nSHLD $F100\nPOP H\nEI\nRET\n";
    MachineDescriptor* m = make(Model::M100, romFrom(src));
    runFor(m, 0.5);
    IBus* bus = m->buses[0].bus;
    const int n = bus->read8(0xF100) | bus->read8(0xF101) << 8;
    ASSERT(n >= 120 && n <= 136);
    delete m;
    // The Model 200's RST 7.5: a 150 Hz square wave from the clock
    const std::string src200 =
        "JMP start_\nORG 003C\nJMP tick_\nORG 0040\n"
        "start_: LXI SP,$0000\nLXI H,$0000\nSHLD $F100\nMVI A,$0B\nSIM\nEI\n"
        "idle_: HLT\nJMP idle_\n"
        "tick_: PUSH H\nLHLD $F100\nINX H\nSHLD $F100\nPOP H\nEI\nRET\n";
    m = make(Model::M200, romFrom(src200, 0x12000));
    runFor(m, 1.0);
    bus = m->buses[0].bus;
    const int n2 = bus->read8(0xF100) | bus->read8(0xF101) << 8;
    ASSERT(n2 >= 148 && n2 <= 152);
    delete m;
}

// The speaker: the 81C55's TIMER OUT while port B bit 2 is low and bit 5 high; bit 5 alone with bit 2 high
TEST_CASE(m100_speaker_timer) {
    auto crossings = [](m100::System* s) {
        int n = 0;
        const auto& v = s->sound.samples();
        for (size_t i = 1; i < v.size(); ++i) n += (v[i - 1] < 0) != (v[i] < 0);
        return n;
    };
    // 2458 clocks: 1000 Hz
    MachineDescriptor* m = make(Model::M100, romFrom(timerSetup(2458) + "MVI A,$20\nOUT $B2\nDI\nHLT\n"));
    auto* s = sysOf(m);
    runFor(m, 0.2);
    ASSERT(s->pio.timerRunning());
    const int c = crossings(s);
    ASSERT(c >= 380 && c <= 420);               // two a cycle
    delete m;
    // Bit 2 high: the speaker follows bit 5 (a steady level: silence after the DC blocker)
    m = make(Model::M100, romFrom(timerSetup(2458) + "MVI A,$24\nOUT $B2\nDI\nHLT\n"));
    s = sysOf(m);
    runFor(m, 0.2);
    ASSERT(crossings(s) < 4);
    ASSERT(s->speaker());
    delete m;
    // The Model 200 the same way
    m = make(Model::M200, romFrom(timerSetup(1229) + "MVI A,$20\nOUT $B2\nDI\nHLT\n", 0x12000));
    s = sysOf(m);
    runFor(m, 0.2);
    const int c2 = crossings(s);
    ASSERT(c2 >= 760 && c2 <= 840);             // 2000 Hz
    delete m;
}

// RS-232 through the IM6402: `serial loopback`, the 81C55 timer at 8 (19200 baud), RST 6.5 on DR
TEST_CASE(m100_uart_loopback) {
    const std::string src =
        "JMP start_\nORG 0034\nJMP rx_\nORG 0040\n"
        "start_: LXI SP,$0000\nLXI H,$F100\nSHLD $F0F0\n" + timerSetup(8) +
        "MVI A,$1C\nOUT $D0\n"                  // 8 bits, no parity, 1 stop
        "MVI A,$0D\nSIM\nEI\n"                  // RST 6.5 unmasked
        "MVI A,$41\nOUT $C0\n"
        "wait_: IN $D0\nANI $10\nJZ wait_\n"
        "MVI A,$42\nOUT $C0\n"
        "idle_: HLT\nJMP idle_\n"
        "rx_: PUSH PSW\nPUSH H\nIN $C0\nLHLD $F0F0\nMOV M,A\nINX H\nSHLD $F0F0\nPOP H\nPOP PSW\nEI\nRET\n";
    MachineDescriptor* m = make(Model::M100, romFrom(src));
    ASSERT(handler(m, "RS-232")->attachSerial("loopback"));
    ASSERT(!handler(m, "RS-232")->attachSerial("printer:/dev/null"));
    runFor(m, 0.05);
    IBus* bus = m->buses[0].bus;
    ASSERT_EQ(bus->read8(0xF100), 0x41);
    ASSERT_EQ(bus->read8(0xF101), 0x42);
    ASSERT_EQ(bus->read8(0xF0F0), 0x02);
    ASSERT_EQ(sysOf(m)->uart.sent.size(), 2u);
    delete m;
}

// A TPDD directory read through `serial tpdd:DIR` (the TPDD protocol at 19200 baud)
TEST_CASE(m100_tpdd_directory) {
    TempDir t("tpdd");
    std::ofstream(t.path / "HELLO.BA", std::ios::binary) << std::string(300, 'x');
    // "ZZ", type 0 (directory reference), 26 bytes: an empty name, 'F', 1 (first)
    std::vector<uint8_t> name(24, ' ');
    name.push_back('F');
    name.push_back(0x01);
    const auto req = Tpdd::request(0x00, name);
    std::string db = "reqdata_:";
    for (size_t i = 0; i < req.size(); ++i)
        db += (i % 8 == 0 ? (i ? "\nDB " : " DB ") : ",") + std::to_string(req[i]);
    char cnt[32];
    std::snprintf(cnt, sizeof cnt, "MVI C,%d\n", (int)req.size());
    const std::string src =
        "JMP start_\nORG 0034\nJMP rx_\nORG 0040\n"
        "start_: LXI SP,$0000\nLXI H,$F100\nSHLD $F0F0\n" + timerSetup(8) +
        "MVI A,$1C\nOUT $D0\nMVI A,$0D\nSIM\nEI\n"
        "LXI D,reqdata_\n" + std::string(cnt) +
        "send_: IN $D0\nANI $10\nJZ send_\nLDAX D\nOUT $C0\nINX D\nDCR C\nJNZ send_\n"
        "idle_: HLT\nJMP idle_\n"
        "rx_: PUSH PSW\nPUSH H\nIN $C0\nLHLD $F0F0\nMOV M,A\nINX H\nSHLD $F0F0\nPOP H\nPOP PSW\nEI\nRET\n"
        "ORG 0400\n" + db + "\n";
    MachineDescriptor* m = make(Model::M100, romFrom(src));
    ASSERT(handler(m, "RS-232")->attachSerial("tpdd:" + t.path.string()));
    runFor(m, 0.1);
    IBus* bus = m->buses[0].bus;
    const int got = (bus->read8(0xF0F0) | bus->read8(0xF0F1) << 8) - 0xF100;
    ASSERT_EQ(got, 31);
    ASSERT_EQ(bus->read8(0xF100), 0x11);
    ASSERT_EQ(bus->read8(0xF101), 28);
    std::string entry;
    for (int i = 0; i < 9; ++i) entry += (char)bus->read8((uint32_t)(0xF102 + i));
    ASSERT_EQ(entry, std::string("HELLO .BA"));
    ASSERT_EQ(bus->read8(0xF100 + 27), 0x01);    // 300 bytes
    ASSERT_EQ(bus->read8(0xF100 + 28), 0x2C);
    delete m;
}

// The printer: port A's byte printed as $E0 bit 1 falls; to a file with `serial printer:PATH`
TEST_CASE(m100_printer) {
    TempDir t("lpt");
    const std::string path = (t.path / "paper.txt").string();
    MachineDescriptor* m = make(Model::M100, romFrom(
        "MVI A,$03\nOUT $B0\nMVI A,$48\nOUT $B1\nMVI A,$02\nOUT $E0\nXRA A\nOUT $E0\n"
        "MVI A,$49\nOUT $B1\nMVI A,$02\nOUT $E0\nXRA A\nOUT $E0\nDI\nHLT\n"));
    ASSERT(handler(m, "Printer")->attachSerial("printer:" + path));
    run(m, 1000);
    ASSERT_EQ(std::string(sysOf(m)->printer.printed.begin(), sysOf(m)->printer.printed.end()), std::string("HI"));
    delete m;
    std::ifstream f(path);
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    ASSERT_EQ(s, std::string("HI"));
}

// Model 200: the ROM banks at $0000-$7FFF (main, second, option, none), $8000-$9FFF fixed,
// the 24 KB RAM banks at $A000-$FFFF; the switching code runs from the fixed ROM
TEST_CASE(m100_t200_banks) {
    std::vector<uint8_t> rom(0x12000, 0xFF);
    rom[0x0010] = 0x01;                          // main ROM
    rom[0xA010] = 0x02;                          // second ROM ($0010 of bank 1)
    const std::string src =
        "JMP $8000\n"
        "ORG 8000\n"
        "LXI SP,$0000\n"
        "MVI A,$01\nOUT $D0\nLDA $0010\nSTA $A000\n"     // ROM bank 1, RAM bank 0
        "MVI A,$02\nOUT $D0\nLDA $0010\nSTA $A001\n"     // the option ROM
        "MVI A,$03\nOUT $D0\nLDA $0010\nSTA $A002\n"     // nothing
        "XRA A\nOUT $D0\nLDA $0010\nSTA $A003\n"
        "MVI A,$04\nOUT $D0\nMVI A,$B1\nSTA $A000\n"    // RAM bank 1
        "MVI A,$08\nOUT $D0\nMVI A,$B2\nSTA $A000\n"    // RAM bank 2
        "IN $D0\nSTA $A001\n"
        "XRA A\nOUT $D0\nHLT\n";
    assemble(rom, 0, src);
    std::vector<uint8_t> opt(0x8000, 0);
    opt[0x0010] = 0x03;
    MachineDescriptor* m = make(Model::M200, rom, 72, opt);
    auto* s = sysOf(m);
    run(m, 4000);
    ASSERT(s->cpu()->isHalted());
    IBus* bus = m->buses[0].bus;
    ASSERT_EQ(bus->read8(0xA000), 0x02);
    ASSERT_EQ(bus->read8(0xA001), 0x03);
    ASSERT_EQ(bus->read8(0xA002), 0xFF);
    ASSERT_EQ(bus->read8(0xA003), 0x01);
    ASSERT_EQ(bus->read8(0x8000), rom[0x8000]);
    io(m)->write8(0xD0, 0x04);
    ASSERT_EQ(bus->read8(0xA000), 0xB1);
    io(m)->write8(0xD0, 0x08);
    ASSERT_EQ(bus->read8(0xA000), 0xB2);
    ASSERT_EQ(bus->read8(0xA001), 0x08);         // the bank register read back
    ASSERT_EQ(s->ramBank(), 2);
    io(m)->write8(0xD0, 0x0C);              // bank 3: nothing
    ASSERT_EQ(bus->read8(0xA000), 0xFF);
    delete m;
    // 24 KB: banks 1 and 2 are empty
    m = make(Model::M200, rom, 24);
    io(m)->write8(0xD0, 0x04);
    m->buses[0].bus->write8(0xB000, 0x12);
    ASSERT_EQ(m->buses[0].bus->read8(0xB000), 0xFF);
    delete m;
}

// Model 200 RS-232 through the 8251: mode, command, RxRDY -> RST 6.5, loopback
TEST_CASE(m100_t200_usart_loopback) {
    const std::string src =
        "JMP start_\nORG 0034\nJMP rx_\nORG 0040\n"
        "start_: LXI SP,$0000\nLXI H,$F100\nSHLD $F0F0\n" + timerSetup(8) +
        "MVI A,$4E\nOUT $C1\nMVI A,$37\nOUT $C1\n"   // async x16 8N1; TxEN DTR RxE ER RTS
        "MVI A,$0D\nSIM\nEI\n"
        "MVI A,$54\nOUT $C0\n"
        "wait_: IN $C1\nANI $01\nJZ wait_\n"
        "MVI A,$32\nOUT $C0\n"
        "idle_: HLT\nJMP idle_\n"
        "rx_: PUSH PSW\nPUSH H\nIN $C0\nLHLD $F0F0\nMOV M,A\nINX H\nSHLD $F0F0\nPOP H\nPOP PSW\nEI\nRET\n";
    MachineDescriptor* m = make(Model::M200, romFrom(src, 0x12000));
    ASSERT(handler(m, "RS-232")->attachSerial("loopback"));
    runFor(m, 0.05);
    IBus* bus = m->buses[0].bus;
    ASSERT_EQ(bus->read8(0xF100), 0x54);
    ASSERT_EQ(bus->read8(0xF101), 0x32);
    ASSERT_EQ(sysOf(m)->usart.sent.size(), 2u);
    delete m;
}

// The RTCs from the ports: the Model 200's RP5C01 at $90-$9F; the Model 100's uPD1990AC read by shifting
TEST_CASE(m100_rtc_ports) {
    MachineDescriptor* m = make(Model::M200, idleRom(Model::M200));
    auto* s = sysOf(m);
    for (int r = 0; r < 13; ++r) ASSERT_EQ(io(m)->read8((uint32_t)(0x90 + r)), s->rtc200.read(r));
    delete m;
    // Model 100: C = 3 (time read) then C = 1 (shift) strobed, as the ROM reads the clock;
    // DATA OUT on port C bit 0 is the seconds' bit 0
    m = make(Model::M100, idleRom(Model::M100));
    s = sysOf(m);
    io(m)->write8(0xB0, 0x03);
    io(m)->write8(0xB1, 0x03);
    io(m)->write8(0xE0, 0x04);
    io(m)->write8(0xE0, 0x00);
    io(m)->write8(0xB1, 0x01);
    io(m)->write8(0xE0, 0x04);
    io(m)->write8(0xE0, 0x00);
    const uint8_t sec = s->rtc.shiftRegister()[0];
    ASSERT_EQ(io(m)->read8(0xB3) & 1, sec & 1);
    // One CLK pulse (port A bit 3) shifts the next bit out
    io(m)->write8(0xB1, 0x09);
    io(m)->write8(0xB1, 0x01);
    ASSERT_EQ(io(m)->read8(0xB3) & 1, (sec >> 1) & 1);
    delete m;
}

// The power switch: TRAP, then off after 50 ms; on again restarts with RAM kept.
// Port B bit 4 (auto power-off) cuts the power at once.
TEST_CASE(m100_power) {
    const std::string src =
        "JMP start_\nORG 0024\nJMP trap_\nORG 0040\n"
        "start_: LXI SP,$0000\nLDA $F000\nINR A\nSTA $F000\n"
        "idle_: HLT\nJMP idle_\n"
        "trap_: MVI A,$55\nSTA $F001\nMVI A,$03\nOUT $B0\nMVI A,$10\nOUT $B2\nHLT\n";
    MachineDescriptor* m = make(Model::M100, romFrom(src));
    auto* s = sysOf(m);
    IBus* bus = m->buses[0].bus;
    run(m, 1000);
    ASSERT_EQ(bus->read8(0xF000), 1);
    ASSERT(m->onKey("power", true));
    m->onKey("power", false);
    run(m, 1000);
    ASSERT_EQ(bus->read8(0xF001), 0x55);        // the TRAP handler ran and set auto power-off
    ASSERT(!s->powered());
    m->onKey("power", true);                     // on: RAM kept, the ROM runs again
    run(m, 1000);
    ASSERT(s->powered());
    ASSERT_EQ(bus->read8(0xF000), 2);
    delete m;
    // Without the ROM powering down, the holdup ends it after 50 ms
    m = make(Model::M100, romFrom("JMP start_\nORG 0024\ntrap_: JMP trap_\nORG 0040\nstart_: JMP start_\n"));
    s = sysOf(m);
    m->onKey("power", true);
    runFor(m, 0.04);
    ASSERT(s->powered());
    runFor(m, 0.02);
    ASSERT(!s->powered());
    delete m;
}

// Snapshots: memory, the chips and the banks come back
TEST_CASE(m100_snapshot) {
    MachineDescriptor* m = make(Model::M200, idleRom(Model::M200));
    auto* s = sysOf(m);
    IBus* bus = m->buses[0].bus;
    io(m)->write8(0xD0, 0x04);
    bus->write8(0xA000, 0x99);
    io(m)->write8(0xF1, 0x00);
    io(m)->write8(0xF0, 0x32);
    std::vector<uint8_t> snap(bus->stateSize());
    bus->saveState(snap.data());
    io(m)->write8(0xD0, 0x00);
    bus->write8(0xA000, 0x11);
    io(m)->write8(0xF1, 0x00);
    io(m)->write8(0xF0, 0x00);
    bus->loadState(snap.data());
    ASSERT_EQ(s->bankRegister(), 0x04);
    ASSERT_EQ(bus->read8(0xA000), 0x99);
    ASSERT_EQ(s->lcdc.mode(), 0x32);
    delete m;
}

// The cassette: .wav only; SOD reaches the recording while the motor runs
TEST_CASE(m100_cassette) {
    TempDir t("cas");
    MachineDescriptor* m = make(Model::M100, romFrom(
        "MVI A,$08\nOUT $E0\n"                    // the motor
        "loop_: MVI A,$C0\nSIM\nMVI B,$80\nd1_: DCR B\nJNZ d1_\n"
        "MVI A,$40\nSIM\nMVI B,$80\nd2_: DCR B\nJNZ d2_\nJMP loop_\n"));
    IOHandler* tape = handler(m, "Tape");
    ASSERT(tape->startTapeRecord());
    runFor(m, 0.05);
    const std::string wav = (t.path / "out.wav").string();
    ASSERT(!tape->saveTapeRecording((t.path / "out.cas").string()));
    ASSERT(tape->saveTapeRecording(wav));
    ASSERT(std::filesystem::file_size(wav) > 500);
    ASSERT(!tape->mountTape((t.path / "out.cas").string()));
    ASSERT(tape->mountTape(wav));
    delete m;
}
