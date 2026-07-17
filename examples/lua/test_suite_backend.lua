-- Backend-Agnostic Test Suite Example
-- Issue #24 Phase 5: Backend Abstraction Layer
--
-- This test suite runs identically on both emulator and hardware.
-- The SAME test code works with both backends.
--
-- Usage (Emulator):
--   1. Start mmemu: ./bin/mmemu-cli -m c64
--   2. In CLI: script run examples/lua/test_suite_backend.lua emulator
--
-- Usage (Hardware - Future):
--   ./test_runner.lua --backend hardware --port /dev/ttyUSB0

local TestFramework = require("test_framework")
local Backend = require("backend_interface")

-- Parse command-line arguments (handle both CLI and direct Lua execution)
local backend_type = (arg and arg[1]) or "emulator"

-- Create test framework
local tests = TestFramework.create(backend_type)

-- ============================================================================
-- Test Suite: Memory Operations (Backend-Agnostic)
-- ============================================================================

--- Test: Write and read back pattern
function test_zero_page_pattern(backend)
    -- Test zero page RAM (skip $00/$01 which are special processor state)
    -- $00 = indirect addressing temp, $01 = 6510 I/O port
    local test_range = {0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B}

    -- Fill with pattern
    for idx, addr in ipairs(test_range) do
        backend:write_byte(addr, (addr * 2) & 0xFF)
    end

    -- Verify pattern
    for idx, addr in ipairs(test_range) do
        local val = backend:read_byte(addr)
        if val ~= ((addr * 2) & 0xFF) then
            error("Byte at $" .. string.format("%02X", addr) ..
                  " = $" .. string.format("%02X", val) ..
                  " (expected $" .. string.format("%02X", (addr * 2) & 0xFF) .. ")")
        end
    end

    return true
end

--- Test: 16-bit word read/write
function test_word_operations(backend)
    backend:write_word(0x10, 0x1234)
    local val = backend:read_word(0x10)

    if val ~= 0x1234 then
        error("Word mismatch: got $" .. string.format("%04X", val) ..
              " expected $1234")
    end

    return true
end

--- Test: Register operations
function test_register_access(backend)
    -- Set accumulator
    backend:set_register("A", 0x42)
    local a = backend:get_register("A")

    if a ~= 0x42 then
        error("Accumulator mismatch: got $" .. string.format("%02X", a) ..
              " expected $42")
    end

    -- Set X register
    backend:set_register("X", 0x13)
    local x = backend:get_register("X")

    if x ~= 0x13 then
        error("X register mismatch")
    end

    return true
end

--- Test: Program counter
function test_program_counter(backend)
    backend:set_pc(0x2000)
    local pc = backend:get_pc()

    if pc ~= 0x2000 then
        error("PC mismatch: got $" .. string.format("%04X", pc) ..
              " expected $2000")
    end

    return true
end

--- Test: Memory fill and verify
function test_memory_fill(backend)
    -- Fill 32-byte region with 0xAA
    backend:fill(0x0100, 32, 0xAA)

    -- Verify
    if not backend:verify(0x0100, 32, 0xAA) then
        error("Memory fill verification failed")
    end

    return true
end

--- Test: Pattern fill with function
function test_pattern_fill_function(backend)
    -- Fill with incrementing pattern
    backend:fill(0x0200, 16, function(offset)
        return offset & 0xFF
    end)

    -- Verify
    if not backend:verify(0x0200, 16, function(offset)
        return offset & 0xFF
    end) then
        error("Pattern fill verification failed")
    end

    return true
end

--- Test: State snapshot and comparison
function test_state_snapshot(backend)
    -- Set initial state
    backend:set_register("A", 0x11)
    backend:set_register("X", 0x22)
    backend:set_register("Y", 0x33)
    backend:set_pc(0x3000)

    -- Snapshot state
    local state1 = backend:get_state()

    -- Modify state
    backend:set_register("A", 0x44)

    -- Get new state
    local state2 = backend:get_state()

    -- Verify difference detected
    local diff = backend:diff_state(state1, state2)
    if diff == nil then
        error("State difference not detected")
    end

    -- Restore original state
    backend:set_register("A", 0x11)
    local state3 = backend:get_state()

    if backend:diff_state(state1, state3) ~= nil then
        error("State not properly restored")
    end

    return true
end

--- Test: Memory dump (just ensure it doesn't crash)
function test_memory_dump(backend)
    backend:dump(0x0000, 32, "Zero Page Dump:")
    return true
end

-- ============================================================================
-- Register all tests
-- ============================================================================

tests:add_test("zero_page_pattern", test_zero_page_pattern)
tests:add_test("word_operations", test_word_operations)
tests:add_test("register_access", test_register_access)
tests:add_test("program_counter", test_program_counter)
tests:add_test("memory_fill", test_memory_fill)
tests:add_test("pattern_fill_function", test_pattern_fill_function)
tests:add_test("state_snapshot", test_state_snapshot)
tests:add_test("memory_dump", test_memory_dump)

-- ============================================================================
-- Run tests
-- ============================================================================

tests:run_all()

-- Return results for programmatic access
return tests:get_results()
