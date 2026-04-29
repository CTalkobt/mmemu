# MEGA65 Math Accelerator

The MEGA65 Math Accelerator is a hardware peripheral that provides fast 32-bit integer multiplication and division. In `mmsim`, it is emulated as an instantaneous computation device, though real hardware has a small latency.

## 1. Memory Map

By default, the Math Accelerator is mapped to the `$D760–$D77F` range in the I/O area.

| Address | Register | Description |
|---------|----------|-------------|
| `$D760–$D763` | `DIVIDEND` | 32-bit Dividend (Input for Division) |
| `$D764–$D767` | `DIVISOR`  | 32-bit Divisor (Input for Division) |
| `$D768–$D76B` | `QUOTIENT` | 32-bit Quotient (Output of Division, Read-only) |
| `$D76E`       | `SIGN`     | Sign byte (Used by signed math routines) |
| `$D770–$D773` | `MULA` / `REMAINDER` | 32-bit Multiplier A / Division Remainder (Output) |
| `$D774–$D777` | `MULB`     | 32-bit Multiplier B (Input) |
| `$D778–$D77B` | `PRODUCT`  | 32-bit Product (Output of Multiplication, Read-only) |

*Note: All multi-byte registers are little-endian.*

## 2. Operation

### 2.1 Multiplication
To perform a multiplication:
1. Write the first 32-bit factor to `MULA` (`$D770–$D773`).
2. Write the second 32-bit factor to `MULB` (`$D774–$D777`).
3. The result is immediately available in `PRODUCT` (`$D778–$D77B`).

In the current implementation, the result is recomputed whenever any byte of `MULA` or `MULB` is written.

### 2.2 Division
To perform a division:
1. Write the 32-bit dividend to `DIVIDEND` (`$D760–$D763`).
2. Write the 32-bit divisor to `DIVISOR` (`$D764–$D767`).
3. The quotient is immediately available in `QUOTIENT` (`$D768–$D76B`).
4. The remainder is immediately available in `REMAINDER` (`$D770–$D773`).

In the current implementation, the result is recomputed whenever any byte of `DIVIDEND` or `DIVISOR` is written.

## 3. Implementation Details

- **Instantaneous Execution**: Unlike real hardware which requires ~16–40 clock cycles depending on the operation, `mmsim` updates the output registers immediately upon a write to the input registers.
- **32-bit Limitation**: The current implementation computes a 32-bit product. (Note: Real MEGA65 hardware supports a 64-bit product, but only the lower 32 bits are currently emulated in this plugin).
- **Unsigned Math**: The hardware registers themselves perform unsigned operations. Signed math is typically handled by software using the `SIGN` register as a scratchpad.
- **Triggering**: While the documentation for the real hardware suggests that writing the most significant byte triggers the operation, this emulator recomputes on any write to the input range to ensure compatibility with various access patterns.
