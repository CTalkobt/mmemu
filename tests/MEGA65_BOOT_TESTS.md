# MEGA65 Boot Ready Prompt Tests

This directory contains three automated tests to verify that the MEGA65 emulator boots successfully to the "Ready" prompt.

## Test Approaches

### 1. Python Integration Test (Recommended)

**File**: `mega65_boot_ready_test.py`

Fully automated Python test that:
- Starts mmemu-cli with MEGA65 machine
- Runs boot sequence with periodic checks
- Monitors screen memory for "Ready" prompt
- Reports success/failure with timing

**Usage**:
```bash
# Run with defaults (60 second timeout)
python3 tests/mega65_boot_ready_test.py

# Custom timeout
python3 tests/mega65_boot_ready_test.py --timeout 120

# Custom max cycles
python3 tests/mega65_boot_ready_test.py --max-cycles 2000000
```

**Output**:
```
========================================
MEGA65 Boot Ready Prompt Test
========================================
Max boot time: 60 seconds
Max cycles: 1000000

Starting MEGA65 emulator...
✓ Connected to localhost:2000
Setting up emulator...
Running boot sequence...

[Check 1] Cycles:      0 PC: E4B8
[Check 2] Cycles: 100000 PC: FF4B
[Check 3] Cycles: 200000 PC: FF4B
✓ SUCCESS!
✓ Ready prompt found!
✓ Elapsed time: 15.3 seconds
✓ Cycles: 245000

Screen contents (first 5 lines):
  Line 0: READY.
  ...
```

### 2. CLI Command Script

**File**: `mega65_boot_test.txt`

Simple script that can be piped to mmemu-cli for manual testing.

**Usage**:
```bash
./bin/mmemu-cli < tests/mega65_boot_test.txt
```

This will:
- Create a MEGA65 machine
- Disable break on BRK
- Disable KERNAL HLE
- Run 1 million cycles
- Display final state and screen memory

### 3. Lua Test Script

**File**: `mega65_boot_ready_test.lua`

Lua-based test using mmemu's Lua scripting engine. Can be integrated as:
- A breakpoint action
- A cycle event
- A standalone test module

**Usage with mmemu-cli**:
```bash
# As a breakpoint action
./bin/mmemu-cli -m mega65 << 'EOF'
break $E4B8 action "local test = require('tests/mega65_boot_ready_test'); test.test(mmemu)"
run 1000000
EOF
```

**Usage as Lua module**:
```lua
local test = require("tests.mega65_boot_ready_test")
local success = test.test(backend)
if success then
    print("MEGA65 boot successful!")
end
```

## Expected Behavior

When MEGA65 emulation is working correctly:

1. **CPU boots from $E4B8** (C64 reset vector)
2. **Executes KERNAL initialization** (several thousand cycles)
3. **Sets up memory mapping** via MAP instruction
4. **Initializes hardware** (CIA, VIC-IV, etc.)
5. **Reaches IRQ handler loop** (around 200K+ cycles)
6. **Displays "Ready" prompt** in screen memory ($0400-$0427)

## Troubleshooting

### Test hangs or times out
- **Cause**: CPU is stuck in infinite loop or waiting for hardware event
- **Check**: Look at final PC value in test output
- **Solution**: Verify CPU and boot ROM are working correctly

### Ready prompt not found but screen shows something else
- **Cause**: Boot sequence is running but not reaching completion
- **Check**: Use `mega65_boot_test.txt` to manually inspect screen memory
- **Solution**: May need additional hardware emulation (timers, I/O)

### Connection refused
- **Cause**: mmemu-cli not starting properly
- **Check**: Run `./bin/mmemu-cli -m mega65` directly to see errors
- **Solution**: Rebuild with `make all`

## Performance Notes

- **Typical boot time**: 15-30 seconds wall-clock time
- **Emulated cycles**: 200K-500K cycles before "Ready"
- **Bottleneck**: Currently waiting for hardware timer/IRQ events
- **Optimization**: Could skip non-essential boot stages

## Future Enhancements

1. **Headless mode**: Add `--headless` flag to mmemu-cli for faster testing
2. **Screen snapshot**: Save screen memory at various boot stages
3. **Boot markers**: Add telemetry points to track boot progress
4. **Parallel testing**: Run multiple boot tests to check consistency
5. **Performance regression**: Track boot time across commits

## Related Files

- `/tests/mega65_boot_ready_test.py` — Python integration test
- `/tests/mega65_boot_test.txt` — CLI command script
- `/tests/mega65_boot_ready_test.lua` — Lua test module
- `/tests/mega65_boot_ready_check.lua` — Lua helper functions
- `/src/plugins/machines/mega65/` — MEGA65 machine implementation
- `/src/plugins/45gs02/` — 45GS02 CPU implementation
- `/src/plugins/devices/map_mmu/` — MAP MMU implementation
