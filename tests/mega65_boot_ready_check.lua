--[[
MEGA65 Boot Ready Prompt Test (Lua)

This script can be used as a cycle event to check if MEGA65 boots to Ready prompt.
Usage in mmemu-cli:
  create mega65
  brktrap off
  break $E4B8 action "... load this script ..."
  run

Or run directly with Lua 5.4 and the mmemu backend API.
]]

local stdlib = require("stdlib")
local device_io = require("device_io")
local test_utils = require("test_utils")

-- Configuration
local SCREEN_ADDR = 0x0400
local SCREEN_WIDTH = 40
local SCREEN_HEIGHT = 25
local MAX_BOOT_CYCLES = 1000000
local CHECK_INTERVAL = 50000

-- Commodore screen codes for "READY"
-- These are the actual byte values in screen memory
local function string_at_address(backend, addr, length)
    local str = ""
    for i = 0, length - 1 do
        local byte = backend.peek8(addr + i)
        -- Convert Commodore codes to ASCII-like display
        if byte >= 32 and byte < 127 then
            str = str .. string.char(byte)
        elseif byte == 0 then
            str = str .. "."
        else
            str = str .. "?"
        end
    end
    return str
end

-- Check screen for "Ready" text (case-insensitive)
local function screen_contains_ready(backend)
    local screen_text = string_at_address(backend, SCREEN_ADDR, SCREEN_WIDTH * SCREEN_HEIGHT)

    -- Search for "READY" in various cases
    if string.find(string.upper(screen_text), "READY") then
        return true
    end
    return false
end

-- Print screen contents
local function print_screen(backend, num_lines)
    num_lines = num_lines or 5
    print("Screen contents (first " .. num_lines .. " lines):")
    for line = 0, num_lines - 1 do
        local addr = SCREEN_ADDR + (line * SCREEN_WIDTH)
        local line_text = string_at_address(backend, addr, SCREEN_WIDTH)
        print(string.format("  Line %d: %s", line, line_text))
    end
end

-- Main test function
local function test_mega65_boot(backend, initial_cycles)
    initial_cycles = initial_cycles or 0

    print("========================================")
    print("MEGA65 Boot Ready Prompt Test")
    print("========================================")
    print("Starting cycles: " .. initial_cycles)
    print("Max boot cycles: " .. MAX_BOOT_CYCLES)
    print("")

    local cycles_executed = 0
    local start_time = os.time()

    -- Polling loop - check screen memory periodically
    while cycles_executed < MAX_BOOT_CYCLES do
        local current_cycles = backend.cycles() - initial_cycles

        -- Check for Ready prompt
        if screen_contains_ready(backend) then
            local elapsed = os.time() - start_time
            print("✓ SUCCESS!")
            print("✓ Ready prompt detected at cycle " .. current_cycles)
            print("✓ Elapsed time: " .. elapsed .. " seconds")
            print("")
            print_screen(backend, 5)
            return true
        end

        -- Print progress
        if (cycles_executed % CHECK_INTERVAL) == 0 and cycles_executed > 0 then
            print(string.format("Checked %d cycles, Current PC: 0x%04X",
                current_cycles, backend.pc()))
        end

        cycles_executed = cycles_executed + 1

        -- Safety: don't loop forever
        if cycles_executed >= MAX_BOOT_CYCLES then
            break
        end
    end

    -- Failed - Ready not found
    print("✗ FAILED!")
    print("✗ Ready prompt not found after " .. cycles_executed .. " cycles")
    print("")
    print_screen(backend, 5)
    print("")
    print("Final CPU state:")
    print("  PC: 0x" .. string.format("%04X", backend.pc()))
    print("  Cycles: " .. backend.cycles())
    return false
end

-- Export for use as a module
return {
    test = test_mega65_boot,
    screen_contains_ready = screen_contains_ready,
    print_screen = print_screen
}
