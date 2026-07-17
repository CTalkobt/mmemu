-- Common Memory Patterns for Testing
-- Issue #24 Phase 6.1: Memory Test Patterns
--
-- Pre-built memory test patterns and generators

local stdlib = require("stdlib")
local patterns = {}

-- ============================================================================
-- Static Patterns
-- ============================================================================

-- Basic patterns (single byte repeated)
patterns.ZERO_FILL = 0x00
patterns.ONES_FILL = 0xFF
patterns.ALTERNATING_55 = 0x55  -- 0101 0101
patterns.ALTERNATING_AA = 0xAA  -- 1010 1010

-- Common memory test patterns
patterns.WALKING_ONE = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80}
patterns.WALKING_ZERO = {0xFE, 0xFD, 0xFB, 0xF7, 0xEF, 0xDF, 0xBF, 0x7F}
patterns.COUNT_UP = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F}
patterns.COUNT_DOWN = {0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00}

-- ============================================================================
-- Pattern Generators
-- ============================================================================

--- Generate address pattern (address mod 256 at each location)
-- @param size: Size of pattern
-- @param start_addr: Starting address (default 0)
-- @return Pattern array
function patterns.address_pattern(size, start_addr)
    start_addr = start_addr or 0
    local result = {}
    for i = 0, size - 1 do
        table.insert(result, (start_addr + i) & 0xFF)
    end
    return result
end

--- Generate XOR pattern based on address
-- @param size: Size of pattern
-- @param start_addr: Starting address
-- @param xor_mask: XOR mask value (default 0xAA)
-- @return Pattern array
function patterns.xor_pattern(size, start_addr, xor_mask)
    start_addr = start_addr or 0
    xor_mask = xor_mask or 0xAA
    local result = {}
    for i = 0, size - 1 do
        table.insert(result, ((start_addr + i) & 0xFF) ~ xor_mask)
    end
    return result
end

--- Generate Fibonacci pattern
-- @param size: Number of values
-- @return Pattern array
function patterns.fibonacci_pattern(size)
    local result = {0x01, 0x01}
    for i = 3, size do
        local next = (result[i-1] + result[i-2]) & 0xFF
        table.insert(result, next)
    end
    return result
end

--- Generate pseudo-random pattern (linear congruential generator)
-- @param size: Size of pattern
-- @param seed: Starting seed (default 1)
-- @return Pattern array
function patterns.random_pattern(size, seed)
    seed = seed or 1
    local result = {}
    local a = 1103515245
    local c = 12345
    local m = 2147483648
    local x = seed

    for i = 1, size do
        x = ((a * x + c) % m) & 0xFF
        table.insert(result, x)
    end
    return result
end

--- Generate checkerboard pattern
-- @param size: Size of pattern
-- @return Pattern array (alternates 0x55 and 0xAA)
function patterns.checkerboard_pattern(size)
    local result = {}
    for i = 1, size do
        table.insert(result, i % 2 == 1 and 0x55 or 0xAA)
    end
    return result
end

--- Generate stripe pattern
-- @param size: Size of pattern
-- @param stripe_width: Width of each stripe (default 4)
-- @return Pattern array
function patterns.stripe_pattern(size, stripe_width)
    stripe_width = stripe_width or 4
    local result = {}
    local pattern = {0x00, 0xFF}
    local pattern_idx = 1

    for i = 1, size do
        table.insert(result, pattern[pattern_idx])
        if (i % stripe_width) == 0 then
            pattern_idx = pattern_idx == 1 and 2 or 1
        end
    end
    return result
end

-- ============================================================================
-- Memory Test Suites
-- ============================================================================

--- Standard RAM test pattern suite
patterns.RAM_TEST_SUITE = {
    {name = "zeros", value = 0x00, description = "All zeros"},
    {name = "ones", value = 0xFF, description = "All ones"},
    {name = "alternating_55", value = 0x55, description = "Alternating bits (0101 0101)"},
    {name = "alternating_aa", value = 0xAA, description = "Alternating bits (1010 1010)"},
}

