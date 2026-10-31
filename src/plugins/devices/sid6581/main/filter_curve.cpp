#include "filter_curve.h"
#include <cmath>
#include <algorithm>

// ---------------------------------------------------------------------------
// Resonance Correction Tables (cutoff_hi × resonance_nibble → Q multiplier)
// ---------------------------------------------------------------------------
// The real 6581 Q varies nonlinearly with both cutoff frequency and resonance setting.
// At high cutoff frequencies with high Q, the peak becomes less pronounced (compression).
// At low frequencies, Q tracks more predictably. This is due to op-amp bandwidth limitations
// and feedback network behavior changing across the frequency range.

const float FilterCurve::RESONANCE_TABLE_6581[560] = {
    // Each row: 16 Q multipliers for resonance values 0-15, for a given cutoff band
    // Low cutoff region (0x00-0x0F): Q tracks relatively linearly
    1.00f, 1.02f, 1.05f, 1.08f, 1.11f, 1.14f, 1.17f, 1.20f, 1.23f, 1.26f, 1.29f, 1.31f, 1.34f, 1.36f, 1.38f, 1.40f,
    // cutoff band 0x01
    1.00f, 1.02f, 1.05f, 1.08f, 1.11f, 1.14f, 1.17f, 1.20f, 1.23f, 1.26f, 1.29f, 1.31f, 1.34f, 1.36f, 1.38f, 1.40f,
    // Mid-low cutoff: Q scaling becomes more pronounced
    1.00f, 1.03f, 1.06f, 1.10f, 1.14f, 1.18f, 1.22f, 1.26f, 1.29f, 1.32f, 1.35f, 1.38f, 1.40f, 1.42f, 1.43f, 1.45f,
    1.00f, 1.03f, 1.07f, 1.11f, 1.15f, 1.19f, 1.24f, 1.28f, 1.31f, 1.34f, 1.37f, 1.40f, 1.42f, 1.44f, 1.45f, 1.46f,
    1.00f, 1.04f, 1.08f, 1.13f, 1.17f, 1.22f, 1.26f, 1.30f, 1.34f, 1.37f, 1.40f, 1.42f, 1.44f, 1.45f, 1.47f, 1.48f,
    1.00f, 1.04f, 1.09f, 1.14f, 1.19f, 1.24f, 1.28f, 1.32f, 1.36f, 1.39f, 1.42f, 1.44f, 1.46f, 1.47f, 1.48f, 1.49f,
    1.00f, 1.05f, 1.10f, 1.15f, 1.21f, 1.26f, 1.30f, 1.34f, 1.38f, 1.41f, 1.44f, 1.46f, 1.47f, 1.48f, 1.49f, 1.50f,
    1.00f, 1.05f, 1.11f, 1.17f, 1.22f, 1.28f, 1.32f, 1.36f, 1.40f, 1.43f, 1.45f, 1.47f, 1.49f, 1.50f, 1.50f, 1.51f,
    // Mid cutoff: Q peaks start to show compression (peak less sharp)
    1.00f, 1.06f, 1.12f, 1.18f, 1.24f, 1.29f, 1.34f, 1.38f, 1.42f, 1.44f, 1.46f, 1.48f, 1.49f, 1.50f, 1.51f, 1.52f,
    1.00f, 1.06f, 1.13f, 1.19f, 1.25f, 1.31f, 1.35f, 1.39f, 1.43f, 1.46f, 1.47f, 1.49f, 1.50f, 1.51f, 1.52f, 1.52f,
    1.00f, 1.07f, 1.14f, 1.21f, 1.27f, 1.32f, 1.37f, 1.41f, 1.44f, 1.47f, 1.48f, 1.50f, 1.51f, 1.52f, 1.52f, 1.53f,
    1.00f, 1.07f, 1.15f, 1.22f, 1.28f, 1.33f, 1.38f, 1.42f, 1.45f, 1.47f, 1.49f, 1.50f, 1.51f, 1.52f, 1.53f, 1.53f,
    1.00f, 1.08f, 1.16f, 1.23f, 1.29f, 1.35f, 1.39f, 1.43f, 1.46f, 1.48f, 1.50f, 1.51f, 1.52f, 1.53f, 1.53f, 1.54f,
    1.00f, 1.08f, 1.17f, 1.24f, 1.31f, 1.36f, 1.40f, 1.44f, 1.47f, 1.49f, 1.50f, 1.52f, 1.53f, 1.53f, 1.54f, 1.54f,
    1.00f, 1.09f, 1.18f, 1.26f, 1.32f, 1.37f, 1.41f, 1.45f, 1.48f, 1.50f, 1.51f, 1.52f, 1.53f, 1.54f, 1.54f, 1.55f,
    1.00f, 1.09f, 1.19f, 1.27f, 1.33f, 1.38f, 1.42f, 1.46f, 1.49f, 1.51f, 1.52f, 1.53f, 1.54f, 1.54f, 1.55f, 1.55f,
    // Mid-high cutoff: Nonlinearity increases, Q saturation more evident
    1.00f, 1.10f, 1.20f, 1.28f, 1.35f, 1.40f, 1.43f, 1.47f, 1.50f, 1.51f, 1.53f, 1.54f, 1.54f, 1.55f, 1.55f, 1.56f,
    1.00f, 1.10f, 1.21f, 1.29f, 1.36f, 1.41f, 1.44f, 1.48f, 1.50f, 1.52f, 1.53f, 1.54f, 1.55f, 1.55f, 1.56f, 1.56f,
    1.00f, 1.11f, 1.22f, 1.31f, 1.37f, 1.42f, 1.45f, 1.48f, 1.51f, 1.52f, 1.54f, 1.55f, 1.55f, 1.56f, 1.56f, 1.57f,
    1.00f, 1.11f, 1.23f, 1.32f, 1.38f, 1.43f, 1.46f, 1.49f, 1.52f, 1.53f, 1.54f, 1.55f, 1.56f, 1.56f, 1.57f, 1.57f,
    // High cutoff: Q compression more severe, peaks roll off
    1.00f, 1.12f, 1.24f, 1.33f, 1.39f, 1.44f, 1.47f, 1.50f, 1.52f, 1.54f, 1.55f, 1.56f, 1.56f, 1.57f, 1.57f, 1.58f,
    1.00f, 1.12f, 1.25f, 1.34f, 1.40f, 1.45f, 1.48f, 1.50f, 1.53f, 1.54f, 1.55f, 1.56f, 1.57f, 1.57f, 1.58f, 1.58f,
    1.00f, 1.13f, 1.26f, 1.35f, 1.41f, 1.45f, 1.48f, 1.51f, 1.53f, 1.55f, 1.56f, 1.57f, 1.57f, 1.58f, 1.58f, 1.58f,
    1.00f, 1.13f, 1.27f, 1.36f, 1.42f, 1.46f, 1.49f, 1.52f, 1.54f, 1.55f, 1.56f, 1.57f, 1.58f, 1.58f, 1.58f, 1.59f,
    1.00f, 1.14f, 1.28f, 1.37f, 1.43f, 1.47f, 1.50f, 1.52f, 1.54f, 1.56f, 1.57f, 1.57f, 1.58f, 1.58f, 1.59f, 1.59f,
    1.00f, 1.14f, 1.29f, 1.38f, 1.44f, 1.47f, 1.50f, 1.53f, 1.55f, 1.56f, 1.57f, 1.58f, 1.58f, 1.59f, 1.59f, 1.59f,
    1.00f, 1.15f, 1.30f, 1.39f, 1.44f, 1.48f, 1.51f, 1.53f, 1.55f, 1.56f, 1.57f, 1.58f, 1.59f, 1.59f, 1.59f, 1.60f,
    1.00f, 1.15f, 1.31f, 1.40f, 1.45f, 1.49f, 1.52f, 1.54f, 1.56f, 1.57f, 1.58f, 1.59f, 1.59f, 1.59f, 1.60f, 1.60f,
    1.00f, 1.16f, 1.32f, 1.41f, 1.46f, 1.49f, 1.52f, 1.54f, 1.56f, 1.57f, 1.58f, 1.59f, 1.59f, 1.60f, 1.60f, 1.60f,
    1.00f, 1.16f, 1.33f, 1.42f, 1.47f, 1.50f, 1.53f, 1.55f, 1.56f, 1.58f, 1.59f, 1.59f, 1.60f, 1.60f, 1.60f, 1.61f,
    1.00f, 1.17f, 1.34f, 1.42f, 1.47f, 1.50f, 1.53f, 1.55f, 1.57f, 1.58f, 1.59f, 1.60f, 1.60f, 1.60f, 1.61f, 1.61f,
    1.00f, 1.17f, 1.35f, 1.43f, 1.48f, 1.51f, 1.53f, 1.55f, 1.57f, 1.58f, 1.59f, 1.60f, 1.60f, 1.61f, 1.61f, 1.61f,
    1.00f, 1.18f, 1.36f, 1.44f, 1.49f, 1.51f, 1.54f, 1.56f, 1.57f, 1.59f, 1.60f, 1.60f, 1.61f, 1.61f, 1.61f, 1.62f,
    1.00f, 1.18f, 1.37f, 1.45f, 1.49f, 1.52f, 1.54f, 1.56f, 1.58f, 1.59f, 1.60f, 1.61f, 1.61f, 1.61f, 1.62f, 1.62f,
    // Very high cutoff: Heavy saturation, Q barely increases with setting
    1.00f, 1.19f, 1.38f, 1.45f, 1.50f, 1.52f, 1.55f, 1.57f, 1.58f, 1.59f, 1.60f, 1.61f, 1.61f, 1.62f, 1.62f, 1.62f,
};

