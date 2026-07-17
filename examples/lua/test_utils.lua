-- Test Utilities for mmemu Lua Scripts
-- Issue #24 Phase 6.1: Test Utilities
--
-- High-level testing utilities and assertions

local stdlib = require("stdlib")
local test_utils = {}

-- ============================================================================
-- Assertion API
-- ============================================================================

--- Assert values are equal
function test_utils.assert_equal(actual, expected, message)
    if actual ~= expected then
        local msg = message or ""
        if msg ~= "" then msg = msg .. ": " end
        msg = msg .. "expected " .. tostring(expected) .. ", got " .. tostring(actual)
        error(msg)
    end
end

--- Assert values are NOT equal
function test_utils.assert_not_equal(actual, unexpected, message)
    if actual == unexpected then
        local msg = message or ""
        if msg ~= "" then msg = msg .. ": " end
        msg = msg .. "should not equal " .. tostring(unexpected)
        error(msg)
    end
end

--- Assert binary values are equal (with hex formatting)
function test_utils.assert_byte_equal(actual, expected, message)
    actual = actual & 0xFF
    expected = expected & 0xFF
    if actual ~= expected then
        local msg = message or ""
        if msg ~= "" then msg = msg .. ": " end
        msg = msg .. "expected " .. stdlib.hex(expected) .. ", got " .. stdlib.hex(actual)
        error(msg)
    end
end

--- Assert 16-bit values are equal
function test_utils.assert_word_equal(actual, expected, message)
    actual = actual & 0xFFFF
    expected = expected & 0xFFFF
    if actual ~= expected then
        local msg = message or ""
        if msg ~= "" then msg = msg .. ": " end
        msg = msg .. "expected " .. stdlib.hex(expected, 4) .. ", got " .. stdlib.hex(actual, 4)
        error(msg)
    end
end

--- Assert condition is true
function test_utils.assert_true(condition, message)
    if not condition then
        error(message or "Expected true")
    end
end

--- Assert condition is false
function test_utils.assert_false(condition, message)
    if condition then
        error(message or "Expected false")
    end
end

--- Assert value is in expected range
function test_utils.assert_in_range(value, min, max, message)
    if value < min or value > max then
        local msg = message or ""
        if msg ~= "" then msg = msg .. ": " end
        msg = msg .. value .. " not in [" .. min .. ", " .. max .. "]"
        error(msg)
    end
end

--- Assert specific bits are set
function test_utils.assert_bits_set(value, bits, message)
    local missing = bits & ~value
    if missing ~= 0 then
        local msg = message or ""
        if msg ~= "" then msg = msg .. ": " end
        msg = msg .. "bits not set: " .. stdlib.binary(missing)
        error(msg)
    end
end

--- Assert specific bits are clear
function test_utils.assert_bits_clear(value, bits, message)
    local set = bits & value
    if set ~= 0 then
        local msg = message or ""
        if msg ~= "" then msg = msg .. ": " end
        msg = msg .. "bits not clear: " .. stdlib.binary(set)
        error(msg)
    end
end

--- Assert pattern matches (like sprintf)
function test_utils.assert_pattern(value, pattern, message)
    if not string.match(tostring(value), pattern) then
        local msg = message or ""
        if msg ~= "" then msg = msg .. ": " end
        msg = msg .. "'" .. tostring(value) .. "' does not match pattern '" .. pattern .. "'"
        error(msg)
    end
end

-- ============================================================================
-- Memory Assertion Utilities
-- ============================================================================

--- Assert memory region contains pattern
function test_utils.assert_memory(backend, addr, expected_bytes, message)
    for offset, expected in ipairs(expected_bytes) do
        local actual = backend:read_byte(addr + offset - 1)
        if actual ~= expected then
            local msg = message or ""
            if msg ~= "" then msg = msg .. ": " end
            msg = msg .. "at " .. stdlib.hex(addr + offset - 1) .. ": "
            msg = msg .. "expected " .. stdlib.hex(expected) .. ", got " .. stdlib.hex(actual)
            error(msg)
        end
    end
end

