-- Simple test script for mmemu Lua scripting
-- Demonstrates basic memory and register access

-- Read and modify accumulator register
mmemu.log("=== Simple C64 Test ===")
mmemu.log("Reading current registers...")

-- Get current register values
pc = mmemu.get_pc()
a = mmemu.get_register("A")
x = mmemu.get_register("X")
y = mmemu.get_register("Y")

-- Log current state
mmemu.log("PC: " .. mmemu.hex(pc))
mmemu.log("A: " .. mmemu.hex(a))
mmemu.log("X: " .. mmemu.hex(x))
mmemu.log("Y: " .. mmemu.hex(y))

-- Modify accumulator and log change
mmemu.set_register("A", 0x42)
new_a = mmemu.get_register("A")
mmemu.log("Set A to: " .. mmemu.hex(new_a))

-- Write to zero page
mmemu.log("Writing to zero page...")
mmemu.write_byte(0x00, 0xAA)
mmemu.write_byte(0x01, 0xBB)

-- Read back and verify
val0 = mmemu.read_byte(0x00)
val1 = mmemu.read_byte(0x01)
mmemu.log("ZP $00: " .. mmemu.hex(val0))
mmemu.log("ZP $01: " .. mmemu.hex(val1))

mmemu.log("=== Test Complete ===")
