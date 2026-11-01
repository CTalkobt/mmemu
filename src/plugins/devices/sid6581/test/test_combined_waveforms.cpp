#include "../main/combined_waveforms.h"
#include "test_harness.h"

TEST_CASE(combined_waveforms_single_waveform_returns_base) {
    uint32_t phase = 0x800000;  // 50% through cycle
    uint16_t pulseWidth = 0x800;  // 50% duty cycle
    uint32_t lfsr = 0x7FFFFF;
    bool ringMsb = false;

    uint16_t tri_only = CombinedWaveformTable::getCombinedOutput(phase, 0x10, pulseWidth, lfsr, ringMsb);
    ASSERT(tri_only > 0);

    uint16_t saw_only = CombinedWaveformTable::getCombinedOutput(phase, 0x20, pulseWidth, lfsr, ringMsb);
    ASSERT(saw_only > 0);

    // At phase 0x800000, phase >> 12 = 0x800 which equals pulseWidth, so pulse output is 0x0FFF
    uint16_t pulse_only = CombinedWaveformTable::getCombinedOutput(phase, 0x40, pulseWidth, lfsr, ringMsb);
    ASSERT(pulse_only == 0x0FFF);

    uint16_t none = CombinedWaveformTable::getCombinedOutput(phase, 0x00, pulseWidth, lfsr, ringMsb);
    ASSERT(none == 0);
}

TEST_CASE(combined_waveforms_triangle_pulse) {
    uint32_t phase = 0x400000;
    uint16_t pulseWidth = 0x800;
    uint32_t lfsr = 0x7FFFFF;
    bool ringMsb = false;

    uint16_t combined = CombinedWaveformTable::getCombinedOutput(phase, 0x50, pulseWidth, lfsr, ringMsb);
    ASSERT(combined > 0);
    ASSERT(combined < 0x0FFF);
}

TEST_CASE(combined_waveforms_sawtooth_pulse) {
    uint32_t phase = 0x400000;
    uint16_t pulseWidth = 0x800;
    uint32_t lfsr = 0x7FFFFF;
    bool ringMsb = false;

    uint16_t combined = CombinedWaveformTable::getCombinedOutput(phase, 0x60, pulseWidth, lfsr, ringMsb);
    ASSERT(combined > 0);
    ASSERT(combined < 0x0FFF);
}

TEST_CASE(combined_waveforms_triangle_sawtooth) {
    uint32_t phase = 0x400000;
    uint16_t pulseWidth = 0x800;
    uint32_t lfsr = 0x7FFFFF;
    bool ringMsb = false;

    uint16_t combined = CombinedWaveformTable::getCombinedOutput(phase, 0x30, pulseWidth, lfsr, ringMsb);
    ASSERT(combined > 0);
    ASSERT(combined < 0x0FFF);
}

TEST_CASE(combined_waveforms_all_three) {
    uint32_t phase = 0x400000;
    uint16_t pulseWidth = 0x800;
    uint32_t lfsr = 0x7FFFFF;
    bool ringMsb = false;

    uint16_t combined = CombinedWaveformTable::getCombinedOutput(phase, 0x70, pulseWidth, lfsr, ringMsb);
    ASSERT(combined > 0);
    ASSERT(combined < 0x0FFF);
}

TEST_CASE(combined_waveforms_phase_sweep) {
    uint16_t pulseWidth = 0x800;
    uint32_t lfsr = 0x7FFFFF;
    bool ringMsb = false;

    uint16_t out1 = CombinedWaveformTable::getCombinedOutput(0x000000, 0x50, pulseWidth, lfsr, ringMsb);
    uint16_t out2 = CombinedWaveformTable::getCombinedOutput(0x400000, 0x50, pulseWidth, lfsr, ringMsb);
    uint16_t out3 = CombinedWaveformTable::getCombinedOutput(0x800000, 0x50, pulseWidth, lfsr, ringMsb);
    uint16_t out4 = CombinedWaveformTable::getCombinedOutput(0xC00000, 0x50, pulseWidth, lfsr, ringMsb);

    bool hasVariation = (out1 != out2) || (out2 != out3) || (out3 != out4);
    ASSERT(hasVariation);
}

TEST_CASE(combined_waveforms_noise_approximation) {
    uint32_t phase = 0x400000;
    uint16_t pulseWidth = 0x800;
    uint32_t lfsr = 0x7FFFFF;
    bool ringMsb = false;

    uint16_t noise_tri = CombinedWaveformTable::getCombinedOutput(phase, 0x90, pulseWidth, lfsr, ringMsb);
    uint16_t noise_saw = CombinedWaveformTable::getCombinedOutput(phase, 0xA0, pulseWidth, lfsr, ringMsb);

    ASSERT((noise_tri >= 0 && noise_saw >= 0));
}

TEST_CASE(combined_waveforms_consistent_output) {
    uint32_t phase = 0x555555;
    uint16_t pulseWidth = 0xAAA;
    uint32_t lfsr = 0x12345;
    bool ringMsb = true;

    uint16_t out1 = CombinedWaveformTable::getCombinedOutput(phase, 0x50, pulseWidth, lfsr, ringMsb);
    uint16_t out2 = CombinedWaveformTable::getCombinedOutput(phase, 0x50, pulseWidth, lfsr, ringMsb);

    ASSERT(out1 == out2);
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
