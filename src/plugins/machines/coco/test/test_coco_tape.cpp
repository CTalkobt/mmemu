#include "test_harness.h"
#include "coco_test_rom.h"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <cstdlib>
#include <unistd.h>

using namespace cocotest;

namespace {

// A block as BASIC writes it: leader, sync, payload
std::vector<uint8_t> sampleCas() {
    std::vector<uint8_t> v(32, 0x55);
    v.push_back(0x3C);
    for (int i = 0; i < 40; ++i) v.push_back((uint8_t)(i * 7 + 3));
    v.push_back(0x55);
    return v;
}

// Play `player` into `rec`'s recorder as a square wave on the DAC
void dub(coco::Tape& player, coco::Tape& rec) {
    player.setMotor(true);
    rec.setMotor(true);
    rec.startTapeRecord();
    bool last = !player.input();
    while (player.playing()) {
        player.clock(64);
        rec.clock(64);
        if (player.input() != last) {
            last = player.input();
            rec.output(last ? 0 : 63);       // PA0 high: the low half of the cycle
        }
    }
    rec.clock(100000);
    rec.output(32);
    rec.stopTapeRecord();
}

bool containsSeq(const std::vector<uint8_t>& hay, const std::vector<uint8_t>& needle) {
    return std::search(hay.begin(), hay.end(), needle.begin(), needle.end()) != hay.end();
}

}  // namespace

// .cas playback: one cycle a bit, least significant first; 1s short
TEST_CASE(coco_cas_playback_waveform) {
    coco::Tape t;
    t.loadCas({0x01});
    t.controlTape("play");
    ASSERT(!t.playing());                 // the motor is off
    t.setMotor(true);
    ASSERT(t.playing());
    // The 50 ms lead-in, then bit 0 (a 1): a short cycle, PA0 high then low
    t.clock(14318180 / 20);
    t.clock(1);
    ASSERT(t.input());
    t.clock(coco::Tape::TICKS_BIT1 / 2);
    ASSERT(!t.input());
    t.clock(coco::Tape::TICKS_BIT1 / 2);
    ASSERT(t.input());                    // bit 1 (a 0): a long cycle
    t.clock(coco::Tape::TICKS_BIT1 / 2);
    ASSERT(t.input());
    t.clock(coco::Tape::TICKS_BIT0 / 2);
    ASSERT(!t.input());
    t.setMotor(false);                    // the motor stops the tape
    const size_t pos = t.position();
    t.clock(100000);
    ASSERT_EQ(t.position(), pos);
}

// .cas round trip: play a tape into the recorder (as the DAC would carry
// it) and decode the recording back to the same bytes
TEST_CASE(coco_cas_roundtrip) {
    const auto cas = sampleCas();
    coco::Tape player, rec;
    player.loadCas(cas);
    player.controlTape("play");
    dub(player, rec);
    const auto back = rec.recordedCas();
    std::vector<uint8_t> payload(cas.begin() + 32, cas.end());
    ASSERT(containsSeq(back, payload));
    ASSERT(back.size() >= payload.size() + 24);
    ASSERT_EQ(back.front(), 0x55);
}

// .wav: the recording saved as WAV plays back to the same bytes
TEST_CASE(coco_wav_roundtrip) {
    const auto cas = sampleCas();
    coco::Tape player, rec, player2, rec2;
    player.loadCas(cas);
    player.controlTape("play");
    dub(player, rec);
    const auto wav = rec.recordedWav();
    ASSERT(wav.size() > 44);
    ASSERT(player2.loadWav(wav));
    player2.controlTape("play");
    dub(player2, rec2);
    std::vector<uint8_t> payload(cas.begin() + 32, cas.end());
    ASSERT(containsSeq(rec2.recordedCas(), payload));
    ASSERT(!player2.loadWav({1, 2, 3}));
}

// Files: mount, save; the CoCo's motor relay (PIA1 CA2) runs the tape and
// PA0 reads it
TEST_CASE(coco_tape_on_the_machine) {
    MachineDescriptor* m = make();
    ASSERT(m);
    run(m, 100000);
    coco::System* sys = coco::systemOf(m);
    char path[] = "/tmp/coco_tape_XXXXXX.cas";
    int fd = mkstemps(path, 4);
    ASSERT(fd >= 0);
    close(fd);
    {
        const auto cas = sampleCas();
        std::ofstream f(path, std::ios::binary);
        f.write((const char*)cas.data(), (std::streamsize)cas.size());
    }
    ASSERT(sys->tape.mountTape(path));
    IBus* bus = m->buses[0].bus;
    ASSERT(!sys->tape.motor());
    bus->write8(0xFF21, 0x3C);            // CA2 high: the motor on
    ASSERT(sys->tape.motor());
    ASSERT(sys->tape.playing());
    // PA0 changes as the tape runs
    int changes = 0;
    bool last = bus->read8(0xFF20) & 1;
    for (int i = 0; i < 400; ++i) {
        run(m, 200);
        const bool now = bus->read8(0xFF20) & 1;
        if (now != last) ++changes;
        last = now;
    }
    ASSERT(changes > 20);
    bus->write8(0xFF21, 0x34);
    ASSERT(!sys->tape.motor());
    // Record from the DAC with the motor on, save .cas and .wav
    ASSERT(sys->tape.startTapeRecord());
    bus->write8(0xFF21, 0x3C);
    for (int i = 0; i < 64; ++i) {
        bus->write8(0xFF20, (i & 1) ? 0xFC : 0x00);
        run(m, 200);
    }
    sys->tape.stopTapeRecord();
    std::string wavPath = std::string(path) + ".wav";
    ASSERT(sys->tape.saveTapeRecording(wavPath));
    std::ifstream w(wavPath, std::ios::binary);
    char riff[4] = {};
    w.read(riff, 4);
    ASSERT(std::string(riff, 4) == "RIFF");
    std::remove(wavPath.c_str());
    std::remove(path);
    // The CLI's `load x.cas` goes through the loader: a tape
    delete m;
}

// The bit banger: PA1 frames (8N1) to bytes
TEST_CASE(coco_bitbanger_decodes) {
    coco::BitBanger bb;
    bb.setBaud(600);
    const double bit = 14318180.0 / 600;
    uint64_t t = 1000;
    auto sendByte = [&](uint8_t v) {
        bb.txLevel(t, false);                       // start
        for (int i = 0; i < 8; ++i) bb.txLevel(t + (uint64_t)(bit * (1 + i)), (v >> i) & 1);
        bb.txLevel(t + (uint64_t)(bit * 9), true);  // stop
        t += (uint64_t)(bit * 11);
    };
    sendByte('O');
    sendByte('K');
    bb.txLevel(t + (uint64_t)(bit * 20), true);
    ASSERT_EQ(bb.sent.size(), 2u);
    ASSERT_EQ(bb.sent[0], 'O');
    ASSERT_EQ(bb.sent[1], 'K');
    std::string err;
    ASSERT(!bb.setOption("speed", "1", err));
    ASSERT(bb.setOption("baud", "1200", err));
}
