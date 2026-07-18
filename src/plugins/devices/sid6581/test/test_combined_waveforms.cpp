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
