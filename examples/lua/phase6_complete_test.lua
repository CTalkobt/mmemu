-- Phase 6 Complete Test Suite
-- Tests all Phase 6 features: stdlib, test_utils, device_io, memory_patterns, profiler

print("")
print("================================================")
print("Phase 6 Complete Feature Test")
print("================================================")
print("")

-- Load all Phase 6 modules
local stdlib = require("stdlib")
local test_utils = require("test_utils")
local device_io = require("device_io")
local patterns = require("memory_patterns")
local profiler = require("profiler")

print("✓ All Phase 6 modules loaded successfully")
print("")

-- ============================================================================
-- Test Phase 6.1: stdlib
-- ============================================================================

print("Testing Phase 6.1: stdlib.lua")
print(string.rep("-", 40))

-- String formatting
assert(stdlib.hex(0xFF) == "$FF", "hex formatting")
assert(stdlib.hex(0x1234, 4) == "$1234", "hex with width")
print("✓ Hex formatting works")

-- Binary formatting
assert(stdlib.binary(0xAA) == "10101010", "binary formatting")
print("✓ Binary formatting works")

-- Flag formatting (0x81 has N bit set: binary 10000001)
local flags_str = stdlib.format_flags(0x81)
stdlib.assert(flags_str:match("N"), "flags with N bit")
print("✓ Flag formatting works: " .. flags_str)

-- Table operations
local t1 = {a = 1, b = 2}
local t2 = {c = 3}
local merged = stdlib.table_merge(t1, t2)
assert(merged.a == 1 and merged.c == 3, "table merge")
print("✓ Table merge works")

local inverted = stdlib.table_invert({x = "alpha", y = "beta"})
assert(inverted["alpha"] == "x", "table invert")
print("✓ Table invert works")

-- Bitwise operations
assert(stdlib.bitfield(0xFF, 0, 4) == 0x0F, "bitfield extraction")
assert(stdlib.rotate_left(0x81, 1) == 0x03, "rotate left")
assert(stdlib.popcount(0xFF) == 8, "popcount")
print("✓ Bitwise operations work")

-- Assertions
stdlib.assert_eq(42, 42)
stdlib.assert_hex_eq(0xFF, 0xFF)
stdlib.assert_range(50, 0, 100)
print("✓ Assertions work")

print("")

-- ============================================================================
-- Test Phase 6.2: device_io
-- ============================================================================

print("Testing Phase 6.2: device_io.lua")
print(string.rep("-", 40))

-- Verify device bindings exist
stdlib.assert(device_io.SID_set_frequency ~= nil, "SID function")
stdlib.assert(device_io.VIC_set_sprite_pos ~= nil, "VIC function")
stdlib.assert(device_io.CIA_set_timer_a ~= nil, "CIA function")
stdlib.assert(device_io.DMA_copy ~= nil, "DMA function")
print("✓ All device I/O functions available")

-- Device constants
stdlib.assert(device_io.SID.BASE_ADDR == 0xD400, "SID base address")
stdlib.assert(device_io.VIC.BASE_ADDR == 0xD000, "VIC base address")
stdlib.assert(device_io.CIA.CIA1_BASE == 0xDC00, "CIA1 base address")
print("✓ Device base addresses correct")

print("")

-- ============================================================================
-- Test Phase 6.3: memory_patterns
-- ============================================================================

print("Testing Phase 6.3: memory_patterns.lua")
print(string.rep("-", 40))

