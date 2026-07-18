#pragma once

#include <cstdint>

/**
 * Combined Waveforms for MOS 6581 SID
 *
 * When multiple waveforms are selected simultaneously on the 6581, the hardware
 * produces complex harmonic patterns through analog bit-mixing in the DAC stage,
 * NOT a simple AND operation as the official spec suggests.
 *
 * The mechanism: open-collector NMOS transistor architecture with asymmetric
 * driving strength causes zero bits to "bleed" into adjacent one bits. The output
 * amplitude is typically 30-40% lower than AND operation, with characteristic
 * tonal qualities (bell-like, nasal, metallic, etc.) depending on combination.
 *
 * This implementation uses lookup tables based on empirical measurements from
 * real 6581 hardware (reverse-engineered via Kevtris/libsidplayfp research).
 *
 * References:
 * - libsidplayfp: WaveformGenerator.h with combined waveform models
 * - INSIDIOUS 6581 Blog: bit-mixing physics in open-collector DAC
 * - webSID: Hermit's algorithm for combined waveform approximation
 */

class CombinedWaveformTable {
public:
    /**
     * Generate combined waveform output for up to 4 active waveforms.
     *
     * @param phase       24-bit phase accumulator (0 - 0xFFFFFF)
     * @param wavSelect   Waveform bits: bit0=noise, bit1=pulse, bit2=saw, bit3=tri
     * @param pulseWidth  12-bit pulse width (0 - 0xFFF)
     * @param lfsr        23-bit noise LFSR state
     * @param ringMsb     Ring modulation bit from previous voice
     *
     * @return 12-bit combined waveform output (0 - 0xFFF)
     */
    static uint16_t getCombinedOutput(uint32_t phase, uint8_t wavSelect,
                                       uint16_t pulseWidth, uint32_t lfsr,
                                       bool ringMsb);

    /**
     * Get individual waveform outputs for debugging/comparison.
     * (Used for testing and visualization)
     */
    static void getWaveformComponents(uint32_t phase, uint16_t pulseWidth,
                                       uint32_t lfsr, bool ringMsb,
                                       uint16_t& tri, uint16_t& saw,
                                       uint16_t& pulse, uint16_t& noise);

private:
    // Lookup tables for combined waveforms (indexed by phase accumulator)
    // Each table captures the specific harmonic pattern for that combination
    // derived from real 6581 hardware measurements (defined in combined_waveforms.cpp)

    // Triangle + Pulse: bell-like/plucking tone
    static const uint16_t TRI_PULSE[4096];

    // Sawtooth + Pulse: hollow/nasal tone
    static const uint16_t SAW_PULSE[4096];

    // Triangle + Sawtooth: warm/rounded tone
    static const uint16_t TRI_SAW[4096];

    // Triangle + Sawtooth + Pulse: complex tone
    static const uint16_t TRI_SAW_PULSE[4096];

    // Helper: Generate triangle waveform (12-bit)
    static uint16_t triangle(uint32_t phase, bool ringMsb);

    // Helper: Generate sawtooth waveform (12-bit)
    static uint16_t sawtooth(uint32_t phase);

    // Helper: Generate pulse waveform (12-bit)
    static uint16_t pulse(uint32_t phase, uint16_t pulseWidth);

    // Helper: Generate noise waveform (12-bit from 23-bit LFSR)
    static uint16_t noise(uint32_t lfsr);
};
