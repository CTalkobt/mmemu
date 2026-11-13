#include "test_harness.h"
#include "trs80_test_rom.h"
#include <cstdio>
#include <fstream>
#include <string>
#include <unistd.h>

using namespace trs80test;
using trs80::Model;
using trs80::Tape;

namespace {

std::vector<uint8_t> image() {
    std::vector<uint8_t> b(16, 0x00);           // the leader
    b.push_back(0xA5);                          // the sync byte
    for (char c : std::string("HELLO, TRS-80")) b.push_back((uint8_t)c);
    b.push_back(0x3C);
    return b;
}

/** Play `from` (motor on) and record what it plays on `to`, at 1 tick = 1 us. */
void dub(Tape& from, Tape& to) {
    to.setMasterHz(1e6);
    to.startTapeRecord();
    to.setMotor(0, true);
    from.setMotor(0, true);
    int last = 0;
    for (uint64_t t = 0; t < 3000000 && from.playing(); ++t) {
        from.advance(t);
        if (from.level() != last) {
            last = from.level();
            to.output(t, last);
        }
    }
    to.setMotor(4000000, false);
    to.stopTapeRecord();
}

/** The recording matches `want` (playback adds a zero byte after the end). */
bool same(const std::vector<uint8_t>& got, const std::vector<uint8_t>& want) {
    if (got.size() < want.size()) return false;
    for (size_t i = 0; i < want.size(); ++i) if (got[i] != want[i]) return false;
    for (size_t i = want.size(); i < got.size(); ++i) if (got[i]) return false;
    return true;
}

}  // namespace

// .cas played with each speed's pulses and recorded back decodes to the same bytes
TEST_CASE(trs80_tape_cas_roundtrip) {
    const Tape::Speed speeds[] = {Tape::Speed::Baud500, Tape::Speed::Baud1500, Tape::Speed::Baud250};
    for (Tape::Speed sp : speeds) {
        Tape a, b;
        a.setMasterHz(1e6);
        a.speed = [sp]() { return sp; };
        a.loadCas(image());
        a.controlTape("play");
        dub(a, b);
        ASSERT(same(b.recordedCas(), image()));
    }
    // And through a .wav: recorded, loaded, played and recorded again
    Tape a, b, c, d;
    a.setMasterHz(1e6);
    a.loadCas(image());
    a.controlTape("play");
    dub(a, b);
    const auto wav = b.recordedWav();
    ASSERT(wav.size() > 44 && wav[0] == 'R' && wav[8] == 'W');
    c.setMasterHz(1e6);
    ASSERT(c.loadWav(wav));
    c.controlTape("play");
    dub(c, d);
    ASSERT(same(d.recordedCas(), image()));
}

// The Model I reading a tape as Level II does: a clock pulse sets the
// flip-flop at port $FF bit 7, a write to $FF clears it, a data pulse half a
// bit later is a 1
TEST_CASE(trs80_tape_m1_port_read) {
    const std::string path = "/tmp/mmsim_trs80_" + std::to_string((long)getpid()) + ".cas";
    {
        const auto b = image();
        std::ofstream f(path, std::ios::binary);
        f.write((const char*)b.data(), (std::streamsize)b.size());
    }
    MachineDescriptor* m = make(Model::M1Level2, idleRom());
    auto* sys = trs80::systemOf(m);
    ASSERT(sys->tape.mountTape(path));
    IBus* io = m->buses[1].bus;
    io->write8(0xFF, 0x04);                     // motor on
    const double hz = m->clockHz;
    auto waitFlip = [&]() {
        for (uint64_t t = 0; t < (uint64_t)(hz * 0.05); t += (uint64_t)m->schedulerStep(*m))
            if (io->read8(0xFF) & 0x80) return true;
        return false;
    };
    auto readBit = [&]() {
        if (!waitFlip()) return -1;             // the clock pulse
        io->write8(0xFF, 0x04);
        runFor(m, 0.0015);                      // the data pulse comes ~1 ms after the clock
        const int bit = (io->read8(0xFF) & 0x80) ? 1 : 0;
        io->write8(0xFF, 0x04);
        return bit;
    };
    int reg = 0, bits = 0;
    while (reg != 0xA5 && bits < 400) {
        const int bit = readBit();
        ASSERT(bit >= 0);
        reg = ((reg << 1) | bit) & 0xFF;
        ++bits;
    }
    ASSERT_EQ(reg, 0xA5);
    std::string text;
    for (int n = 0; n < 13; ++n) {
        int byte = 0;
        for (int i = 0; i < 8; ++i) byte = (byte << 1) | readBit();
        text += (char)byte;
    }
    ASSERT_EQ(text, std::string("HELLO, TRS-80"));
    io->write8(0xFF, 0x00);                     // motor off: playback stops
    ASSERT(!sys->tape.playing());
    delete m;
    std::remove(path.c_str());
}

// Model III: with the cassette interrupts enabled a .cas plays at 1500
// baud, its rises and falls clocked into the latch (bits 0, 1)
TEST_CASE(trs80_tape_m3_1500_baud) {
    MachineDescriptor* m = make(Model::M3, idleRom(0x3800));
    auto* sys = trs80::systemOf(m);
    sys->tape.loadCas(std::vector<uint8_t>(64, 0x55));
    sys->tape.controlTape("play");
    IBus* io = m->buses[1].bus;
    io->write8(0xE0, 0x03);                     // rise and fall interrupts
    io->write8(0xEC, 0x02);                     // motor on
    int rises = 0, falls = 0, highs = 0, samples = 0;
    const uint64_t end = (uint64_t)(m->clockHz * 0.0101);
    for (uint64_t t = 0; t < end; t += (uint64_t)m->schedulerStep(*m)) {
        const uint8_t latch = (uint8_t)~io->read8(0xE0);
        if (latch & 0x03) {
            rises += latch & 1;
            falls += (latch >> 1) & 1;
            const uint8_t ff = io->read8(0xFF);  // reading $FF clears them
            highs += ff & 1;
            ++samples;
        }
    }
    // 0x55: a 752 us and a 376 us cycle in turn, 17.7 cycles in 10 ms
    ASSERT(rises >= 16 && rises <= 19);
    ASSERT(falls >= 16 && falls <= 19);
    ASSERT(highs > 0 && highs < samples);
    delete m;
}