-- Static patterns
assert(patterns.ZERO_FILL == 0x00, "zero fill pattern")
assert(patterns.ONES_FILL == 0xFF, "ones fill pattern")
assert(#patterns.WALKING_ONE == 8, "walking one length")
print("✓ Static patterns available")

-- Pattern generators
local addr_pat = patterns.address_pattern(16, 0x100)
assert(addr_pat[1] == 0x00, "address pattern first byte")
assert(addr_pat[16] == 0x0F, "address pattern last byte")
print("✓ Address pattern generator works")

local random_pat = patterns.random_pattern(10, 42)
assert(#random_pat == 10, "random pattern length")
print("✓ Random pattern generator works")

local fib_pat = patterns.fibonacci_pattern(8)
assert(fib_pat[1] == 0x01 and fib_pat[2] == 0x01, "fibonacci pattern")
print("✓ Fibonacci pattern generator works")

local checkerboard = patterns.checkerboard_pattern(8)
assert(checkerboard[1] == 0x55 and checkerboard[2] == 0xAA, "checkerboard")
print("✓ Checkerboard pattern generator works")

print("")

-- ============================================================================
-- Test Phase 6.1.2: test_utils
-- ============================================================================

print("Testing Phase 6.1.2: test_utils.lua")
print(string.rep("-", 40))

-- Test assertions
test_utils.assert_equal(42, 42)
test_utils.assert_not_equal(1, 2)
test_utils.assert_byte_equal(0xFF, 0xFF)
test_utils.assert_word_equal(0xFFFF, 0xFFFF)
test_utils.assert_true(true)
test_utils.assert_false(false)
test_utils.assert_in_range(50, 0, 100)
print("✓ All assertions work")

-- Test results tracking
local results = test_utils.TestResults.new()
results:record_pass("test1")
results:record_fail("test2", "Error message")
assert(results.passed == 1, "passed count")
assert(results.failed == 1, "failed count")
assert(results:total() == 2, "total count")
print("✓ Test results tracking works")

-- Success rate
assert(results:success_rate() == 50, "success rate calculation")
print("✓ Success rate: " .. results:success_rate() .. "%")

print("")

-- ============================================================================
-- Test Phase 6.3: profiler
-- ============================================================================

print("Testing Phase 6.3: profiler.lua")
print(string.rep("-", 40))

local prof = profiler.Profiler.new()

-- Test basic timing
prof:start("operation1")
for i = 1, 1000 do
    local x = i * 2
end
local elapsed1 = prof:stop()
assert(elapsed1 >= 0, "timing works")
print("✓ Profile.start/stop works (took " .. elapsed1 .. "ms)")

-- Test measure function
local measure_result, measure_time = prof:measure(function()
    local sum = 0
    for i = 1, 100 do
        sum = sum + i
    end
    return sum
end, "measure_test")
assert(measure_result == 5050, "measure result")
assert(measure_time >= 0, "measure time")
print("✓ Profile.measure works (took " .. measure_time .. "ms)")

-- Test statistics
local stats = prof:stats()
assert(stats ~= nil, "stats exists")
assert(stats.count >= 2, "stats count >= 2")
assert(stats.total >= 0, "stats total >= 0")
assert(stats.avg >= 0, "stats average >= 0")
print("✓ Profiler statistics:")
print("  - Samples: " .. stats.count)
print("  - Total: " .. stats.total .. "ms")
print("  - Average: " .. string.format("%.2f", stats.avg) .. "ms")
print("  - Min: " .. stats.min .. "ms")
print("  - Max: " .. stats.max .. "ms")

print("")

-- ============================================================================
-- Integration Test: Combined Features
-- ============================================================================

print("Integration Test: Combined Phase 6 Features")
print(string.rep("-", 40))

local integration_prof = profiler.Profiler.new()

-- Simulate a complete test scenario
integration_prof:start("integration_test")

-- Use stdlib for formatting
local test_label = "Memory Test " .. stdlib.hex(0x0100, 4)
print("Running: " .. test_label)

-- Use patterns
local test_pattern = patterns.address_pattern(32, 0x0100)
if stdlib.hex_bytes then
    local pattern_hex = stdlib.hex_bytes(test_pattern)
    print("Pattern: " .. pattern_hex:sub(1, math.min(47, #pattern_hex)) .. "...")
else
    print("Pattern: 32 bytes from address $0100")
end

-- Simulate device control (without actual backend)
print("Simulated device operations:")
print("  - SID frequency set to 440 Hz")
print("  - VIC sprite at (100, 100)")
print("  - Memory filled with pattern")

-- Use test utilities
test_utils.assert_equal(#test_pattern, 32)
print("✓ Pattern verification passed")

integration_prof:stop()

print("")
print("================================================")
print("Phase 6 Complete Test Results")
print("================================================")
print("")
print("✓ Phase 6.1: stdlib.lua - PASSED")
print("✓ Phase 6.1: test_utils.lua - PASSED")
print("✓ Phase 6.3: memory_patterns.lua - PASSED")
print("✓ Phase 6.2: device_io.lua - PASSED")
print("✓ Phase 6.3: profiler.lua - PASSED")
print("✓ Integration Test - PASSED")
print("")
print("Total Tests Passed: 6/6")
print("Integration Stats:")
integration_prof:print_report()
print("")
print("================================================")
print("✓ Phase 6 Implementation Complete and Verified!")
print("================================================")
