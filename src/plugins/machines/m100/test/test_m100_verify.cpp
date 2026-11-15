// #358: the Model 100/102/200 verification items (typing pace, port polarity,
// printer line ends, cassette .wav rate, LCD busy timing). Stand-in ROMs only.

#include "test_harness.h"
#include "m100_test_rom.h"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

using namespace m100test;
using m100::Model;

namespace {

constexpr double HZ = m100::CPU_HZ;

uint64_t ms(double t) { return (uint64_t)(t * HZ / 1000.0); }

// One keyboard scan as the ROMs make it: columns 0-7 read together
void scan(m100::Keyboard& k, int n = 1) {
    for (int i = 0; i < n; ++i) k.noteRead(0x100);
}

bool shiftDown(const m100::Keyboard& k) { return (k.read(0x0FF) & 0x01) == 0; }
bool keyDown(const m100::Keyboard& k, int col, int bit) { return (k.read((uint16_t)(0x1FF & ~(1 << col))) & (1 << bit)) == 0; }

std::string slurp(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

}  // namespace

// Typed text follows the ROM's scans: SHIFT first, the key held 3 scans, released 2
TEST_CASE(m100_typing_paced_by_scans) {
    m100::Keyboard k;
    k.enqueueText("Aa");
    k.clock(ms(1), HZ);
    ASSERT(shiftDown(k));                   // SHIFT ahead of the key
    ASSERT(!keyDown(k, 1, 0));
    scan(k, 1);
    k.clock(ms(10), HZ);
    ASSERT(!keyDown(k, 1, 0));              // one scan is not enough
    scan(k, 1);
    k.clock(ms(1), HZ);
    ASSERT(keyDown(k, 1, 0));               // A
    ASSERT(shiftDown(k));
    scan(k, 2);
    k.clock(ms(20), HZ);
    ASSERT(keyDown(k, 1, 0));               // still held: 2 scans of 3
    scan(k, 1);
    k.clock(ms(1), HZ);
    ASSERT(!keyDown(k, 1, 0));
    ASSERT(shiftDown(k));                   // SHIFT stays through the gap
    scan(k, 2);
    k.clock(ms(10), HZ);                    // the gap ends: "a" needs SHIFT up first
    k.clock(ms(1), HZ);
    ASSERT(!shiftDown(k));
    ASSERT(!keyDown(k, 1, 0));
    scan(k, 2);
    k.clock(ms(5), HZ);
    ASSERT(keyDown(k, 1, 0));               // a
    ASSERT(!shiftDown(k));
    scan(k, 3);
    k.clock(ms(10), HZ);
    scan(k, 2);
    k.clock(ms(10), HZ);
    k.clock(ms(1), HZ);
    ASSERT(!k.typing());
    ASSERT_EQ(k.read(0x000), 0xFF);
    ASSERT_EQ(k.scans(), 14u);
}

// Without scans: SHIFT 20 ms ahead, 60 ms down, 40 ms up
TEST_CASE(m100_typing_without_scans) {
    m100::Keyboard k;
    k.enqueueText("Q");
    k.clock(ms(1), HZ);
    ASSERT(shiftDown(k));
    k.clock(ms(18), HZ);
    ASSERT(!keyDown(k, 2, 0));
    k.clock(ms(3), HZ);
    ASSERT(keyDown(k, 2, 0));
    k.clock(ms(58), HZ);
    ASSERT(keyDown(k, 2, 0));
    k.clock(ms(3), HZ);
    ASSERT(!keyDown(k, 2, 0));
    ASSERT(shiftDown(k));
    k.clock(ms(40), HZ);
    ASSERT(!shiftDown(k));
    k.clock(ms(20), HZ);
    ASSERT(!k.typing());
    // A program that polls column 8 alone is not a scan
    k.noteRead(0x0FF);
    k.noteRead(0x1FE);
    ASSERT_EQ(k.scans(), 0u);
}

// The host repeats a held key: it stays down once, a shifted symbol's SHIFT counted once
TEST_CASE(m100_host_key_repeat) {
    m100::Keyboard k;
    ASSERT(k.key("dquote", true));
    ASSERT(k.key("dquote", true));
    ASSERT(k.key("dquote", true));
    ASSERT(shiftDown(k));
    ASSERT(keyDown(k, 3, 4));
    ASSERT(k.key("dquote", false));
    ASSERT(!shiftDown(k));
    ASSERT(!keyDown(k, 3, 4));
    ASSERT(k.key("dquote", false));         // an extra up changes nothing
    ASSERT(k.key("left_shift", true));
    ASSERT(k.key("question", true));
    ASSERT(k.key("question", false));
    ASSERT(shiftDown(k));                   // the real SHIFT is still held
    ASSERT(k.key("left_shift", false));
    ASSERT(!shiftDown(k));
    // CAPS LOCK toggles once per press, not per repeat
    k.key("caps_lock", true);
    k.key("caps_lock", true);
    k.key("caps_lock", false);
    ASSERT(k.capsLock());
    // CODE from the GUI's Page Up
    ASSERT(k.key("restore", true));
    ASSERT_EQ(k.read(0x0FF), 0xD7);         // CODE (bit 3) and CAPS (bit 5)
}

// Typing waits while the ROM's type-ahead count (found in the ROM) is high
TEST_CASE(m100_typing_waits_for_type_ahead) {
    std::vector<uint8_t> rom(0x8000, 0xFF);
    const uint8_t code[] = {0x3A, 0xAA, 0xFF, 0xFE, 0x02, 0xD0};   // LDA $FFAA / CPI 2 / RNC
    std::memcpy(&rom[0x70B0], code, sizeof code);
    m100::System s(Model::M100, rom);
    s.write8(0xFFAA, 12);
    s.keyboard.enqueueText("a");
    for (int i = 0; i < 100; ++i) s.keyboard.clock(ms(1), HZ);
    ASSERT(!keyDown(s.keyboard, 1, 0));
    s.write8(0xFFAA, 1);
    s.keyboard.clock(ms(1), HZ);
    ASSERT(keyDown(s.keyboard, 1, 0));
    // A stand-in without the pattern is not watched
    m100::System t(Model::M100, idleRom(Model::M100));
    t.write8(0xFFAA, 12);
    t.keyboard.enqueueText("a");
    t.keyboard.clock(ms(1), HZ);
    ASSERT(keyDown(t.keyboard, 1, 0));
}

// Port C and $D0 idle levels: a ready printer, RS-232 lines deasserted, power fine
TEST_CASE(m100_port_polarity) {
    MachineDescriptor* m = make(Model::M100, idleRom(Model::M100));
    auto* s = m100::systemOf(m);
    const uint8_t pc = s->peekPort(0xBB);
    ASSERT_EQ(pc & 0x06, 0x02);             // /BUSY high, BUSY low
    ASSERT_EQ(pc & 0x30, 0x30);             // /CTS, /DSR: nothing connected
    ASSERT(s->serial.attachSerial("loopback"));
    ASSERT_EQ(s->peekPort(0xBB) & 0x30, 0x00);   // asserted: low
    const uint8_t st = s->peekPort(0xD0);
    ASSERT_EQ(st & 0xE1, 0xE0);             // RP, +5 V, /LPS high; no carrier
    delete m;
    MachineDescriptor* m2 = make(Model::M200, idleRom(Model::M200));
    auto* t = m100::systemOf(m2);
    ASSERT_EQ(t->peekPort(0xBB) & 0x07, 0x03);   // /LPS, /BUSY high, BUSY low
    delete m2;
}

// The printer file: CR becomes CR LF (an auto-LF printer); printer-raw: keeps the bytes
TEST_CASE(m100_printer_line_ends) {
    const auto dir = std::filesystem::temp_directory_path() / ("mmsim_m100_lf_" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    m100::Printer p;
    ASSERT(p.attachSerial("printer:" + (dir / "a.txt").string()));
    ASSERT(p.autoLineFeed());
    for (int b : {0x41, 0x0D, 0x42, 0x0D, 0x0A, 0x43, 0x0A}) p.write((uint8_t)b);
    ASSERT_EQ(slurp(dir / "a.txt"), std::string("A\r\nB\r\nC\n"));
    ASSERT_EQ(p.printed.size(), 7u);
    ASSERT(p.attachSerial("printer-raw:" + (dir / "b.txt").string()));
    ASSERT(!p.autoLineFeed());
    for (int b : {0x41, 0x0D, 0x42}) p.write((uint8_t)b);
    ASSERT_EQ(slurp(dir / "b.txt"), std::string("A\rB"));
    ASSERT(p.attachSerial("printer:none"));
    // The RS-232 port does not take either
    MachineDescriptor* m = make(Model::M100, idleRom(Model::M100));
    ASSERT(!m100::systemOf(m)->serial.attachSerial("printer-raw:" + (dir / "c.txt").string()));
    delete m;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// Cassette recordings are saved at 44.1 kHz
TEST_CASE(m100_cassette_wav_rate) {
    m100::System s(Model::M100, idleRom(Model::M100));
    s.tape.setMotor(true);
    ASSERT(s.tape.startTapeRecord());
    for (int i = 0; i < 20; ++i) {
        s.tape.output(i & 1 ? 63 : 0);
        s.tape.clock(512);
    }
    const auto w = s.tape.recordedWav();
    ASSERT(w.size() > 44);
    const uint32_t rate = w[24] | w[25] << 8 | w[26] << 16 | (uint32_t)w[27] << 24;
    ASSERT_EQ(rate, 44100u);
}

// The LCD busy flags run on CPU clocks: the 200's HD61830 by MAME's counts
TEST_CASE(m100_lcd_busy_timing) {
    m100::System t(Model::M200, idleRom(Model::M200));
    t.out(0xF1, 0x0C);                      // display data write
    t.out(0xF0, 0x55);
    ASSERT_EQ(t.in(0xF1) & 0x80, 0x80);
    t.step();                               // (no CPU: 64 clocks pass)
    ASSERT_EQ(t.in(0xF1) & 0x80, 0x00);
    t.out(0xF1, 0x0F);                      // set bit: 36 controller clocks, 72 CPU clocks
    t.out(0xF0, 0x01);
    t.step();
    ASSERT_EQ(t.in(0xF1) & 0x80, 0x80);
    t.step();
    ASSERT_EQ(t.in(0xF1) & 0x80, 0x00);
    // The 100's HD44102s: ticked too (busy time 0 unless set)
    m100::System s(Model::M100, idleRom(Model::M100));
    s.lcd.setBusyTime(100);
    s.pio.writeIo(0, 0x03);                 // ports A and B out, as the ROM sets them
    s.out(0xB1, 0x01);                      // chip 0 selected
    s.out(0xF0, 0x39);                      // display on
    ASSERT_EQ(s.in(0xF0) & 0x80, 0x80);
    s.step();
    s.step();
    ASSERT_EQ(s.in(0xF0) & 0x80, 0x00);
}