const float FilterCurve::RESONANCE_TABLE_8580[560] = {
    // 8580 has flatter Q response - less compression at high frequencies
    // More predictable Q behavior across the entire frequency range
    1.00f, 1.02f, 1.05f, 1.08f, 1.11f, 1.14f, 1.17f, 1.20f, 1.23f, 1.26f, 1.29f, 1.32f, 1.35f, 1.37f, 1.39f, 1.41f,
    1.00f, 1.02f, 1.05f, 1.08f, 1.11f, 1.14f, 1.17f, 1.20f, 1.23f, 1.26f, 1.29f, 1.32f, 1.35f, 1.37f, 1.39f, 1.41f,
    1.00f, 1.03f, 1.06f, 1.09f, 1.12f, 1.15f, 1.18f, 1.22f, 1.25f, 1.28f, 1.31f, 1.33f, 1.36f, 1.38f, 1.40f, 1.42f,
    1.00f, 1.03f, 1.06f, 1.09f, 1.12f, 1.15f, 1.19f, 1.22f, 1.25f, 1.28f, 1.31f, 1.34f, 1.36f, 1.39f, 1.41f, 1.43f,
    1.00f, 1.03f, 1.07f, 1.10f, 1.13f, 1.17f, 1.20f, 1.23f, 1.26f, 1.29f, 1.32f, 1.35f, 1.37f, 1.40f, 1.42f, 1.44f,
    1.00f, 1.04f, 1.07f, 1.10f, 1.14f, 1.17f, 1.21f, 1.24f, 1.27f, 1.30f, 1.33f, 1.36f, 1.38f, 1.41f, 1.43f, 1.45f,
    1.00f, 1.04f, 1.07f, 1.11f, 1.14f, 1.18f, 1.21f, 1.25f, 1.28f, 1.31f, 1.34f, 1.37f, 1.39f, 1.42f, 1.44f, 1.46f,
    1.00f, 1.04f, 1.08f, 1.11f, 1.15f, 1.19f, 1.22f, 1.26f, 1.29f, 1.32f, 1.35f, 1.38f, 1.40f, 1.43f, 1.45f, 1.47f,
    1.00f, 1.04f, 1.08f, 1.12f, 1.16f, 1.19f, 1.23f, 1.26f, 1.30f, 1.33f, 1.36f, 1.39f, 1.41f, 1.44f, 1.46f, 1.48f,
    1.00f, 1.05f, 1.09f, 1.12f, 1.16f, 1.20f, 1.24f, 1.27f, 1.31f, 1.34f, 1.37f, 1.40f, 1.42f, 1.45f, 1.47f, 1.49f,
    1.00f, 1.05f, 1.09f, 1.13f, 1.17f, 1.21f, 1.25f, 1.28f, 1.32f, 1.35f, 1.38f, 1.41f, 1.43f, 1.46f, 1.48f, 1.50f,
    1.00f, 1.05f, 1.10f, 1.14f, 1.18f, 1.22f, 1.26f, 1.29f, 1.33f, 1.36f, 1.39f, 1.42f, 1.44f, 1.47f, 1.49f, 1.51f,
    1.00f, 1.06f, 1.10f, 1.14f, 1.19f, 1.23f, 1.27f, 1.30f, 1.34f, 1.37f, 1.40f, 1.43f, 1.45f, 1.48f, 1.50f, 1.52f,
    1.00f, 1.06f, 1.11f, 1.15f, 1.19f, 1.24f, 1.28f, 1.31f, 1.35f, 1.38f, 1.41f, 1.44f, 1.46f, 1.49f, 1.51f, 1.53f,
    1.00f, 1.06f, 1.11f, 1.16f, 1.20f, 1.25f, 1.29f, 1.32f, 1.36f, 1.39f, 1.42f, 1.45f, 1.47f, 1.50f, 1.52f, 1.54f,
    1.00f, 1.07f, 1.12f, 1.16f, 1.21f, 1.25f, 1.30f, 1.33f, 1.37f, 1.40f, 1.43f, 1.46f, 1.48f, 1.51f, 1.53f, 1.55f,
    1.00f, 1.07f, 1.12f, 1.17f, 1.22f, 1.26f, 1.30f, 1.34f, 1.38f, 1.41f, 1.44f, 1.47f, 1.49f, 1.52f, 1.54f, 1.56f,
    1.00f, 1.07f, 1.13f, 1.18f, 1.23f, 1.27f, 1.31f, 1.35f, 1.39f, 1.42f, 1.45f, 1.48f, 1.50f, 1.53f, 1.55f, 1.57f,
    1.00f, 1.08f, 1.13f, 1.18f, 1.23f, 1.28f, 1.32f, 1.36f, 1.40f, 1.43f, 1.46f, 1.49f, 1.51f, 1.54f, 1.56f, 1.58f,
    1.00f, 1.08f, 1.14f, 1.19f, 1.24f, 1.29f, 1.33f, 1.37f, 1.41f, 1.44f, 1.47f, 1.50f, 1.52f, 1.55f, 1.57f, 1.59f,
    1.00f, 1.08f, 1.14f, 1.20f, 1.25f, 1.30f, 1.34f, 1.38f, 1.42f, 1.45f, 1.48f, 1.51f, 1.53f, 1.56f, 1.58f, 1.60f,
    1.00f, 1.09f, 1.15f, 1.21f, 1.26f, 1.31f, 1.35f, 1.39f, 1.43f, 1.46f, 1.49f, 1.52f, 1.54f, 1.57f, 1.59f, 1.61f,
    1.00f, 1.09f, 1.16f, 1.21f, 1.27f, 1.32f, 1.36f, 1.40f, 1.44f, 1.47f, 1.50f, 1.53f, 1.55f, 1.58f, 1.60f, 1.62f,
    1.00f, 1.10f, 1.16f, 1.22f, 1.28f, 1.33f, 1.37f, 1.41f, 1.45f, 1.48f, 1.51f, 1.54f, 1.56f, 1.59f, 1.61f, 1.63f,
    1.00f, 1.10f, 1.17f, 1.23f, 1.29f, 1.34f, 1.38f, 1.42f, 1.46f, 1.49f, 1.52f, 1.55f, 1.57f, 1.60f, 1.62f, 1.64f,
    1.00f, 1.11f, 1.18f, 1.24f, 1.30f, 1.35f, 1.39f, 1.43f, 1.47f, 1.50f, 1.53f, 1.56f, 1.58f, 1.61f, 1.63f, 1.65f,
    1.00f, 1.11f, 1.18f, 1.25f, 1.31f, 1.36f, 1.40f, 1.44f, 1.48f, 1.51f, 1.54f, 1.57f, 1.59f, 1.62f, 1.64f, 1.66f,
    1.00f, 1.12f, 1.19f, 1.26f, 1.32f, 1.37f, 1.41f, 1.45f, 1.49f, 1.52f, 1.55f, 1.58f, 1.60f, 1.63f, 1.65f, 1.67f,
    1.00f, 1.12f, 1.20f, 1.27f, 1.33f, 1.38f, 1.42f, 1.46f, 1.50f, 1.53f, 1.56f, 1.59f, 1.61f, 1.64f, 1.66f, 1.68f,
    1.00f, 1.13f, 1.21f, 1.28f, 1.34f, 1.39f, 1.43f, 1.47f, 1.51f, 1.54f, 1.57f, 1.60f, 1.62f, 1.65f, 1.67f, 1.69f,
    1.00f, 1.13f, 1.22f, 1.29f, 1.35f, 1.40f, 1.44f, 1.48f, 1.52f, 1.55f, 1.58f, 1.61f, 1.63f, 1.66f, 1.68f, 1.70f,
    1.00f, 1.14f, 1.23f, 1.30f, 1.36f, 1.41f, 1.45f, 1.49f, 1.53f, 1.56f, 1.59f, 1.62f, 1.64f, 1.67f, 1.69f, 1.71f,
};

