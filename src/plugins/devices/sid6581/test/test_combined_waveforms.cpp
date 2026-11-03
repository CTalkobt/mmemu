#include "../main/combined_waveforms.h"
#include "test_harness.h"

using Chip = CombinedWaveformTable::Chip;

// ===========================================================================
// Issue #119: combined waveforms (pulldown model fitted to real chips)
// ===========================================================================

/** One waveform: the plain waveform; none selected: 0. */
TEST_CASE(combined_waveforms_single_waveform_returns_base) {
    for (uint32_t phase : {0x000000u, 0x345678u, 0x800000u, 0xABCDEFu}) {
        uint16_t tri, saw, pls, nse;
        CombinedWaveformTable::getWaveformComponents(phase, 0x800, 0x5A5A5A, false, tri, saw, pls, nse);
        for (Chip chip : {Chip::MOS6581, Chip::MOS8580}) {
            ASSERT_EQ(CombinedWaveformTable::getCombinedOutput(phase, 0x10, 0x800, 0x5A5A5A, false, chip), tri);
            ASSERT_EQ(CombinedWaveformTable::getCombinedOutput(phase, 0x20, 0x800, 0x5A5A5A, false, chip), saw);
            ASSERT_EQ(CombinedWaveformTable::getCombinedOutput(phase, 0x40, 0x800, 0x5A5A5A, false, chip), pls);
            ASSERT_EQ(CombinedWaveformTable::getCombinedOutput(phase, 0x80, 0x800, 0x5A5A5A, false, chip), nse);
            ASSERT_EQ(CombinedWaveformTable::getCombinedOutput(phase, 0x00, 0x800, 0x5A5A5A, false, chip), 0);
        }
    }
}

/** Triangle: the folded ramp with the LSB grounded; noise: LFSR bits 20,18,14,11,9,5,2,0. */
TEST_CASE(combined_waveforms_base_waveform_bits) {
    ASSERT_EQ(CombinedWaveformTable::triangle(0x123000, false), 0x246);
    ASSERT_EQ(CombinedWaveformTable::triangle(0xFED000, false), (0xFED ^ 0xFFF) << 1);
    ASSERT_EQ(CombinedWaveformTable::triangle(0xFED000, false) & 1, 0);
    ASSERT_EQ(CombinedWaveformTable::noise(1u << 2), 1 << 5);   // was read from bit 3
    for (int b : {20, 18, 14, 11, 9, 5, 2, 0}) {
        uint16_t out = CombinedWaveformTable::noise(1u << b);
        ASSERT_EQ(CombinedWaveformTable::noiseWriteback(out), 1u << b);
    }
}

/** Combining only pulls bits down: every output is a subset of the plain AND. */
TEST_CASE(combined_waveforms_subset_of_and) {
    for (uint8_t sel : {0x30, 0x50, 0x60, 0x70, 0xC0, 0xD0}) {
        for (Chip chip : {Chip::MOS6581, Chip::MOS8580}) {
            for (uint32_t j = 0; j < 4096; j += 7) {
                uint32_t phase = j << 12;
                uint16_t tri, saw, pls, nse;
                CombinedWaveformTable::getWaveformComponents(phase, 0x400, 0x7FFFFF, false, tri, saw, pls, nse);
                uint16_t a = 0xFFF;
                if ((sel & 0x30) == 0x30) a &= saw & (saw << 1);
                else if (sel & 0x10) a &= tri;
                else if (sel & 0x20) a &= saw;
                if (sel & 0x40) a &= pls;
                if (sel & 0x80) a &= nse;
                uint16_t out = CombinedWaveformTable::getCombinedOutput(phase, sel, 0x400, 0x7FFFFF, false, chip);
                ASSERT_EQ(out & ~a & 0xFFF, 0);
            }
        }
    }
}

/** Pulse acts as a gate: while it is low, pulse combinations output 0. */
TEST_CASE(combined_waveforms_pulse_low_is_silent) {
    for (uint8_t sel : {0x50, 0x60, 0x70})
        for (uint32_t j = 0; j < 0x800; j += 13)    // below PW 0x800: pulse low
            ASSERT_EQ(CombinedWaveformTable::getCombinedOutput(j << 12, sel, 0x800, 0, false), 0);
}

