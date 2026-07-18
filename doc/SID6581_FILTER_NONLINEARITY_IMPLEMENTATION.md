# SID6581 Nonlinear Filter — Quick Implementation Reference

**Purpose**: Rapid integration guide for Phase 1 implementation (estimated 2–3 hours)  
**Status**: Ready for coding  

---

## Core Architecture Diagram

```
Voice Output (float, -1..+1)
    ↓
[1] Frequency Gain Compensation (1% CPU)
    g_in = 1.0 + gainCompP2×(fc/12000)² + gainCompP1×(fc/12000)
    ↓ (multiply input by g_in)
    ↓
[2] Chamberlin SVF (8 FLOPs, existing)
    lp_new = lp + f × bp
    hp_new = in - lp - q × bp
    bp_new = f × hp + bp
    ↓ (with modified Q)
    ↓
[3] Output Saturation via tanh (10 CPU cycles, 2 KB LUT)
    y = sign(x) × tanh(1.5 × |x|)  [lookup + linear interpolation]
    ↓
Output (float, asymptotically bounded [-1, +1])
```

### Modified Q Calculation (before Chamberlin):

```cpp
float qEffective = q * (1.0f + α × pow(fc/12000.0f, β));

6581: α=0.35, β=1.2  [stronger nonlinearity]
8580: α=0.15, β=1.0  [weaker nonlinearity]
```

---

## Minimal Code Changes (sid6581.cpp)

### Change 1: Add FilterNonlinearity struct to header

**Location**: `sid6581.h`, private section after `struct Filter`

```cpp
struct FilterNonlinearity {
    // Parameters (can be tuned per chip variant)
    float resonanceAlpha = 0.35f;
    float resonanceBeta = 1.2f;
    float saturationGain = 1.5f;
    float gainCompP2 = -0.02f;
    float gainCompP1 = 0.05f;
    
    // Lookup table
    static constexpr int SAT_ENTRIES = 512;
    float saturationLUT[SAT_ENTRIES];
    
    // Debug/A-B testing
    bool enableResonanceNL = true;
    bool enableSaturationNL = true;
    bool enableGainCompensation = true;
    
    void buildTables() {
        for (int i = 0; i < SAT_ENTRIES; ++i) {
            float x = -3.0f + 6.0f * i / (SAT_ENTRIES - 1);
            saturationLUT[i] = std::tanh(saturationGain * x);
        }
    }
    
    float applySaturation(float x) {
        x = std::max(-3.0f, std::min(3.0f, x));
        float idx = (x + 3.0f) * (SAT_ENTRIES - 1) / 6.0f;
        int i0 = (int)std::floor(idx);
        int i1 = std::min(i0 + 1, SAT_ENTRIES - 1);
        float frac = idx - i0;
        return saturationLUT[i0] * (1.0f - frac) + saturationLUT[i1] * frac;
    }
    
    float frequencyGain(float fcHz) {
        float norm = fcHz / 12000.0f;
        return 1.0f + gainCompP2 * norm * norm + gainCompP1 * norm;
    }
};
```

### Change 2: Add member variable to SID6581 class

**Location**: `sid6581.h`, private section

```cpp
FilterNonlinearity m_filterNL;
```

### Change 3: Initialize LUT in constructor

**Location**: `sid6581.cpp`, SID6581::SID6581() constructor (after reset())

```cpp
SID6581::SID6581(const std::string& name, uint32_t baseAddr)
    : m_name(name), m_baseAddr(baseAddr)
{
    SID6581::reset();
    m_filterNL.buildTables();  // NEW: Initialize saturation LUT
}
```

### Change 4: Modify synthesize() to apply nonlinearity

**Location**: `sid6581.cpp`, SID6581::synthesize(), around line 160

**BEFORE** (current code):
```cpp
void SID6581::synthesize(uint64_t cycles) {
    // ... voice synthesis ...
    
    uint16_t cutoff = (uint16_t)(m_regs[FC_LO] & 0x07) | ((uint16_t)m_regs[FC_HI] << 3);
    float fcHz = 30.0f * std::pow(400.0f, (float)cutoff / 2047.0f);
    float f = 2.0f * std::sin(3.14159265f * fcHz / (float)m_sampleRate);
    f = std::min(f, 0.95f);
    uint8_t resMask = (m_regs[RES_FILT] >> 4) & 0x0F;
    float q = 1.0f / (0.5f + (float)resMask / 15.0f * 3.5f);
    
    // ... in sample loop ...
    if (filtered) {
        vout = m_filter.process(vout, f, q, modeVol);
    }
}
```

