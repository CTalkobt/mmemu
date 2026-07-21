#!/usr/bin/env lua5.4
--[[
MEGA65 Boot Sequence Test
Tests that MEGA65 reaches the "Ready" prompt during boot sequence.

This test:
1. Creates a MEGA65 machine
2. Runs the boot sequence for up to 1 million cycles
3. Checks screen memory for the "Ready" prompt
4. Reports success or failure
]]

-- Test configuration
local MAX_CYCLES = 1000000
local CHECK_INTERVAL = 50000  -- Check every 50k cycles
local SCREEN_ADDR = 0x0400    -- Standard C64/MEGA65 screen memory
local SCREEN_SIZE = 1000      -- 40x25 = 1000 bytes

-- Character codes for "READY" (Commodore screen codes)
-- Note: These are the actual character codes, not ASCII
local READY_CHARS = {
    0x52,  -- R
    0x45,  -- E
    0x41,  -- A
    0x44,  -- D
    0x59   -- Y
}

-- Helper function to check if screen contains "Ready" text
local function check_for_ready(backend)
    -- Check various positions in screen memory
    for start_addr = SCREEN_ADDR, SCREEN_ADDR + SCREEN_SIZE - 5 do
        local match = true
        for i, expected_char in ipairs(READY_CHARS) do
            local actual_char = backend.peek8(start_addr + i - 1)
            if actual_char ~= expected_char then
                match = false
                break
            end
        end
        if match then
            return true, start_addr
        end
    end
    return false, nil
end

-- Helper function to read screen memory as ASCII
local function read_screen_line(backend, line_num)
    local start = SCREEN_ADDR + (line_num * 40)
    local line = ""
    for i = 0, 39 do
        local char_code = backend.peek8(start + i)
        -- Convert Commodore screen codes to printable ASCII
        if char_code >= 0x20 and char_code <= 0x7E then
            line = line .. string.char(char_code)
        elseif char_code >= 0x41 and char_code <= 0x5A then
            line = line .. string.char(char_code)
        elseif char_code >= 0x61 and char_code <= 0x7A then
            line = line .. string.char(char_code)
        else
            line = line .. "."
        end
    end
    return line
end

-- Main test
print("========================================")
print("MEGA65 Boot Ready Prompt Test")
print("========================================")
print("")

-- Create MEGA65 machine via CLI
local mmemu = require("mmemu")
if not mmemu then
    print("ERROR: mmemu module not found")
    os.exit(1)
end

print("Creating MEGA65 machine...")
local backend = mmemu.create_machine("mega65")
if not backend then
    print("ERROR: Failed to create MEGA65 machine")
    os.exit(1)
end

print("Starting boot sequence...")
print("Running up to " .. MAX_CYCLES .. " cycles...")
print("")

local cycles_executed = 0
local found_ready = false
local ready_addr = nil
local checks = 0

-- Run boot sequence with periodic checks
while cycles_executed < MAX_CYCLES do
    -- Execute steps
    local steps_to_run = math.min(CHECK_INTERVAL, MAX_CYCLES - cycles_executed)

    -- Use the backend to run steps (adjust based on actual API)
    for i = 1, steps_to_run do
        if backend.step then
            backend.step()
        else
            -- Fallback: assume backend has different interface
            break
        end
        cycles_executed = cycles_executed + 1
    end

    checks = checks + 1

    -- Check for Ready prompt
    found_ready, ready_addr = check_for_ready(backend)

    -- Print status
    print(string.format("[Check %3d] Cycles: %7d - ", checks, cycles_executed), end="")

    if found_ready then
        print("FOUND READY at $" .. string.format("%04X", ready_addr))
        break
    else
        -- Print first line of screen for debugging
        local line = read_screen_line(backend, 0)
        print("Screen: " .. line:sub(1, 40))
    end

    -- Prevent infinite loop
    if cycles_executed >= MAX_CYCLES then
        break
    end
end

print("")
print("========================================")
print("Test Results")
print("========================================")

if found_ready then
    print("✓ SUCCESS: Ready prompt found at address $" .. string.format("%04X", ready_addr))
    print("✓ Boot sequence completed successfully")
    print("✓ Cycles executed: " .. cycles_executed)
    print("")
    print("First few lines of screen:")
    for i = 0, 4 do
        print("  Line " .. i .. ": " .. read_screen_line(backend, i))
    end
    os.exit(0)
else
    print("✗ FAILED: Ready prompt not found after " .. cycles_executed .. " cycles")
    print("✗ Boot sequence did not complete")
    print("")
    print("Last screen state:")
    for i = 0, 4 do
        print("  Line " .. i .. ": " .. read_screen_line(backend, i))
    end
    print("")
    print("CPU State at halt:")
    print("  PC: $" .. string.format("%04X", backend.pc or 0))
    print("  Cycles: " .. cycles_executed)
    os.exit(1)
end
