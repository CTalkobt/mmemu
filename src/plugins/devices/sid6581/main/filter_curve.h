#pragma once

#include <cstdint>
#include <cmath>

/**
 * 6581 Filter Curve Nonlinearity Modeling
 *
 * The MOS 6581 SID filter exhibits significant nonlinearity due to:
 *   - Transistor gain variation with temperature and supply voltage
 *   - Capacitor tolerance (±10% is typical for 1980s ceramic capacitors)
 *   - Op-amp slew rate limitations
 *   - Feedback network component tolerances
 *   - Self-resonance peaks in the high-frequency region
 *
 * The filter response deviates from the ideal Chamberlin model:
 *   - Cutoff frequency tracking is nonlinear (~±10-15% error)
 *   - Resonance peak amplitude varies with cutoff (empirical Q curves)
 *   - Phase response shows characteristic lag above ~8 kHz
 *
 * The 8580 (later revision) has improved linearity:
 *   - Better component tolerances
 *   - Lower self-resonance
 *   - More predictable Q behavior
 *
 * This implementation is an approximation, not measured data (see #120):
 *   - the cutoff curve is a smooth multiplier around 1.0 (6581 sags at the top
 *     of the range, 8580 is linear)
 *   - the resonance tables give a Q multiplier per cutoff band and resonance
 *
 * Application:
 *   1. Convert 11-bit cutoff register to actual frequency (with curve)
 *   2. Adjust Q coefficient based on cutoff and variant
 *   3. Apply resonance saturation effects at high Q
 */

class FilterCurve {
public:
    // SID chip variants with different filter characteristics
    enum class Variant {
        SID_6581,  // Original chip (1982-1987): high nonlinearity
        SID_8580   // Revised chip (1987+): improved linearity
    };

    /**
     * Multiplier applied to the Chamberlin frequency coefficient, which is
     * already derived from the cutoff register: close to 1.0 for every value.
     *
     * @param cutoff    11-bit cutoff register (FC_HI:FC_LO, 0-2047)
     * @param variant   Chip variant (6581 vs 8580)
     * @return          Coefficient multiplier (0.88-1.0)
     */
    static float getCutoffCurve(uint16_t cutoff, Variant variant);

    /**
     * Get the nonlinear resonance adjustment factor.
     * Real 6581 Q varies nonlinearly with both cutoff and register resonance value.
     * The effect is most pronounced at high frequencies and high Q settings.
     *
     * @param cutoff    11-bit cutoff register value
     * @param resonance 4-bit resonance nibble (0-15)
     * @param variant   Chip variant
     * @return          Q adjustment multiplier (0.8-1.2 range typical)
     */
    static float getResonanceCorrection(uint16_t cutoff, uint8_t resonance, Variant variant);

    /**
     * Apply filter nonlinearity during synthesis.
     * Wrapper that applies both cutoff and resonance curves in correct order.
     *
     * @param f         Chamberlin coefficient (from frequency calculation)
     * @param q         Chamberlin Q value (from resonance calculation)
     * @param cutoff    11-bit cutoff register for curve lookup
     * @param resonance 4-bit resonance nibble for curve lookup
     * @param variant   Chip variant
     * @return          Struct with adjusted {f, q} coefficients
     */
    struct FilterCoeffs {
        float f;  // Adjusted frequency coefficient
        float q;  // Adjusted Q/resonance factor
    };

    static FilterCoeffs applyNonlinearity(float f, float q, uint16_t cutoff,
                                         uint8_t resonance, Variant variant);

private:
    // Resonance correction factors (indexed by cutoff band + resonance nibble)
    // Shape: resonance[cutoff_band * 16 + res_nibble]
    // Range: 0.8-1.2, applied as multiplier to Q
    // Size: 35 cutoff bands * 16 resonance values = 560 entries (the 8580
    // table defines 32 bands; RESONANCE_ROWS_* say how many are used)
    static constexpr int RESONANCE_ROWS_6581 = 35;   // cutoff bands in the table
    static constexpr int RESONANCE_ROWS_8580 = 32;
    static const float RESONANCE_TABLE_6581[560];
    static const float RESONANCE_TABLE_8580[560];

    // Helper: Clamp value to [0.0, 1.0]
    static inline float clamp01(float v) {
        return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    }
};