// ---------------------------------------------------------------------------
// Implementation of public interface functions
// ---------------------------------------------------------------------------

float FilterCurve::getCutoffCurve(uint16_t cutoff, Variant variant) {
    // Multiplier on the Chamberlin coefficient that SID6581::synthesize() has
    // already derived from the cutoff register (so it must stay close to 1.0
    // and be defined for every register value; see #142).
    //
    // 8580: treated as linear. 6581: a smooth sag that grows toward the top of
    // the range, reaching 12% at $7FF. This is an approximation of the
    // characteristic 6581 behaviour, not measured data; measured curves are
    // the open part of #120.
    if (variant == Variant::SID_8580) return 1.0f;
    float x = (float)(cutoff & 0x7FF) / 2047.0f;
    float t = (x - 0.1f) / 0.9f;
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    float smooth = t * t * (3.0f - 2.0f * t);   // smoothstep
    return 1.0f - 0.12f * smooth;
}

float FilterCurve::getResonanceCorrection(uint16_t cutoff, uint8_t resonance, Variant variant) {
    // Each table row holds the 16 resonance values for one cutoff band, from low
    // to high cutoff. Spread the rows over the whole 11-bit cutoff range (the
    // old (cutoff >> 3) indexing ran past the table above $117 and then ignored
    // the resonance setting).
    cutoff &= 0x7FF;
    resonance &= 0x0F;
    const int rows = (variant == Variant::SID_6581) ? RESONANCE_ROWS_6581 : RESONANCE_ROWS_8580;
    int band = (int)cutoff * rows / 2048;
    const float* table = (variant == Variant::SID_6581) ? RESONANCE_TABLE_6581 : RESONANCE_TABLE_8580;
    return table[band * 16 + resonance];
}

FilterCurve::FilterCoeffs FilterCurve::applyNonlinearity(float f, float q, uint16_t cutoff,
                                                        uint8_t resonance, Variant variant) {
    // Apply cutoff curve: adjust frequency coefficient based on empirical measurements
    float fCurve = getCutoffCurve(cutoff, variant);
    float fAdjusted = f * fCurve;  // Scale by curve (0.0-1.0 typically 0.95-1.05)

    // Apply resonance correction: adjust Q based on cutoff frequency behavior
    float qMult = getResonanceCorrection(cutoff, resonance, variant);
    float qAdjusted = q * qMult;   // Q multiplier typically 1.0-1.7

    // Clamp coefficients for stability
    fAdjusted = clamp01(fAdjusted);
    qAdjusted = std::max(0.22f, qAdjusted); // Q should not drop below minimum

    return {fAdjusted, qAdjusted};
}
