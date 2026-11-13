#pragma once

#include <cstddef>
#include <cstdint>

/**
 * Power-up RAM contents as a pattern, after VICE's ram_init_with_pattern
 * (src/ram.c): DRAM comes up in stripes, not zeroes, and some programs (tape
 * loaders, REU size checks) depend on it. VICE's random bit flips are left
 * out, so a run is repeatable.
 */
struct RamInitParam {
    int startValue = 0;          ///< the first value of the base pattern
    int valueInvert = 0;         ///< bytes until the start value is inverted (0: never)
    int valueOffset = 0;         ///< the first pattern is shifted by this many bytes
    int patternInvert = 0;       ///< bytes until the base pattern is inverted (0: never)
    int patternInvertValue = 0;  ///< what it is inverted with
};

/** Fill `n` bytes of `mem` with the pattern `p` describes. */
inline void ramInitPattern(uint8_t* mem, size_t n, const RamInitParam& p) {
    for (size_t offset = 0; offset < n; ++offset) {
        unsigned j = 0, k = 0;
        if (p.valueInvert) j = (((offset + (size_t)p.valueOffset) / (size_t)p.valueInvert) & 1) ? 0xFF : 0x00;
        if (p.patternInvert) k = ((offset / (size_t)p.patternInvert) & 1) ? (unsigned)p.patternInvertValue : 0x00;
        mem[offset] = (uint8_t)((unsigned)p.startValue ^ j ^ k);
    }
}

/** VICE's C64 defaults (ram_resources_init; testprogs C64/raminitpattern). */
inline RamInitParam ramInitC64() { return RamInitParam{0, 4, 2, 16384, 255}; }
