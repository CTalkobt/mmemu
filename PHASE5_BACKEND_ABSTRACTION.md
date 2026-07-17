# Issue #24 Phase 5: Backend Abstraction Layer

## Overview

Phase 5 introduces a unified testing interface that allows **the same Lua test code** to run against both:
- **mmemu Emulator** (via Lua API)
- **Real MEGA65 Hardware** (via serial/network protocol)

This enables comprehensive hardware validation and emulator accuracy verification.

---

## Architecture

### Backend Interface Contract

All backends implement a common interface (`backend_interface.lua`):

```lua
-- Memory operations
Backend:read_byte(addr)
Backend:write_byte(addr, value)
Backend:read_word(addr)
Backend:write_word(addr, value)

-- Register operations
Backend:get_register(name)
Backend:set_register(name, value)
Backend:get_pc()
Backend:set_pc(addr)

-- Utilities
Backend:log(message)
Backend:step()
Backend:is_available()

-- Advanced
Backend:fill(addr, size, pattern)
Backend:verify(addr, size, pattern)
Backend:dump(addr, size, label)
Backend:get_state()
Backend:diff_state(state1, state2)
```

### Backend Implementations

#### 1. EmulatorBackend (`backend_emulator.lua`)
- **Usage**: Tests run in mmemu CLI/GUI
- **Communication**: Direct Lua function calls to mmemu API
- **Speed**: Fast (no serial overhead)
- **Availability**: Any mmemu instance with Lua support

```lua
local Backend = require("backend_interface")
local backend = Backend.create("emulator")
backend:read_byte(0x2000)  -- Calls mmemu.read_byte(0x2000)
```

#### 2. HardwareBackend (`backend_hardware.lua`)
- **Usage**: Tests run against real MEGA65
- **Communication**: Serial port or network connection
- **Speed**: Slower (due to I/O latency)
- **Availability**: MEGA65 with debug firmware/monitor

```lua
local Backend = require("backend_interface")
local backend = Backend.create("hardware")
backend:connect("/dev/ttyUSB0", 115200)
backend:read_byte(0x2000)  -- Sends "READ 2000" via serial
```

---

## Test Framework

The `test_framework.lua` module provides:
- Test registration
- Automatic test execution
- Pass/fail tracking
- Summary reporting
- Result analysis

```lua
local TestFramework = require("test_framework")
local suite = TestFramework.create("emulator")

suite:add_test("test_name", function(backend)
    backend:write_byte(0x100, 0x42)
    assert(backend:read_byte(0x100) == 0x42)
    return true  -- Test passed
end)

suite:run_all()
```

---

## Usage Patterns

### Pattern 1: Same Tests, Different Backends

```lua
-- test_suite.lua
local function test_memory_pattern(backend)
    backend:fill(0x0200, 32, 0xAA)
    return backend:verify(0x0200, 32, 0xAA)
end

-- Run on emulator
local emu_tests = TestFramework.create("emulator")
emu_tests:add_test("memory", test_memory_pattern)
emu_tests:run_all()

-- Run on hardware (future)
local hw_tests = TestFramework.create("hardware")
hw_tests:add_test("memory", test_memory_pattern)
hw_tests:run_all()
```

### Pattern 2: Backend-Agnostic Test Library

```lua
-- tests/lib_validation.lua
function validate_zero_page(backend)
    -- Works with any backend
    local ok = backend:verify(0x0000, 256, function(i)
        return (i * 2) & 0xFF
    end)
    return ok
end

function validate_stack(backend)
    backend:set_register("SP", 0xFF)
    return backend:get_register("SP") == 0xFF
end
```

### Pattern 3: Hardware Validation

```lua
-- test_emulator_vs_hardware.lua
local function compare_results()
    local emu_results = TestFramework.create("emulator"):run_all()
    local hw_results = TestFramework.create("hardware"):run_all()
    
    for test_name, emu_result in pairs(emu_results) do
        local hw_result = hw_results[test_name]
        if emu_result.passed ~= hw_result.passed then
            print("DIVERGENCE: " .. test_name)
        end
    end
end
```

---

## Current Implementation Status

### ✅ Completed

- `backend_interface.lua` — Full interface contract with 15+ methods
- `backend_emulator.lua` — Full EmulatorBackend implementation
- `test_framework.lua` — Complete test harness with reporting
- `test_suite_backend.lua` — 8 backend-agnostic example tests
- Documentation — Usage guide and architecture

### ✅ Can Run Now

```bash
# In mmemu CLI
./bin/mmemu-cli -m c64
> script run examples/lua/test_suite_backend.lua emulator
```

### ⏳ TODO: Hardware Backend

The `backend_hardware.lua` is a documented stub. To complete it:

1. **Choose Communication Method**:
   - Serial port (RS-232 / USB-UART)
   - Network socket (TCP/UDP)
   - MCP server (if available on hardware)