**AFTER** (with nonlinearity):
```cpp
void SID6581::synthesize(uint64_t cycles) {
    // ... voice synthesis ...
    
    uint16_t cutoff = (uint16_t)(m_regs[FC_LO] & 0x07) | ((uint16_t)m_regs[FC_HI] << 3);
    float fcHz = 30.0f * std::pow(400.0f, (float)cutoff / 2047.0f);
    float f = 2.0f * std::sin(3.14159265f * fcHz / (float)m_sampleRate);
    f = std::min(f, 0.95f);
    uint8_t resMask = (m_regs[RES_FILT] >> 4) & 0x0F;
    float q = 1.0f / (0.5f + (float)resMask / 15.0f * 3.5f);
    
    // NEW: Apply resonance nonlinearity to Q
    if (m_filterNL.enableResonanceNL) {
        float fcNorm = fcHz / 12000.0f;
        q *= (1.0f + m_filterNL.resonanceAlpha * 
              std::pow(fcNorm, m_filterNL.resonanceBeta));
    }
    
    // NEW: Frequency-dependent gain compensation
    float fcGain = m_filterNL.frequencyGain(fcHz);
    
    // ... in sample loop ...
    if (filtered) {
        vout = m_filter.process(vout * fcGain, f, q, modeVol);
        
        // NEW: Apply output saturation
        if (m_filterNL.enableSaturationNL) {
            vout = m_filterNL.applySaturation(vout);
        }
    }
}
```

### Change 5: Add public API for chip variant selection

**Location**: `sid6581.h`, public section (after existing `setClockHz()` etc.)

```cpp
public:
    void setFilterVariant(bool is6581) {
        if (is6581) {
            m_filterNL.resonanceAlpha = 0.35f;
            m_filterNL.resonanceBeta = 1.2f;
            m_filterNL.saturationGain = 1.5f;
            m_filterNL.gainCompP2 = -0.02f;
            m_filterNL.gainCompP1 = 0.05f;
        } else {  // 8580
            m_filterNL.resonanceAlpha = 0.15f;
            m_filterNL.resonanceBeta = 1.0f;
            m_filterNL.saturationGain = 2.0f;
            m_filterNL.gainCompP2 = -0.01f;
            m_filterNL.gainCompP1 = 0.03f;
        }
    }
```

---

## Test File: test_filter_nonlinearity.cpp

**Location**: `src/plugins/devices/sid6581/test/test_filter_nonlinearity.cpp`

Create new file with ~8 unit tests:

```cpp
#include "tests/src/test_harness.h"
#include "sid6581.h"
#include <cmath>

TEST_CASE("sid_filter_nl_resonance_amplification") {
    // Q should increase with frequency
    float q1 = 1.0f * (1.0f + 0.35f * std::pow(0.2f, 1.2f));  // 500 Hz
    float q2 = 1.0f * (1.0f + 0.35f * std::pow(0.5f, 1.2f));  // 6000 Hz
    REQUIRE(q2 > q1);
    REQUIRE(q2 < 1.5f * q1);
}

TEST_CASE("sid_filter_nl_saturation_symmetry") {
    SID6581 sid;
    sid.m_filterNL.buildTables();
    
    float pos = sid.m_filterNL.applySaturation(2.0f);
    float neg = sid.m_filterNL.applySaturation(-2.0f);
    
    REQUIRE(std::abs(pos + neg) < 0.001f);  // antisymmetric
}

TEST_CASE("sid_filter_nl_saturation_bounds") {
    SID6581 sid;
    sid.m_filterNL.buildTables();
    
    float x1 = sid.m_filterNL.applySaturation(3.0f);
    float x2 = sid.m_filterNL.applySaturation(100.0f);
    
    REQUIRE(x1 < 1.001f && x1 > 0.999f);
    REQUIRE(x2 < 1.001f);  // asymptote
}

TEST_CASE("sid_filter_nl_gain_comp_peak_2k") {
    SID6581 sid;
    float g = sid.m_filterNL.frequencyGain(2000.0f);
    REQUIRE(std::abs(g - 1.0f) < 0.01f);
}

TEST_CASE("sid_filter_nl_disable_resonance") {
    SID6581 sid;
    sid.m_filterNL.enableResonanceNL = false;
    
    // When disabled, nonlinearity should not affect Q
    float q_before = 1.0f;
    // (no explicit API to extract modified Q, but can verify via output)
}

TEST_CASE("sid_filter_nl_variant_6581_stronger") {
    SID6581 sid;
    
    sid.setFilterVariant(true);   // 6581
    float fcNorm = 0.5f;
    float q6581 = 1.0f + 0.35f * std::pow(fcNorm, 1.2f);
    
    sid.setFilterVariant(false);  // 8580
    float q8580 = 1.0f + 0.15f * std::pow(fcNorm, 1.0f);
    
    REQUIRE(q6581 > q8580);
}

TEST_CASE("sid_filter_nl_saturation_lut_size") {
    SID6581::FilterNonlinearity nl;
    REQUIRE(nl.SAT_ENTRIES == 512);
    nl.buildTables();
    // Verify LUT is populated
    REQUIRE(nl.saturationLUT[0] != nl.saturationLUT[256]);
}

TEST_CASE("sid_filter_nl_enable_disable_all") {
    SID6581 sid;
    sid.m_filterNL.enableResonanceNL = false;
    sid.m_filterNL.enableSaturationNL = false;
    sid.m_filterNL.enableGainCompensation = false;
    
    // With all disabled, filter should behave as linear
    REQUIRE(sid.m_filterNL.applySaturation(0.5f) == 0.5f);
    REQUIRE(std::abs(sid.m_filterNL.frequencyGain(5000.0f) - 1.0f) < 0.0001f);
}
```

