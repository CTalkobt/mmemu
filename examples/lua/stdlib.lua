-- Standard Library for mmemu Lua Scripts
-- Issue #24 Phase 6.1: Script Library & Utilities
--
-- Common helper functions for test writing and scripting

local stdlib = {}

-- ============================================================================
-- String & Formatting Utilities
-- ============================================================================

--- Format number as hexadecimal string
-- @param value: Number to format
-- @param width: Optional field width (default 2 for bytes)
-- @return hex string with $ prefix (e.g., "$FF")
function stdlib.hex(value, width)
    width = width or 2
    return "$" .. string.format("%0" .. width .. "X", value & ((1 << (width * 4)) - 1))
end

--- Format byte range as hex string
-- @param bytes: Table of byte values
-- @return space-separated hex string (e.g., "FF 42 00")
function stdlib.hex_bytes(bytes)
    local result = {}
    for _, b in ipairs(bytes) do
        table.insert(result, string.format("%02X", b & 0xFF))
    end
    return table.concat(result, " ")
end

--- Parse hex string to number (with or without $ prefix)
-- @param str: Hex string (e.g., "$FF" or "FF")
-- @return number or nil
function stdlib.parse_hex(str)
    if not str then return nil end
    str = str:gsub("^%$", "")  -- Remove $ prefix if present
    local num = tonumber(str, 16)
    return num
end

--- Format number in binary
-- @param value: Number to format
-- @param width: Optional field width in bits
-- @return binary string (e.g., "11111111")
function stdlib.binary(value, width)
    width = width or 8
    local result = ""
    for i = width - 1, 0, -1 do
        result = result .. (((value >> i) & 1) == 1 and "1" or "0")
    end
    return result
end

--- Format CPU flags register as readable string
-- @param flags: Flags register value (8 bits)
-- @return string like "N-B-D-IZC" (set flags uppercase, clear lowercase)
function stdlib.format_flags(flags)
    local names = {'N', 'V', '-', 'B', 'D', 'I', 'Z', 'C'}
    local result = {}
    for i = 7, 0, -1 do
        local bit = (flags >> i) & 1
        if bit == 1 then
            table.insert(result, names[8 - i]:upper())
        else
            table.insert(result, names[8 - i]:lower())
        end
    end
    return table.concat(result, "")
end

-- ============================================================================
-- Table & Collection Utilities
-- ============================================================================

--- Create a table from an array of key-value pairs
-- @param pairs: Table of {key, value} pairs
-- @return Table with key-value mappings
function stdlib.table_from_pairs(pairs)
    local result = {}
    for _, pair in ipairs(pairs) do
        result[pair[1]] = pair[2]
    end
    return result
end

--- Invert a table (swap keys and values)
-- @param tbl: Table to invert
-- @return Inverted table
function stdlib.table_invert(tbl)
    local result = {}
    for k, v in pairs(tbl) do
        result[v] = k
    end
    return result
end

--- Count elements in a table
-- @param tbl: Table to count
-- @return Number of elements
function stdlib.table_count(tbl)
    local count = 0
    for _ in pairs(tbl) do
        count = count + 1
    end
    return count
end

--- Merge multiple tables into one
-- @param ...: Variable number of tables
-- @return Merged table
function stdlib.table_merge(...)
    local result = {}
    for _, tbl in ipairs({...}) do
        for k, v in pairs(tbl) do
            result[k] = v
        end
    end
    return result
end

--- Filter table elements by predicate
-- @param tbl: Table to filter
-- @param predicate: Function(value, key) -> boolean
-- @return Filtered table
function stdlib.table_filter(tbl, predicate)
    local result = {}
    for k, v in pairs(tbl) do
        if predicate(v, k) then
            result[k] = v
        end
    end
    return result
end

--- Map table values through a function
-- @param tbl: Table to map
-- @param mapper: Function(value, key) -> new_value
-- @return Mapped table
function stdlib.table_map(tbl, mapper)
    local result = {}
    for k, v in pairs(tbl) do
        result[k] = mapper(v, k)
    end
    return result
end

--- Convert array to comma-separated string
-- @param arr: Array table
-- @param separator: Optional separator (default ", ")
-- @return String
function stdlib.array_join(arr, separator)
    separator = separator or ", "
    return table.concat(arr, separator)
end

-- ============================================================================
-- Assertion & Validation Utilities
-- ============================================================================

--- Assert that a condition is true
-- @param condition: Boolean condition
-- @param message: Error message if false
function stdlib.assert(condition, message)
    if not condition then
        error(message or "Assertion failed")
    end
end

--- Assert equality with readable diff
-- @param actual: Actual value
-- @param expected: Expected value
-- @param message: Optional message prefix
function stdlib.assert_eq(actual, expected, message)
    if actual ~= expected then
        local msg = message or "Assertion failed"
        msg = msg .. ": expected " .. tostring(expected) .. ", got " .. tostring(actual)
        error(msg)
    end
end

