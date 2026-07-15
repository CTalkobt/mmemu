# Known Issues with MEGA65 Emulation (mmemu)

Based on feedback from Bobby Tables (LGB) regarding PR #887 DMA line mode implementation.

## 1. Prefix Instruction Flag Handling

**Issue**: Prefix instructions (like NEG) set flags when they shouldn't in prefix context.

**Current Problem**: When executing `NEG / NEG / STQ`, the flags are set as if all three instructions executed. However, only prefix instructions should affect flags, and only the prefix's flags should be visible.

**Example**:
```asm
NEG     ; Prefix: Sets flags for this NEG
NEG     ; Prefix: Sets flags for this NEG
STQ     ; Actual instruction (should not set flags from prefixes)
```

The CPU should only set flags as if the two NEG prefixes executed, not from the STQ.

**Status**: ❌ Not fixed - requires 45GS02 CPU instruction execution review
**Files to Review**:
- `src/plugins/45gs02/main/cpu45gs02.cpp` - Prefix execution logic

---

## 2. Unimplemented DMA Instructions

**Issue**: SWAP, MODULO, and MIX DMA commands should not be implemented in mmemu.

**Why**: These don't exist in the actual MEGA65 hardware (pre-PR #887). Having them implemented could make someone think they're supported when they're not.

**Status**: ❌ Not implemented (should remain commented out)
**Files**: `src/plugins/devices/f018b_dma/main/f018b_dma.cpp`

---

## 3. Line Mode and Modulo Mutually Exclusive

**Issue**: Line mode addressing and modulo mode have conflicting address calculation logic.

**Hardware Behavior**: These cannot both be active simultaneously due to how address step logic works in the F018B.

**Status**: ⚠️  Needs validation - Check that mmemu rejects/handles this case correctly
**Files**: `src/plugins/devices/f018b_dma/main/f018b_dma.cpp`

---

## 4. DMA Line Mode Test Case (PR #887)

**Test File**: `dmagic_line_unit_test_mmemu.asm` (from PR #887)

**Purpose**: Comprehensive test of line mode DMA features:
- Character boundary crossing (8-pixel blocks)
- Slope accumulation
- Half-slope and full-slope modes
- Reversed minor axis
- Y-major vs X-major axis
- XY boundary crossings

**Status**: ❌ Test fails in mmemu (and pre-PR #887 Xemu)
- This is expected - tests require PR #887+ implementation
- Once prefix and modulo issues are fixed, this test should pass

**How to Run**:
```bash
# Original ACME syntax version:
ca45 tests/dmagic_line_unit_test_mmemu.asm -o test.prg

# Or run through mmemu when DMA line mode is fully implemented
./bin/mmemu-cli -m mega65 --load test.prg
```

**Expected Result**: Test reports pass for all 15 line mode tests via serial monitor output.

---

## Summary of Required Fixes

1. **HIGH PRIORITY**: Fix prefix instruction flag handling in 45GS02
   - Only prefix instructions should set flags
   - Non-prefix instructions in prefix chains should not affect flags

2. **MEDIUM PRIORITY**: Remove/comment out unimplemented DMA commands
   - SWAP, MODULO, MIX should not be in F018B

3. **MEDIUM PRIORITY**: Validate line mode and modulo exclusivity
   - Ensure they cannot both be active

4. **LOW PRIORITY**: Full DMA line mode implementation (depends on 1-3)
   - Character boundary handling
   - Slope accumulation
   - All test cases passing

---

## References

- PR #887: DMA line mode implementation
- Test case: `dmagic_line_unit_test_mmemu.asm` (written by Bobby Tables)
- Hardware reference: MEGA65 F018B DMA controller documentation