--- Comprehensive memory test patterns
function patterns.comprehensive_suite(size)
    return {
        {name = "zeros", pattern = stdlib.table_map({}, function() return 0x00 end)},
        {name = "ones", pattern = stdlib.table_map({}, function() return 0xFF end)},
        {name = "address", pattern = patterns.address_pattern(size)},
        {name = "xor", pattern = patterns.xor_pattern(size, 0, 0xAA)},
        {name = "random", pattern = patterns.random_pattern(size, 42)},
        {name = "checkerboard", pattern = patterns.checkerboard_pattern(size)},
    }
end

-- ============================================================================
-- Pattern Verification
-- ============================================================================

--- Verify memory matches pattern
-- @param backend: Backend instance
-- @param addr: Starting address
-- @param pattern: Expected pattern (array of bytes or single byte)
-- @return true if match, false otherwise
function patterns.verify(backend, addr, pattern)
    if type(pattern) == "number" then
        -- Single byte pattern - fill entire region
        -- Need to know size - caller should use verify_array instead
        error("Use verify_array for single-byte patterns (need to specify size)")
    end

    for offset, expected in ipairs(pattern) do
        local actual = backend:read_byte(addr + offset - 1)
        if actual ~= expected then
            return false
        end
    end
    return true
end

--- Verify memory matches byte array pattern
-- @param backend: Backend instance
-- @param addr: Starting address
-- @param pattern: Array of expected bytes
-- @return true if match, false otherwise
-- @return index of first mismatch (nil if matched)
function patterns.verify_array(backend, addr, pattern)
    for offset, expected in ipairs(pattern) do
        local actual = backend:read_byte(addr + offset - 1)
        if actual ~= expected then
            return false, offset
        end
    end
    return true
end

--- Fill memory with pattern
-- @param backend: Backend instance
-- @param addr: Starting address
-- @param pattern: Pattern (array or single byte)
-- @return number of bytes written
function patterns.fill(backend, addr, pattern)
    if type(pattern) == "number" then
        error("Use fill_array for single-byte patterns")
    end

    for offset, value in ipairs(pattern) do
        backend:write_byte(addr + offset - 1, value)
    end
    return #pattern
end

--- Fill memory with single repeated byte
-- @param backend: Backend instance
-- @param addr: Starting address
-- @param size: Number of bytes
-- @param value: Byte value
-- @return number of bytes written
function patterns.fill_byte(backend, addr, size, value)
    for i = 0, size - 1 do
        backend:write_byte(addr + i, value)
    end
    return size
end

--- Compare two memory regions
-- @param backend: Backend instance
-- @param addr1: First address
-- @param addr2: Second address
-- @param size: Size to compare
-- @return true if regions match, false otherwise
-- @return first differing offset (nil if matched)
function patterns.compare(backend, addr1, addr2, size)
    for i = 0, size - 1 do
        local val1 = backend:read_byte(addr1 + i)
        local val2 = backend:read_byte(addr2 + i)
        if val1 ~= val2 then
            return false, i
        end
    end
    return true
end

--- Generate hex dump string of memory
-- @param backend: Backend instance
-- @param addr: Starting address
-- @param size: Number of bytes to dump
-- @param bytes_per_line: Bytes per line (default 16)
-- @return Formatted dump string
function patterns.hex_dump(backend, addr, size, bytes_per_line)
    bytes_per_line = bytes_per_line or 16
    local result = {}

    for line = 0, math.ceil(size / bytes_per_line) - 1 do
        local line_addr = addr + (line * bytes_per_line)
        local line_str = stdlib.hex(line_addr, 4) .. ":"

        -- Hex bytes
        for col = 0, bytes_per_line - 1 do
            if line * bytes_per_line + col < size then
                local byte_val = backend:read_byte(line_addr + col)
                line_str = line_str .. " " .. stdlib.hex(byte_val)
            else
                line_str = line_str .. "    "
            end
        end

        -- ASCII
        line_str = line_str .. "  "
        for col = 0, bytes_per_line - 1 do
            if line * bytes_per_line + col < size then
                local byte_val = backend:read_byte(line_addr + col)
                if byte_val >= 32 and byte_val < 127 then
                    line_str = line_str .. string.char(byte_val)
                else
                    line_str = line_str .. "."
                end
            end
        end

        table.insert(result, line_str)
    end

    return table.concat(result, "\n")
end

-- ============================================================================
-- Version & Export
-- ============================================================================

patterns.VERSION = "1.0.0"

return patterns
