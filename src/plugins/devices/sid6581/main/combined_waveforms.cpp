#include "combined_waveforms.h"
#include <cmath>

namespace {

enum class Distance { Linear, Quadratic, Exponential };

/** Pulldown model parameters for one waveform combination. */
struct ModelParams {
    Distance dist;
    float threshold;      // a bit stays 1 while its remaining level exceeds this
    float topBit;         // weight of bit 11 (saw combinations)
    float pulseStrength;  // extra pull when the lines are interconnected
    float distAbove;      // distance scale for pull from higher bits
    float distBelow;      // distance scale for pull from lower bits
};

// Combinations with their own model: saw+tri, pulse+tri, pulse+saw,
// pulse+saw+tri, pulse+noise.
enum Combo { TS, PT, PS, PTS, PN, COMBOS };

// Parameters fitted to samplings of real chips (reSIDfp "average" chips,
// https://github.com/libsidplayfp/combined-waveforms). Pulse+noise was not
// sampled; its values are reSIDfp's estimate.
const ModelParams PARAMS[2][COMBOS] = {
    {   // 6581 R3 0486S
        { Distance::Exponential, 0.79111582f,  1.06053483f,  0.f,         1.97922957f,   2.67848182f },
        { Distance::Linear,      0.941692829f, 1.f,          1.80072665f, 0.033124879f,  0.232303441f },
        { Distance::Linear,      1.09394681f,  1.42332006f,  3.44251633f, 0.0797301158f, 0.102444135f },
        { Distance::Linear,      1.53609717f,  0.0391017497f,1.67601228f, 1.44580793f,   1.42448807f },
        { Distance::Exponential, 0.96f,        1.f,          2.5f,        1.1f,          1.2f },
    },
    {   // 8580 R5 1088
        { Distance::Exponential, 0.853578329f, 1.09615636f,  0.f,         1.8819375f,    6.80794907f },
        { Distance::Exponential, 0.929835618f, 1.f,          1.12836814f, 1.10453653f,   1.48065746f },
        { Distance::Quadratic,   0.911938608f, 0.996440411f, 1.2278074f,  0.000117214302f, 0.18948476f },
        { Distance::Exponential, 0.938004673f, 1.04827631f,  1.21178246f, 0.915959001f,  1.42698038f },
        { Distance::Exponential, 0.95f,        1.f,          1.15f,       1.f,           1.45f },
    },
};

float weight(Distance d, float scale, int bits) {
    switch (d) {
        case Distance::Linear:    return 1.f / (1.f + bits * scale);
        case Distance::Quadratic: return 1.f / (1.f + bits * bits * scale);
        default:                  return std::pow(scale, (float)-bits);
    }
}

uint16_t evaluate(const ModelParams& p, uint16_t raw) {
    // w[12 + i - j]: weight of bit j's pull on bit i (j above i: index < 12)
    float w[25];
    w[12] = 1.f;
    for (int k = 1; k <= 12; ++k) {
        w[12 - k] = weight(p.dist, p.distAbove, k);
        w[12 + k] = weight(p.dist, p.distBelow, k);
    }
    float bit[12];
    for (int i = 0; i < 12; ++i) bit[i] = (raw >> i) & 1 ? 1.f : 0.f;
    bit[11] *= p.topBit;

    uint16_t out = 0;
    for (int i = 0; i < 12; ++i) {
        if (bit[i] <= 0.f) continue;   // a 0 bit stays 0
        float pull = 0.f, total = 0.f;
        for (int j = 0; j < 12; ++j) {
            if (j == i) continue;
            float wt = w[i - j + 12];
            pull  += (1.f - bit[j]) * wt;
            total += wt;
        }
        pull = (pull - p.pulseStrength) / total;
        if (1.f - pull > p.threshold) out |= (uint16_t)(1u << i);
    }
    return out;
}

struct Tables {
    uint16_t t[2][COMBOS][4096];
    Tables() {
        for (int chip = 0; chip < 2; ++chip)
            for (int c = 0; c < COMBOS; ++c)
                for (int v = 0; v < 4096; ++v)
                    t[chip][c][v] = evaluate(PARAMS[chip][c], (uint16_t)v);
    }
};

const Tables& tables() {
    static const Tables instance;   // built on first use (a few ms)
    return instance;
}

}  // namespace

