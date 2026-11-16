#include "test_harness.h"
#include "plugins/devices/sp0256/main/sp0256.h"
#include <cstdlib>
#include <vector>

// The SP0256 with hand-built stand-in "allophone" programs (not the AL2's
// ROM): the microsequencer's byte layout is immed4 in the low nibble, the
// opcode (as MAME numbers it) in the high one.

namespace {

// Allophone n's entry at 2n: a PAUSE of `rpt`, then RTS (a halt)
std::vector<uint8_t> pauses(int rpt) {
    std::vector<uint8_t> r(0x800, 0x00);
    for (int n = 0; n < 64; ++n) r[2 * n] = (uint8_t)(0xF0 | rpt);
    return r;
}

int samplesUntilStandby(Sp0256& sp, int limit = 100000) {
    int n = 0;
    while (!sp.sby() && n < limit) { sp.sample(); ++n; }
    return n;
}

}  // namespace

// Without its ROM every allophone halts at once: silent, the handshake still answers
TEST_CASE(sp0256_no_rom_handshake) {
    Sp0256 sp;
    int lrqChanges = 0;
    sp.onLrq = [&](bool) { ++lrqChanges; };
    sp.reset();
    ASSERT(!sp.hasRom());
    ASSERT(sp.lrq());
    ASSERT(sp.sby());
    sp.ald(0x05);
    ASSERT(!sp.lrq());
    ASSERT(!sp.sby());
    sp.ald(0x06);                   // dropped: LRQ low
    sp.sample();
    ASSERT(sp.lrq());               // taken by the sequencer
    ASSERT(sp.sby());
    ASSERT_EQ((int)sp.output(), 0);
    ASSERT_EQ(lrqChanges, 2);
}

// A PAUSE of 5: 6 periods of 64 samples, silent; then standby
TEST_CASE(sp0256_pause_timing) {
    Sp0256 sp;
    sp.setRom(pauses(5));
    sp.reset();
    sp.ald(3);
    int n = 0, loud = 0;
    while (!sp.sby() && n < 10000) { if (sp.sample() != 0) ++loud; ++n; }
    ASSERT(n >= 5 * 64 && n <= 6 * 64 + 2);
    ASSERT_EQ(loud, 0);
}

// LOADALL (mode 0): amplitude and pitch with zero coefficients give the
// impulses themselves; a second ALD waits (LRQ) until the first starts, then
// plays after it
TEST_CASE(sp0256_voiced_and_double_buffer) {
    std::vector<uint8_t> r(0x800, 0x00);
    // Allophone 1's entry holds the block itself (over 2's and later
    // entries); allophone 0 is a pause
    r[0] = 0xF2; r[1] = 0x00;       // allophone 0: PAUSE 2, RTS
    const uint8_t block[] = {0x83,  // LOADALL, repeat 3 (opcode 8 = 0001)
                             0x3F,  // AM: 31 << 1 = 62
                             50,    // PR: 50 samples a period
                             0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // B0..F5
                             0x00}; // RTS
    for (size_t i = 0; i < sizeof block; ++i) r[2 + i] = block[i];
    Sp0256 sp;
    sp.setRom(r);
    sp.reset();
    sp.ald(1);
    ASSERT_EQ((int)sp.sample(), 62 * 4);    // the first impulse at once
    ASSERT(sp.lrq());
    ASSERT(!sp.sby());
    sp.ald(0);                      // queued
    ASSERT(!sp.lrq());
    sp.ald(2);                      // dropped
    int n = 0, impulses = 0;
    for (; n < 3 * 50 + 10; ++n) if (sp.sample() == 62 * 4) ++impulses;
    ASSERT_EQ(impulses, 2);          // every 51 samples, 3 in all
    ASSERT(sp.lrq());               // allophone 0 started after it
    ASSERT(!sp.sby());
    const int rest = samplesUntilStandby(sp);
    ASSERT(rest > 0 && rest <= 3 * 64);
}