--- Assert hex equality with formatting
-- @param actual: Actual value
-- @param expected: Expected value (hex)
-- @param message: Optional message prefix
function stdlib.assert_hex_eq(actual, expected, message)
    if actual ~= expected then
        local msg = message or "Assertion failed"
        msg = msg .. ": expected " .. stdlib.hex(expected) .. ", got " .. stdlib.hex(actual)
        error(msg)
    end
end

--- Assert value is in range
-- @param value: Value to check
-- @param min: Minimum (inclusive)
-- @param max: Maximum (inclusive)
-- @param message: Optional message
function stdlib.assert_range(value, min, max, message)
    if value < min or value > max then
        local msg = message or "Range assertion failed"
        msg = msg .. ": " .. value .. " not in [" .. min .. ", " .. max .. "]"
        error(msg)
    end
end

--- Assert binary value matches pattern
-- @param value: Value to check
-- @param pattern: Pattern string (1 for set, 0 for clear, - for don't-care)
-- @param message: Optional message
function stdlib.assert_bits(value, pattern, message)
    for i = 1, #pattern do
        local bit_pos = #pattern - i
        local bit = (value >> bit_pos) & 1
        local expected = pattern:sub(i, i)

        if expected == '1' and bit ~= 1 then
            error((message or "Bit assertion") .. ": bit " .. bit_pos .. " should be set")
        elseif expected == '0' and bit ~= 0 then
            error((message or "Bit assertion") .. ": bit " .. bit_pos .. " should be clear")
        end
    end
end

-- ============================================================================
-- Bitwise Operation Utilities
-- ============================================================================

--- Extract a bitfield from a value
-- @param value: Value to extract from
-- @param start: Starting bit position (0-7 for bytes)
-- @param length: Number of bits to extract
-- @return Extracted value
function stdlib.bitfield(value, start, length)
    local mask = ((1 << length) - 1) << start
    return (value & mask) >> start
end

--- Set a bitfield in a value
-- @param value: Original value
-- @param field_value: Value to set
-- @param start: Starting bit position
-- @param length: Number of bits
-- @return Updated value
function stdlib.bitfield_set(value, field_value, start, length)
    local mask = ((1 << length) - 1) << start
    return (value & ~mask) | ((field_value & ((1 << length) - 1)) << start)
end

--- Rotate bits left
-- @param value: Value to rotate
-- @param amount: Number of positions to rotate (max 8 for bytes)
-- @return Rotated value
function stdlib.rotate_left(value, amount)
    amount = amount % 8
    return ((value << amount) | (value >> (8 - amount))) & 0xFF
end

--- Rotate bits right
-- @param value: Value to rotate
-- @param amount: Number of positions to rotate
-- @return Rotated value
function stdlib.rotate_right(value, amount)
    amount = amount % 8
    return ((value >> amount) | (value << (8 - amount))) & 0xFF
end

--- Count number of set bits
-- @param value: Value to count bits in
-- @return Number of set bits
function stdlib.popcount(value)
    local count = 0
    while value > 0 do
        count = count + (value & 1)
        value = value >> 1
    end
    return count
end

-- ============================================================================
-- Timing & Performance Utilities
-- ============================================================================

--- Get current time in milliseconds
-- @return Time in ms (relative, use for measurements)
function stdlib.time_ms()
    -- Note: os.clock() returns CPU time in seconds
    return math.floor(os.clock() * 1000)
end

--- Measure execution time of a function
-- @param func: Function to measure
-- @param ...: Arguments to pass to function
-- @return Execution time in ms, and function result
function stdlib.measure_time(func, ...)
    local start = stdlib.time_ms()
    local result = func(...)
    local elapsed = stdlib.time_ms() - start
    return elapsed, result
end

-- ============================================================================
-- Debug & Diagnostic Utilities
-- ============================================================================

--- Dump a table as formatted string
-- @param tbl: Table to dump
-- @param indent: Optional indentation level
-- @return Formatted string
function stdlib.table_dump(tbl, indent)
    indent = indent or 0
    local prefix = string.rep("  ", indent)
    local result = "{\n"

    for k, v in pairs(tbl) do
        result = result .. prefix .. "  [" .. tostring(k) .. "] = "

        if type(v) == "table" then
            result = result .. stdlib.table_dump(v, indent + 1) .. "\n"
        else
            result = result .. tostring(v) .. "\n"
        end
    end

    result = result .. prefix .. "}"
    return result
end

--- Print debug information with label
-- @param label: Debug label
-- @param value: Value to print
function stdlib.debug_print(label, value)
    if type(value) == "table" then
        print(label .. ":")
        print(stdlib.table_dump(value))
    else
        print(label .. ": " .. tostring(value))
    end
end

--- Assert with debug context
-- @param condition: Condition to test
-- @param label: Debug label
-- @param context: Context table to dump on failure
function stdlib.assert_debug(condition, label, context)
    if not condition then
        if context then
            print("Debug context:")
            print(stdlib.table_dump(context))
        end
        error("Assertion failed: " .. label)
    end
end

-- ============================================================================
-- Version & Export
-- ============================================================================

stdlib.VERSION = "1.0.0"
stdlib.AUTHOR = "mmemu Lua Framework"

return stdlib
