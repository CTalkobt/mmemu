-- Memory filling and verification test
-- Demonstrates looping and memory operations

mmemu.log("=== Memory Fill Test ===")

-- Fill a memory region with a pattern
base_addr = 0x0200
length = 256
pattern = 0x55

mmemu.log("Filling $0200-$02FF with pattern $55...")
for i = 0, length - 1 do
    mmemu.write_byte(base_addr + i, pattern)
end

-- Verify the fill
mmemu.log("Verifying fill...")
errors = 0
for i = 0, length - 1 do
    val = mmemu.read_byte(base_addr + i)
    if val ~= pattern then
        mmemu.log("ERROR at offset " .. i .. ": expected $55, got $" .. string.format("%02X", val))
        errors = errors + 1
    end
end

if errors == 0 then
    mmemu.log("Verification passed - all " .. length .. " bytes match pattern")
else
    mmemu.log("Verification failed - " .. errors .. " mismatches")
end

-- Test XOR pattern
mmemu.log("\nFilling with XOR pattern...")
xor_base = 0x0300
for i = 0, 63 do
    val = (i * 0xAA) % 256
    mmemu.write_byte(xor_base + i, val)
end

-- Read back and show pattern
mmemu.log("Pattern at $0300:")
for i = 0, 15 do
    val = mmemu.read_byte(xor_base + i)
    mmemu.log("  $030" .. string.format("%X", i) .. ": " .. mmemu.hex(val))
end

mmemu.log("=== Test Complete ===")
