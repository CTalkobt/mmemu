#pragma once

#include <cstdint>

/**
 * SID waveform output, including combined waveforms (#119).
 *
 * With one waveform selected the output is that waveform. With several, the
 * selector connects the output bits of each selected waveform: a 0 bit grounds
 * the line (an AND), and when pulse or saw+triangle is selected the lines are
 * also interconnected, so a grounded bit pulls its neighbours down too, more
 * strongly the closer they are. The result is quieter and has a different
 * timbre than the plain AND of the waveforms.
 *
 * This uses the combined-waveform "pulldown" model by Antti Lankila and
 * Leandro Nini (reSIDfp), with its parameters fitted by Monte Carlo search to
 * samplings of real chips (https://github.com/libsidplayfp/combined-waveforms):
 * 6581 R3 0486S and 8580 R5 1088, the "average" chips. For each combination
 * the 12 output bits b[i] of the raw AND are mixed:
 *
 *   pull[i] = (sum over j != i of (1 - b[j]) * w(i - j) - pulseStrength)
 *             / (sum over j != i of w(i - j))
 *   out bit i = b[i] && (1 - pull[i]) > threshold
 *
 * with the top bit scaled by topBit first, and w a distance weight (linear,
 * quadratic or exponential in the bit distance, separate scales below and
 * above). The model is evaluated once per input value into 4096-entry tables.
 *
 * Waveform select bits (control register bits 4-7): 0x10 triangle,
 * 0x20 sawtooth, 0x40 pulse, 0x80 noise.
 */
class CombinedWaveformTable {
public:
    enum class Chip { MOS6581, MOS8580 };

    /**
     * 12-bit waveform output.
     *
     * @param phase       24-bit phase accumulator
     * @param wavSelect   control register (bits 4-7 select the waveforms)
     * @param pulseWidth  12-bit pulse width
     * @param lfsr        23-bit noise LFSR
     * @param ringMsb     ring modulation: the source voice's MSB when RING is set
     * @param chip        6581 or 8580 combined-waveform behaviour
     */
    static uint16_t getCombinedOutput(uint32_t phase, uint8_t wavSelect,
                                      uint16_t pulseWidth, uint32_t lfsr,
                                      bool ringMsb, Chip chip = Chip::MOS6581);

    /**
     * Apply the combined-waveform pulldown to a raw 12-bit AND of the selected
     * waveforms. Single waveforms and combinations without pulse or
     * saw+triangle are returned unchanged. Noise does not change the model
     * (pulse+noise has its own), as in reSIDfp.
     */
    static uint16_t pulldown(uint16_t raw, uint8_t wavSelect, Chip chip);

    /** Individual waveform outputs (12-bit), for tests and inspection. */
    static void getWaveformComponents(uint32_t phase, uint16_t pulseWidth,
                                      uint32_t lfsr, bool ringMsb,
                                      uint16_t& tri, uint16_t& saw,
                                      uint16_t& pulse, uint16_t& noise);

    static uint16_t triangle(uint32_t phase, bool ringMsb);
    static uint16_t sawtooth(uint32_t phase);
    static uint16_t pulse(uint32_t phase, uint16_t pulseWidth);
    /** Noise output: LFSR bits 20,18,14,11,9,5,2,0 as output bits 11..4. */
    static uint16_t noise(uint32_t lfsr);

    /**
     * LFSR bits driven by a waveform output: noise() in reverse. With noise
     * combined with another waveform, a 0 output bit pulls the LFSR bit
     * feeding it low as the register shifts, so the noise dies away.
     */
    static uint32_t noiseWriteback(uint16_t output);
    /** LFSR bits that feed the noise output. */
    static constexpr uint32_t NOISE_TAPS = (1u << 20) | (1u << 18) | (1u << 14) | (1u << 11) |
                                           (1u << 9) | (1u << 5) | (1u << 2) | (1u << 0);
};