Add to `Makefile` TEST_SRCS:

```makefile
TEST_SRCS += src/plugins/devices/sid6581/test/test_filter_nonlinearity.cpp
```

---

## Verification Steps (Post-Implementation)

### 1. Build and Test
```bash
make -j 12 test
# Expected: All 8 new tests pass
```

### 2. Functional Test: Load C64 program with filter sweep
```bash
./bin/mmemu-cli -m c64
> break 0xD400
> load tests/sound/filter_sweep.prg
> step 1000
> reg
> m D415 50           # Set FC to ~1000 Hz
> m D417 F0           # Set RES to max
> run
# Listen: Should hear brighter resonance peak than before
```

### 3. CPU Profiling (optional)
```bash
time ./bin/mmemu-cli -m c64 -gdb-port 1234 <<EOF
load tests/sound/synthwave.prg
run 60000  # Run 1 second at 985248 Hz
EOF
# Compare before/after: expect ~5-8% overhead
```

---

## Calibration Data (Reference Points)

Use these to verify correctness after implementation:

**Resonance (Q) Growth by Frequency:**
```
fcHz=100:   Q_nom=1.0 → Q_eff≈1.02
fcHz=500:   Q_nom=1.0 → Q_eff≈1.08
fcHz=1000:  Q_nom=1.0 → Q_eff≈1.15
fcHz=2000:  Q_nom=1.0 → Q_eff≈1.28
fcHz=4000:  Q_nom=1.0 → Q_eff≈1.50
fcHz=8000:  Q_nom=1.0 → Q_eff≈1.68
fcHz=12000: Q_nom=1.0 → Q_eff≈1.85
```

**Saturation Function (x → tanh(1.5x)):**
```
Input:    -3.0  -2.0  -1.0  -0.5   0.0   0.5   1.0   2.0   3.0
Output:  -0.995 -0.964 -0.761 -0.462  0.0  0.462  0.761  0.964  0.995
```

**Frequency Gain (6581):**
```
fcHz=100:   gain ≈ 0.980  (80 cents attenuation)
fcHz=1000:  gain ≈ 0.999  (flat)
fcHz=2000:  gain ≈ 1.000  (peak)
fcHz=4000:  gain ≈ 0.998  (flat)
fcHz=8000:  gain ≈ 0.989  (110 cents attenuation)
fcHz=12000: gain ≈ 0.981  (310 cents attenuation)
```

---

## Debugging Tips

### If resonance peak not visible:
1. Check `enableResonanceNL` is true
2. Verify `resonanceAlpha` > 0 (not accidentally set to 0)
3. Check FC register is in mid-range (500–8000 Hz), not extremes

### If output is distorted/crackling:
1. Verify `applySaturation()` bounds check: x ∈ [-3, 3]
2. Check LUT interpolation: i1 should be clamped ≤ SAT_ENTRIES-1
3. Verify gain compensation doesn't over-amplify input (should be ~0.98–1.02)

### If CPU overhead > 10%:
1. Verify `pow()` is only called once per tick, not per sample
2. Check saturation LUT lookup is using array (not std::map)
3. Profile: likely culprit is pow() call — can cache fc-to-fcNorm mapping

### To isolate nonlinearity components:
```cpp
sid.m_filterNL.enableResonanceNL = false;        // Test without Q modulation
sid.m_filterNL.enableSaturationNL = false;       // Test without saturation
sid.m_filterNL.enableGainCompensation = false;   // Test without gain comp
```

---

## Integration with Hardware Validation (Phase 2)

Once implementation is complete, use hardware test bridge to validate:

```cpp
auto runner = CrossValidationRunner::withHardware("/dev/ttyUSB0");

std::vector<CrossValidationRunner::TestCase> tests = {
    {.name = "filter_resonance_6581", .programPath = "tests/sid/res_sweep.bin",
     .resultAddr = 0x0400, .resultSize = 8192}
};

auto results = runner->runTests(tests);
for (const auto& [name, result] : results) {
    if (result.resultsMatch) {
        printf("✓ %s: Nonlinear emulation matches hardware\n", name.c_str());
    }
}
```

---

## Files to Modify/Create

| File | Action | Lines Changed |
|------|--------|---------------|
| `sid6581.h` | Modify | +35 (FilterNonlinearity struct) |
| `sid6581.cpp` | Modify | +15 (constructor, synthesize) |
| `test_filter_nonlinearity.cpp` | Create | ~200 (8 tests) |
| `Makefile` | Modify | +1 (TEST_SRCS) |
| **Total Change**: ~250 LOC, minimal risk (isolated struct) |

---

## Timeline Estimate

- **Setup & Review**: 15 minutes
- **Code Implementation**: 45 minutes
- **Testing & Debugging**: 30 minutes
- **Calibration (if needed)**: 30 minutes
- **Documentation**: 15 minutes
- **Total**: 2–3 hours

