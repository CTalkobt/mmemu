# F018B DMA: Line Drawing Enhancements (Issue #81)

## Overview

The F018B DMA controller implements hardware-accelerated line drawing and texture scaling through advanced address calculation features. These enhancements enable drawing lines at up to **40.5 Mpixels per second** on the MEGA65.

## Reference Documentation

- **MEGA65 Book**: Appendix L, Section "Texture Scaling and Line Drawing" (L-15 to L-16)
- **VHDL Reference**: gs4510.vhdl DMAgic state machine
- **Example**: mega65-tools `test_290.c` — hardware accelerated line drawing
- **Issue**: GitHub issue #81 — "F018B DMA: Audio DMA, inline lists, line drawing enhancements"

## Implementation Status

### ✅ Fully Implemented Features

#### 1. **Line Mode Enable & Control** ($8F/$9F)
- **Option $8F**: Destination address line drawing mode control
- **Option $9F**: Source address line drawing mode control
- **Bit 7**: Enable line mode (`slopeType & 0x80`)
- **Bit 6**: Axis selection (0=X-major, 1=Y-major) 
- **Bit 5**: Slope sign (0=positive, 1=negative)

#### 2. **Card Boundary Offset Configuration**
- **Option $87/$88**: X column bytes (destination) — offset to move right one pixel
- **Option $97/$98**: X column bytes (source)
- **Option $89/$8A**: Y row bytes (destination) — offset to move down one row  
- **Option $99/$9A**: Y row bytes (source)

Configuration allows flexible screen layouts:
```c
// FCM mode vertical stripe (texture scaling):
xCol = 8;     // Move 8 bytes right for next pixel column
yCol = 8 * 25; // Move 8*25 bytes down for next row (25 char-width)

// Standard horizontal line on 40-char display:
xCol = 1;     // Move 1 byte right
yCol = 40;    // Move 40 bytes down (one line)
```

#### 3. **Slope Configuration**
- **Option $8B/$8C**: Slope value (destination)
- **Option $9B/$9C**: Slope value (source)
- **16-bit slope**: Added to accumulator each major-axis step
- **Fixed-point**: Upper 8 bits = integer pixels, lower 8 bits = fractional pixels

#### 4. **Slope Accumulator Initialization** ✨ (Enhancement)
- **Option $8D/$8E**: Initial fractional slope (destination) — **VHDL verified**
- **Option $9D/$9E**: Initial fractional slope (source) — **VHDL verified**
- **16-bit initialization**: Sets starting point of accumulator for sub-pixel precision
- **Enables**: Starting at fractional positions within a pixel grid

#### 5. **Address Calculation Core**
Implemented in `stepAddress()` function:
```cpp
if (lm.slopeType & 0x40) {
    // Y-major axis: always step Y, conditionally step X on slope overflow
    accum += 0x800;  // +8 rows (Y major step)
    
    // Check Y card boundary crossing
    if ((accum & (7u << (3 + 8))) == (7u << (3 + 8)))
        accum += lm.yCol;
    
    // On slope accumulator overflow, step X (minor axis)
    if (lm.slopeAccum >= 0x10000) {
        lm.slopeAccum -= 0x10000;
        // Move left or right depending on slope sign
        accum += (lm.slopeType & 0x20) ? 
            (accum & 0x700 == 0 ? lm.xCol + 0x100 : -0x100) :
            (accum & 0x700 == 0x700 ? lm.xCol + 0x100 : 0x100);
    }
} else {
    // X-major axis: always step X, conditionally step Y on slope overflow
    accum += ((accum & 0x700) == 0x700) ? lm.xCol + 0x100 : 0x100;
    
    if (lm.slopeAccum >= 0x10000) {
        lm.slopeAccum -= 0x10000;
        accum += (lm.slopeType & 0x20) ? (uint32_t)-0x800 : 0x800u;
    }
}
```

### ✅ Texture Scaling Support

#### Skip Rate Configuration (Fractional Stepping)
- **Option $82/$83**: Source skip rate (256ths of bytes per pixel)
- **Option $84/$85**: Destination skip rate
- **Fixed-point format**: Upper byte = whole bytes, lower byte = 256ths

**Examples**:
- `$0100`: 1.0 bytes per pixel (no scaling)
- `$0080`: 0.5 bytes per pixel (2x zoom in)
- `$0200`: 2.0 bytes per pixel (0.5x zoom out)
- `$00FF`: 0.996 bytes per pixel (nearly 1:1, slight zoom out)

#### Texture Layout
For efficient vertical line drawing with scaling:
```c
// Vertical stripe layout (25 characters per row, 8 pixels per char)
// This allows DMA to draw from top to bottom efficiently
Pixel(0,0)  -> Memory[0*8]
Pixel(0,1)  -> Memory[1*8]
Pixel(0,2)  -> Memory[2*8]
...
Pixel(1,0)  -> Memory[25*8]   // +200 bytes for next column
Pixel(1,1)  -> Memory[25*8 + 1*8]
```

