-- Test script for VS Code extension
print("================================")
print("mmemu Lua - VS Code Test Script")
print("================================")
print("")

-- Test 1: Basic operations
print("Test 1: Basic Memory Operations")
local test_addr = 0x100
mmemu.write_byte(test_addr, 0x42)
local value = mmemu.read_byte(test_addr)
print(string.format("  Wrote 0x42 to $%04X", test_addr))
print(string.format("  Read back: $%02X", value))
if value == 0x42 then
    print("  ✓ PASSED")
else
    print("  ✗ FAILED")
end
print("")

-- Test 2: Registers
print("Test 2: Register Operations")
mmemu.set_register("A", 0x55)
mmemu.set_register("X", 0xAA)
local a_val = mmemu.get_register("A")
local x_val = mmemu.get_register("X")
print(string.format("  Set A = $55, X = $AA"))
print(string.format("  Read A = $%02X, X = $%02X", a_val, x_val))
if a_val == 0x55 and x_val == 0xAA then
    print("  ✓ PASSED")
else
    print("  ✗ FAILED")
end
print("")

-- Test 3: PC manipulation
print("Test 3: Program Counter")
local pc = mmemu.get_pc()
print(string.format("  Current PC: $%04X", pc))
print("  ✓ PC readable")
print("")

-- Test 4: stdlib utilities
print("Test 4: stdlib Utilities")
local stdlib = require("stdlib")
local hex_str = stdlib.hex(0xFF)
local bin_str = stdlib.binary(0x55)
print(string.format("  Hex 0xFF: %s", hex_str))
print(string.format("  Binary 0x55: %s", bin_str))
if hex_str == "$FF" and bin_str == "01010101" then
    print("  ✓ PASSED")
else
    print("  ✗ FAILED")
end
print("")

-- Test 5: device_io availability
print("Test 5: Device I/O Module")
local device_io = require("device_io")
if device_io.SID_set_frequency and device_io.VIC_set_sprite_pos then
    print("  ✓ All device I/O functions available")
else
    print("  ✗ Missing device functions")
end
print("")

-- Test 6: Profiler
print("Test 6: Performance Profiler")
local profiler = require("profiler")
local prof = profiler.Profiler.new()
prof:start("test")
for i = 1, 100 do end
local elapsed = prof:stop()
print(string.format("  Profiler worked, elapsed: %d ms", elapsed))
print("  ✓ PASSED")
print("")

print("================================")
print("✓ All tests completed!")
print("================================")