uint16_t CombinedWaveformTable::pulldown(uint16_t raw, uint8_t wavSelect, Chip chip) {
    int combo;
    switch ((wavSelect >> 4) & 0x7) {       // tri = 1, saw = 2, pulse = 4
        case 3: combo = TS;  break;
        case 5: combo = PT;  break;
        case 6: combo = PS;  break;
        case 7: combo = PTS; break;
        case 4:
            if (!(wavSelect & 0x80)) return raw;   // pulse alone
            combo = PN;
            break;
        default: return raw;
    }
    return tables().t[chip == Chip::MOS6581 ? 0 : 1][combo][raw & 0xFFF];
}

uint16_t CombinedWaveformTable::getCombinedOutput(uint32_t phase, uint8_t wavSelect,
                                                  uint16_t pulseWidth, uint32_t lfsr,
                                                  bool ringMsb, Chip chip) {
    const bool tri = wavSelect & 0x10, saw = wavSelect & 0x20;
    const bool pls = wavSelect & 0x40, nse = wavSelect & 0x80;
    if (!(tri || saw || pls || nse)) return 0;

    // Raw value from the oscillator: saw and triangle share the accumulator
    // bits; with both selected the triangle XOR is disabled, leaving the saw
    // ANDed with itself shifted left one bit.
    uint16_t raw;
    if (tri && saw) {
        uint16_t s = sawtooth(phase);
        raw = (uint16_t)(s & (s << 1)) & 0xFFF;
    } else if (saw) {
        raw = sawtooth(phase);
    } else if (tri) {
        raw = triangle(phase, ringMsb);
    } else {
        raw = 0xFFF;
    }
    if (pls) raw &= pulse(phase, pulseWidth);
    if (nse) raw &= noise(lfsr);
    return pulldown(raw, wavSelect, chip);
}

uint16_t CombinedWaveformTable::triangle(uint32_t phase, bool ringMsb) {
    // The ramp folds when the MSB is set; with ring modulation the fold uses
    // MSB XOR the source voice's MSB (reSID), inverting the whole ramp.
    // The 12 accumulator MSBs, folded on bit 11, shifted up one: the LSB is
    // grounded when triangle is selected.
    bool fold = ((phase & 0x800000u) != 0) != ringMsb;
    uint16_t ix = (uint16_t)((phase >> 12) & 0x0FFF);
    return (uint16_t)(((fold ? ix ^ 0x0FFF : ix) << 1) & 0x0FFE);
}

uint16_t CombinedWaveformTable::sawtooth(uint32_t phase) {
    return (uint16_t)((phase >> 12) & 0x0FFF);
}

uint16_t CombinedWaveformTable::pulse(uint32_t phase, uint16_t pulseWidth) {
    return (((phase >> 12) & 0x0FFF) >= pulseWidth) ? 0x0FFFu : 0x0000u;
}

uint16_t CombinedWaveformTable::noise(uint32_t lfsr) {
    return (uint16_t)(
        (((lfsr >> 20) & 1) << 11) |
        (((lfsr >> 18) & 1) << 10) |
        (((lfsr >> 14) & 1) <<  9) |
        (((lfsr >> 11) & 1) <<  8) |
        (((lfsr >>  9) & 1) <<  7) |
        (((lfsr >>  5) & 1) <<  6) |
        (((lfsr >>  2) & 1) <<  5) |
        (((lfsr >>  0) & 1) <<  4)
    );
}

uint32_t CombinedWaveformTable::noiseWriteback(uint16_t output) {
    // Inverse of noise(): output bits 11..4 back to LFSR bits 20,18,14,11,9,5,2,0
    return (uint32_t)(
        (((output >> 11) & 1u) << 20) |
        (((output >> 10) & 1u) << 18) |
        (((output >>  9) & 1u) << 14) |
        (((output >>  8) & 1u) << 11) |
        (((output >>  7) & 1u) <<  9) |
        (((output >>  6) & 1u) <<  5) |
        (((output >>  5) & 1u) <<  2) |
        (((output >>  4) & 1u) <<  0));
}

void CombinedWaveformTable::getWaveformComponents(uint32_t phase, uint16_t pulseWidth,
                                                  uint32_t lfsr, bool ringMsb,
                                                  uint16_t& tri, uint16_t& saw,
                                                  uint16_t& pulseOut, uint16_t& noiseOut) {
    tri = triangle(phase, ringMsb);
    saw = sawtooth(phase);
    pulseOut = pulse(phase, pulseWidth);
    noiseOut = noise(lfsr);
}