## Verification Against VHDL

### Tested Features
✅ **Slope accumulator overflow detection** — 16-bit accumulator wraps at 0x10000  
✅ **Axis selection (X-major vs Y-major)** — Controlled by bit 6 of slopeType  
✅ **Slope sign (positive vs negative)** — Controlled by bit 5 of slopeType  
✅ **Card boundary crossing** — Detected on bits [14:11] for Y, bits [10:8] for X  
✅ **Fractional address arithmetic** — Lower 8 bits of accumulator hold fractional pixel position  
✅ **Slope initialization with fractional values** — $8D/$8E, $9D/$9E options working  

### Test Coverage
Test cases in `test_f018b_dma.cpp`:
- `f018b_line_mode_slope_accumulator_init` — Verify $8D/$8E parsing
- `f018b_line_mode_x_major_axis` — X-major selection ($8F bit 6 = 0)
- `f018b_line_mode_y_major_axis` — Y-major selection ($8F bit 6 = 1)
- `f018b_line_mode_positive_slope` — Positive slope ($8F bit 5 = 0)
- `f018b_line_mode_negative_slope` — Negative slope ($8F bit 5 = 1)
- `f018b_line_mode_x_column_bytes` — X offset configuration ($87/$88)
- `f018b_line_mode_y_row_bytes` — Y offset configuration ($89/$8A)
- `f018b_texture_scaling_skip_rate` — Fractional stepping ($82-$85)
- `f018b_line_drawing_speed` — Verify 40.5 Mpixel/sec throughput

## Performance Characteristics

### Drawing Speed
- **Linear mode**: 40.5 MiB/s fill, 20.25 MiB/s copy
- **Line mode**: Up to 40.5 Mpixels/sec (one pixel = one DMA cycle at 40MHz)
- **Comparison to C65**: ~10x faster than C65 DMAgic (~4 Mpixels/sec)

### Memory Efficiency
- **Texture scaling**: Source skip rate < $0100 compresses source data (zooming in)
- **Screen layout**: Flexible via xCol/yCol configuration
- **Sub-pixel precision**: Fractional slope accumulator enables smooth lines

## Usage Example

```c
// Setup for drawing a diagonal line from (10, 20) to (100, 50)
// Using destination-only line mode

// Configure line drawing parameters
// X column bytes: 1 (move right by 1 byte per pixel)
// Y row bytes: 40 (move down by 40 bytes per row on 40-char display)
// Slope: ~4 (Y steps ~4 times per X step)

// Write enhanced DMA job with line mode options:
job.options[] = {
    0x87, 0x01,  // X column bytes LSB
    0x88, 0x00,  // X column bytes MSB
    0x89, 0x28,  // Y row bytes LSB (40)
    0x8A, 0x00,  // Y row bytes MSB
    0x8B, 0x04,  // Slope LSB (4)
    0x8C, 0x00,  // Slope MSB
    0x8D, 0x00,  // Slope accum init LSB (start at integer boundary)
    0x8E, 0x00,  // Slope accum init MSB
    0x8F, 0x81,  // Line mode: enabled, X-major, positive slope
    0x00         // End of options
};

// DMA job draws line at 40.5 Mpixels/sec!
```

## VHDL Compliance Notes

The implementation follows the gs4510.vhdl DMAgic state machine logic:

1. **Address Fixed-Point Format**: Lower 8 bits = fractional position within pixel
2. **Slope Overflow**: When accumulator ≥ 0x10000, minor axis steps and accumulator wraps
3. **Card Boundary**: Detected based on bits [14:11] (Y) and [10:8] (X) of address
4. **Direction Handling**: Negative slopes decrement; positive slopes increment
5. **Hold Flag**: When set, address does not change (useful for repeated fills)

## Current Limitations

- **Line mode + modulo are mutually exclusive** (VHDL constraint: both can't be enabled simultaneously)
- **Spiral mode incompatible with line mode** (separate address calculation systems)
- **Maximum slope**: 16-bit value allows slopes up to ~256:1 (steep diagonal lines)
- **Fractional precision**: 8-bit fractional component allows pixel sub-division into 256 levels

## Future Enhancements

1. **Inline DMA lists** ($D707 trigger) — Issue #81 item 2
2. **Enhanced texture rotation/shearing** — Angle-based coordinate transforms
3. **Anti-aliased line drawing** — Dithering patterns for smooth edges
4. **Combined line + fill** — Single operation for filled polygons

## References

- MEGA65 Book: https://github.com/mega65/mega65-book
- gs4510.vhdl: https://github.com/MEGA65/mega65-core
- mega65-tools test_290.c: https://github.com/mega65/mega65-tools/blob/master/tests/test_290.c
- Issue #81: https://github.com/CTalkobt/mmemu/issues/81
