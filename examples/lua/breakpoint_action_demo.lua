-- Breakpoint Action Execution Demo
-- Demonstrates Lua code execution when breakpoints are hit
-- This script is meant to be set as a breakpoint action via:
--   break $2000 action "mmemu.log('Entered main routine')"

-- Example 1: Simple logging on breakpoint
mmemu.log("=== Breakpoint Action Demo ===")
mmemu.log("This Lua code executed automatically when the breakpoint was hit!")

-- Example 2: Conditional breakpoint action
-- Useful pattern: log state only if conditions are met
local a = mmemu.get_register("A")
if a == 0x00 then
    mmemu.log("Accumulator is zero - checking for initialization")
end

-- Example 3: State inspection on breakpoint
mmemu.log("Current state:")
mmemu.log("  PC: $" .. string.format("%04X", mmemu.get_pc()))
mmemu.log("  A: $" .. string.format("%02X", mmemu.get_register("A")))
mmemu.log("  X: $" .. string.format("%02X", mmemu.get_register("X")))
mmemu.log("  Y: $" .. string.format("%02X", mmemu.get_register("Y")))

-- Example 4: Memory inspection at breakpoint
mmemu.log("\nZero page peek:")
for i = 0, 7 do
    local val = mmemu.read_byte(i)
    mmemu.log("  $" .. string.format("%02X", i) .. ": $" .. string.format("%02X", val))
end

mmemu.log("\n=== Breakpoint Action Complete ===")