--- Assert memory block is filled with value
function test_utils.assert_memory_filled(backend, start_addr, size, fill_value, message)
    for i = 0, size - 1 do
        local actual = backend:read_byte(start_addr + i)
        if actual ~= fill_value then
            local msg = message or ""
            if msg ~= "" then msg = msg .. ": " end
            msg = msg .. "at offset " .. i .. " (" .. stdlib.hex(start_addr + i) .. "): "
            msg = msg .. "expected " .. stdlib.hex(fill_value) .. ", got " .. stdlib.hex(actual)
            error(msg)
        end
    end
end

-- ============================================================================
-- Test Result Tracking
-- ============================================================================

local TestResults = {}
TestResults.__index = TestResults

function TestResults.new()
    local self = setmetatable({}, TestResults)
    self.passed = 0
    self.failed = 0
    self.skipped = 0
    self.errors = {}
    return self
end

function TestResults:record_pass(test_name)
    self.passed = self.passed + 1
end

function TestResults:record_fail(test_name, error_msg)
    self.failed = self.failed + 1
    table.insert(self.errors, {name = test_name, message = error_msg})
end

function TestResults:record_skip(test_name, reason)
    self.skipped = self.skipped + 1
end

function TestResults:total()
    return self.passed + self.failed + self.skipped
end

function TestResults:success_rate()
    local total = self:total()
    if total == 0 then return 100 end
    return math.floor((self.passed / total) * 100)
end

function TestResults:all_passed()
    return self.failed == 0
end

function TestResults:print_summary()
    print("")
    print("================================================")
    print("Test Results Summary")
    print("================================================")
    print("")
    print("Passed:  " .. self.passed)
    print("Failed:  " .. self.failed)
    print("Skipped: " .. self.skipped)
    print("Total:   " .. self:total())
    print("Success: " .. self:success_rate() .. "%")
    print("")

    if self.failed > 0 then
        print("Failed Tests:")
        for _, err in ipairs(self.errors) do
            print("  - " .. err.name)
            print("    " .. err.message)
        end
        print("")
    end

    if self:all_passed() then
        print("✓ ALL TESTS PASSED")
    else
        print("✗ SOME TESTS FAILED")
    end
    print("")
end

test_utils.TestResults = TestResults

-- ============================================================================
-- Test Runner Utilities
-- ============================================================================

--- Run a single test with error handling
-- @param test_name: Name of test
-- @param test_func: Function to run
-- @param results: TestResults object
-- @return true if passed, false if failed
function test_utils.run_test(test_name, test_func, results)
    results = results or TestResults.new()

    print("[ RUN      ] " .. test_name)

    local ok, err = pcall(test_func)
    if ok then
        print("[       OK ] " .. test_name)
        results:record_pass(test_name)
        return true
    else
        print("[    FAIL  ] " .. test_name)
        print("  Error: " .. tostring(err))
        results:record_fail(test_name, err)
        return false
    end
end

--- Run multiple tests
-- @param tests: Table of {name, function} pairs
-- @return TestResults object
function test_utils.run_tests(tests)
    local results = TestResults.new()

    print("")
    print("================================================")
    print("Running " .. #tests .. " tests")
    print("================================================")
    print("")

    for _, test in ipairs(tests) do
        test_utils.run_test(test[1], test[2], results)
    end

    print("")
    results:print_summary()

    return results
end

-- ============================================================================
-- Test Helpers
-- ============================================================================

--- Create a test that verifies memory pattern
function test_utils.memory_pattern_test(backend, addr, pattern, label)
    return function()
        test_utils.assert_memory(backend, addr, pattern, label)
    end
end

--- Create a test that verifies memory fill
function test_utils.memory_fill_test(backend, addr, size, fill_value, label)
    return function()
        test_utils.assert_memory_filled(backend, addr, size, fill_value, label)
    end
end

--- Create a test that verifies register value
function test_utils.register_test(backend, reg_name, expected_value, label)
    return function()
        local actual = backend:get_register(reg_name)
        test_utils.assert_byte_equal(actual, expected_value, label or ("Register " .. reg_name))
    end
end

-- ============================================================================
-- Version & Export
-- ============================================================================

test_utils.VERSION = "1.0.0"

return test_utils