2. **Define Protocol**:
   ```
   Command Format: "<CMD> <ARGS>\n"
   Response Format: "<RESULT>\n"
   
   Examples:
   - "READ 2000\n" → "AA\n"
   - "WRITE 2000 42\n" → "OK\n"
   - "GETREG A\n" → "42\n"
   - "SETREG A 42\n" → "OK\n"
   ```

3. **Implement Serial Handler**:
   ```lua
   local serial = require("luarocks")  -- or custom serial module
   local conn = serial.open(port, baudrate)
   conn:write("READ 2000\n")
   local response = conn:read(3)
   ```

4. **Add Error Handling**:
   - Connection timeouts
   - Invalid responses
   - Protocol errors

---

## Example: Running Tests

### Emulator (Current)

```bash
# Terminal 1: Start mmemu
./bin/mmemu-cli -m c64

# Terminal 2: In mmemu CLI
(mmemu) script run examples/lua/test_suite_backend.lua emulator

# Output:
# ================================================
# Running 8 tests on backend: emulator
# ================================================
#
# [ RUN      ] zero_page_pattern
# [       OK ] zero_page_pattern
# [ RUN      ] word_operations
# [       OK ] word_operations
# ...
# Passed: 8 / 8
# ✓ ALL TESTS PASSED
```

### Hardware (Future)

```bash
# Once hardware backend is implemented
./test_runner.lua \
  --backend hardware \
  --port /dev/ttyUSB0 \
  --baudrate 115200 \
  --suite examples/lua/test_suite_backend.lua

# Same test output, but results from real hardware!
```

---

## Test Results Comparison (Future)

```lua
-- compare_backends.lua
local function compare_all_backends()
    local emu = TestFramework.create("emulator")
    local hw = TestFramework.create("hardware")
    
    -- Add same tests to both
    for _, test_spec in ipairs(TEST_SPECS) do
        emu:add_test(test_spec.name, test_spec.func)
        hw:add_test(test_spec.name, test_spec.func)
    end
    
    emu:run_all()
    hw:run_all()
    
    -- Compare
    local emu_results = emu:get_results()
    local hw_results = hw:get_results()
    
    local divergences = {}
    for test_name, emu_result in pairs(emu_results) do
        if emu_result.passed ~= hw_results[test_name].passed then
            table.insert(divergences, test_name)
        end
    end
    
    if #divergences == 0 then
        print("✓ All tests match between emulator and hardware!")
    else
        print("✗ Divergences found:")
        for _, test_name in ipairs(divergences) do
            print("  - " .. test_name)
        end
    end
end
```

---

## Benefits

| Aspect | Benefit |
|--------|---------|
| **Code Reuse** | Same test runs on emulator and hardware |
| **Validation** | Verify emulator accuracy against real hardware |
| **Regression Prevention** | Detect emulator/hardware divergence |
| **CI Integration** | Automated testing on both targets |
| **Documentation** | Tests serve as API examples |

---

## Next Steps

### Phase 5.1 (Current)
- ✅ Design backend interface
- ✅ Implement EmulatorBackend
- ✅ Create test framework
- ✅ Write example tests
- ⏳ Test with real data

### Phase 5.2 (Future)
- Implement HardwareBackend
- Define serial protocol
- Add connection management
- Error handling & diagnostics

### Phase 5.3 (Future)
- Result comparison tools
- Divergence analysis
- Automated CI integration
- Performance benchmarking

---

## Files

| File | Purpose |
|------|---------|
| `backend_interface.lua` | Abstract interface contract |
| `backend_emulator.lua` | mmemu backend implementation |
| `backend_hardware.lua` | Hardware backend stub (TODO) |
| `test_framework.lua` | Test harness with reporting |
| `test_suite_backend.lua` | Example tests (8 test cases) |
| `PHASE5_BACKEND_ABSTRACTION.md` | This documentation |

---

## Usage Summary

```lua
-- 1. Require framework
local TestFramework = require("test_framework")

-- 2. Create suite with backend
local tests = TestFramework.create("emulator")  -- or "hardware"

-- 3. Add tests (same code works with both backends)
tests:add_test("my_test", function(backend)
    backend:write_byte(0x100, 0x42)
    assert(backend:read_byte(0x100) == 0x42)
    return true
end)

-- 4. Run
tests:run_all()
```

---

## Conclusion

Phase 5 provides the **infrastructure for hardware-validated testing**. The same Lua test code works identically on both emulator and real hardware, enabling:

- **Emulator Validation** — Verify against real hardware
- **Regression Detection** — Catch emulator/hardware divergence
- **Automated Testing** — CI pipeline with both targets
- **Documentation** — Tests demonstrate API usage

Hardware backend implementation (Phase 5.2) will complete the vision.