// OSC3 samplings of a real 6581 R3 0486S (Trurl, via
// https://github.com/libsidplayfp/combined-waveforms), pulse high: the value at
// oscillator step k * 64 + 32 for k = 0..63, for $30, $50, $60 and $70.
static const uint8_t REAL_6581[4][64] = {
    {   // $30
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xE0, 0xE0, 0xF8,
    }, {   // $50
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC0,
    0xFB, 0xF2, 0xEA, 0xC0, 0xDA, 0x80, 0x80, 0x00, 0xBB, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x7B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    }, {   // $60
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x80,
    0x00, 0x00, 0x00, 0x80, 0x00, 0x80, 0x80, 0x80, 0x00, 0x80, 0x80, 0xC0, 0x80, 0xC0, 0xC0, 0xF8,
    }, {   // $70
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0xC0,
    },
};

/**
 * The 6581 model matches real-chip samplings closely; the plain AND does not.
 * (Over all 4096 steps the model mispredicts 124-1030 of 32768 bits per
 * combination, as reSIDfp reports; the AND 8665-14650.)
 */
TEST_CASE(combined_waveforms_match_real_6581) {
    const uint8_t sels[4] = {0x30, 0x50, 0x60, 0x70};
    for (int w = 0; w < 4; ++w) {
        int wrongModel = 0, wrongAnd = 0;
        for (int k = 0; k < 64; ++k) {
            uint32_t phase = (uint32_t)(k * 64 + 32) << 12;
            int model = CombinedWaveformTable::getCombinedOutput(phase, sels[w], 0, 0, false) >> 4;
            uint16_t tri, saw, pls, nse;
            CombinedWaveformTable::getWaveformComponents(phase, 0, 0, false, tri, saw, pls, nse);
            uint16_t a = (sels[w] & 0x10 ? tri : 0xFFF) & (sels[w] & 0x20 ? saw : 0xFFF);
            for (int b = 0; b < 8; ++b) {
                wrongModel += ((model ^ REAL_6581[w][k]) >> b) & 1;
                wrongAnd   += (((a >> 4) ^ REAL_6581[w][k]) >> b) & 1;
            }
        }
        // of 512 bits: model 2-17 (pulse+saw worst, as over the full sweep), AND 131-236
        ASSERT(wrongModel <= 20);
        ASSERT(wrongAnd >= 100);
    }
}

/** The 6581 and 8580 differ, e.g. pulse+saw near the top of the ramp. */
TEST_CASE(combined_waveforms_chip_models_differ) {
    int differ = 0;
    for (uint32_t j = 0xC00; j < 0x1000; ++j)
        if (CombinedWaveformTable::getCombinedOutput(j << 12, 0x60, 0, 0, false, Chip::MOS6581) !=
            CombinedWaveformTable::getCombinedOutput(j << 12, 0x60, 0, 0, false, Chip::MOS8580))
            ++differ;
    ASSERT(differ > 100);
}

// ===========================================================================
// Issue #124: hard sync, ring modulation and OSC3 (reSID behaviour)
// ===========================================================================
#include "../main/sid6581.h"

namespace {
struct SidVoiceFixture {
    SID6581 sid{"sid", 0xD400};
    SidVoiceFixture() { sid.setClockHz(985248); sid.setSampleRate(44100); sid.reset(); }
    void w(int reg, uint8_t v) { sid.ioWrite(nullptr, 0xD400 + reg, v); }
    void freq(int voice, uint16_t f) { w(voice * 7, f & 0xFF); w(voice * 7 + 1, f >> 8); }
    void ctrl(int voice, uint8_t cr) { w(voice * 7 + 4, cr); }
    uint8_t osc3() { uint8_t v = 0; sid.ioRead(nullptr, 0xD41B, &v); return v; }
};
}

