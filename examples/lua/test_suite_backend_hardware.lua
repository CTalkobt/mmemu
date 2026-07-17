-- Hardware Backend Test Suite Example
-- Issue #24 Phase 5.2: Hardware Backend
--
-- This test suite runs on real MEGA65 hardware or in mock mode.
-- The SAME test code works with both EmulatorBackend and HardwareBackend.
--
-- Usage (Mock Hardware - No Real Hardware Needed):
--   1. Start mmemu: ./bin/mmemu-cli -m c64
--   2. In CLI: script run examples/lua/test_suite_backend_hardware.lua
--
-- Usage (Real Hardware):
--   1. Connect MEGA65 via USB
--   2. Ensure serial monitor program is running on MEGA65
--   3. Run: script run examples/lua/test_suite_backend_hardware.lua /dev/ttyUSB0

local TestFramework = require("test_framework")
local Backend = require("backend_interface")
local HardwareBackend = require("backend_hardware")

-- Parse arguments: [port] [baudrate]
-- Default to mock mode for testing without hardware
local port = (arg and arg[1]) or "mock"
local baudrate = (arg and arg[2]) or 115200

-- Create hardware backend
local hw = HardwareBackend.new(port, baudrate)

-- Try to connect
if not hw:connect() then
    print("ERROR: Failed to connect to hardware backend")
    print("  Port: " .. port)
    print("  Baudrate: " .. baudrate)
    print("")
    print("To use mock mode (no hardware):")
    print("  ./bin/mmemu-cli -m c64")
    print("  > script run examples/lua/test_suite_backend_hardware.lua")
    return
end

-- Create test framework with the connected hardware backend
local tests = TestFramework.create(hw)

-- ============================================================================
-- Test Suite: Memory Operations (Backend-Agnostic)
-- ============================================================================

--- Test: Write and read back pattern
function test_zero_page_pattern(backend)
    -- Test zero page RAM (skip $00/$01 which are special processor state)
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

--- Test: 16-bit word operations
function test_word_operations(backend)
    -- Write low byte
    backend:write_byte(0x0100, 0x34)
    -- Write high byte
    backend:write_byte(0x0101, 0x12)

    -- Read back
    local lo = backend:read_byte(0x0100)
    local hi = backend:read_byte(0x0101)

    if lo ~= 0x34 or hi ~= 0x12 then
        error("Word mismatch: got $" .. string.format("%04X", (hi << 8) | lo) ..
              " expected $1234")
    end

    return true
end

--- Test: CPU register access
function test_register_access(backend)
    -- Set accumulator
    backend:set_register("A", 0x55)
    local a_val = backend:get_register("A")
    if a_val ~= 0x55 then
        error("Accumulator mismatch: $" .. string.format("%02X", a_val) .. " != $55")
    end

    -- Set X register
    backend:set_register("X", 0xAA)
    local x_val = backend:get_register("X")
    if x_val ~= 0xAA then
        error("X register mismatch: $" .. string.format("%02X", x_val) .. " != $AA")
    end

    return true
end

--- Test: Program counter manipulation
function test_program_counter(backend)
    -- Set PC
    backend:set_pc(0x0800)
    local pc = backend:get_pc()

    if pc ~= 0x0800 then
        error("PC mismatch: $" .. string.format("%04X", pc) .. " != $0800")
    end

    return true
end

--- Test: Memory fill operation
function test_memory_fill(backend)
    local addr_start = 0x0200
    local size = 16

    -- Fill with pattern
    for i = 0, size - 1 do
        backend:write_byte(addr_start + i, 0xAA)
    end

    -- Verify filled
    for i = 0, size - 1 do
        local val = backend:read_byte(addr_start + i)
        if val ~= 0xAA then
            error("Fill failed at offset " .. i)
        end
    end

    return true
end

--- Test: Dynamic pattern fill
function test_pattern_fill_function(backend)
    local addr_start = 0x0300
    local pattern_func = function(offset)
        return (offset * 3) & 0xFF
    end

    -- Fill with dynamic pattern
    for i = 0, 15 do
        backend:write_byte(addr_start + i, pattern_func(i))
    end

    -- Verify pattern
    for i = 0, 15 do
        local val = backend:read_byte(addr_start + i)
        local expected = pattern_func(i)
        if val ~= expected then
            error("Pattern mismatch at offset " .. i .. ": got $" ..
                  string.format("%02X", val) .. " expected $" ..
                  string.format("%02X", expected))
        end
    end

    return true
end

--- Test: State snapshot
function test_state_snapshot(backend)
    -- Save initial state
    local state1 = backend:get_state()

    -- Modify state
    backend:write_byte(0x0400, 0x12)
    backend:set_register("A", 0x34)

    -- Get modified state
    local state2 = backend:get_state()

    -- States should differ
    if state1 == state2 then
        error("State should have changed after modification")
    end

    return true
end

--- Test: Memory dump and diagnostics
function test_memory_dump(backend)
    -- Create a pattern in memory
    for i = 0, 15 do
        backend:write_byte(0x0400 + i, (i * 2) & 0xFF)
    end

    -- Dump memory (logs to backend, returns nil)
    backend:dump(0x0400, 16, "Memory Dump ($0400-$040F):")

    return true
end

-- ============================================================================
-- Register Tests
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
-- Run Tests
-- ============================================================================

tests:run_all()

-- Disconnect
hw:disconnect()