/** Voice 3 synced to voice 2 restarts at voice 2's MSB rising edge, even mid-tick. */
TEST_CASE(sid_hard_sync_on_source_msb_rising) {
    SidVoiceFixture f;
    f.freq(1, 0x1000);                  // voice 2: MSB rises at cycle 0x800000/0x1000 = 2048
    f.freq(2, 0x1234);
    f.ctrl(2, 0x20 | 0x02);             // voice 3: sawtooth + SYNC
    f.sid.tick(2100);                   // one tick spanning the edge

    uint32_t synced = (0x1234u * (2100 - 2048)) & 0xFFFFFF;
    uint32_t free   = (0x1234u * 2100) & 0xFFFFFF;
    ASSERT_EQ((int)f.osc3(), (int)(synced >> 16));   // sawtooth OSC3 = accumulator bits 23..16
    ASSERT((synced >> 16) != (free >> 16));

    // No edge yet: no reset (the old check was "source phase < freq")
    SidVoiceFixture g;
    g.freq(1, 0x1000); g.freq(2, 0x1234); g.ctrl(2, 0x20 | 0x02);
    g.sid.tick(2000);
    ASSERT_EQ((int)g.osc3(), (int)(((0x1234u * 2000) & 0xFFFFFF) >> 16));
}

/** The triangle is ring-modulated only with RING set, and then the whole ramp folds. */
TEST_CASE(sid_ring_mod_needs_ring_bit_and_folds_ramp) {
    for (int ring = 0; ring < 2; ring++) {
        SidVoiceFixture f;
        f.freq(1, 0x8000);              // voice 2 (ring source): MSB set after 300 cycles
        f.freq(2, 0x1000);              // voice 3: phase 0x12C000, MSB clear
        f.ctrl(2, 0x10 | (ring ? 0x04 : 0));   // triangle (+ RING)
        f.sid.tick(300);
        uint32_t p = 0x1000u * 300;
        uint16_t tri = ring ? (uint16_t)((~p >> 11) & 0xFFF) : (uint16_t)((p >> 11) & 0xFFF);
        ASSERT_EQ((int)f.osc3(), (int)(tri >> 4));   // 0x25 plain, 0xDA ring-modulated
    }
}

/** OSC3 reads the waveform output (e.g. noise for random numbers), not the accumulator. */
TEST_CASE(sid_osc3_reads_waveform_output) {
    SidVoiceFixture f;
    f.freq(2, 0x1000);
    f.ctrl(2, 0x10);                    // triangle
    f.sid.tick(3000);                   // phase 0xBB8000: MSB set, so the ramp is folded
    uint32_t p = 0x1000u * 3000;
    ASSERT_EQ((int)f.osc3(), (int)((((~p) >> 11) & 0xFFF) >> 4));
    ASSERT((int)f.osc3() != (int)((p >> 16) & 0xFF));
}

/**
 * Noise combined with pulse while the pulse is low outputs 0, and the writeback
 * clears the LFSR bits feeding the output as it shifts: the noise locks up and
 * stays silent when switched back to noise alone (#119, as in reSID).
 */
TEST_CASE(sid_noise_writeback_locks_up_lfsr) {
    SidVoiceFixture f;
    f.freq(2, 0x8000);                  // LFSR shifts every 16 cycles (bit 19 rising)
    f.w(2 * 7 + 2, 0xFF); f.w(2 * 7 + 3, 0x0F);   // PW $FFF: pulse low
    f.ctrl(2, 0xC0);                    // noise + pulse
    for (int i = 0; i < 2000; ++i) f.sid.tick(8);
    f.ctrl(2, 0x80);                    // noise alone
    int nonZero = 0;
    for (int i = 0; i < 500; ++i) { f.sid.tick(8); if (f.osc3()) ++nonZero; }
    ASSERT_EQ(nonZero, 0);

    // Noise alone keeps running
    SidVoiceFixture g;
    g.freq(2, 0x8000);
    g.ctrl(2, 0x80);
    int changes = 0; uint8_t last = g.osc3();
    for (int i = 0; i < 500; ++i) { g.sid.tick(8); if (g.osc3() != last) ++changes; last = g.osc3(); }
    ASSERT(changes > 50);
}
