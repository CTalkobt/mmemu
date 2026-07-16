-- Regression Test Framework
-- Demonstrates automated test execution with Lua scripting

mmemu.log("=== Regression Test Suite ===")

-- Test 1: Zero Page Access
function test_zeropage_writes()
    mmemu.log("Test 1: Zero Page Write Pattern")

    -- Fill ZP with pattern
    for addr = 0x00, 0xFF do
        mmemu.write_byte(addr, (addr * 0xAA) % 256)
    end

    -- Verify pattern
    local errors = 0
    for addr = 0x00, 0xFF do
        local expected = (addr * 0xAA) % 256
        local actual = mmemu.read_byte(addr)
        if actual ~= expected then
            mmemu.log("ERROR at $" .. string.format("%02X", addr) ..
                      ": expected $" .. string.format("%02X", expected) ..
                      ", got $" .. string.format("%02X", actual))
            errors = errors + 1
        end
    end

    if errors == 0 then
        mmemu.log("✓ Test 1 PASSED: All ZP bytes verified")
        return true
    else
        mmemu.log("✗ Test 1 FAILED: " .. errors .. " errors")
        return false
    end
end

-- Test 2: Register Preservation
function test_register_preservation()
    mmemu.log("Test 2: Register Preservation")

    -- Set specific register values
    mmemu.set_register("A", 0x12)
    mmemu.set_register("X", 0x34)
    mmemu.set_register("Y", 0x56)

    -- Read them back
    local a = mmemu.get_register("A")
    local x = mmemu.get_register("X")
    local y = mmemu.get_register("Y")

    -- Verify
    local passed = (a == 0x12) and (x == 0x34) and (y == 0x56)

    if passed then
        mmemu.log("✓ Test 2 PASSED: Registers preserved correctly")
        mmemu.log("  A=$" .. string.format("%02X", a) ..
                  " X=$" .. string.format("%02X", x) ..
                  " Y=$" .. string.format("%02X", y))
        return true
    else
        mmemu.log("✗ Test 2 FAILED:")
        mmemu.log("  A=$" .. string.format("%02X", a) .. " (expected $12)")
        mmemu.log("  X=$" .. string.format("%02X", x) .. " (expected $34)")
        mmemu.log("  Y=$" .. string.format("%02X", y) .. " (expected $56)")
        return false
    end
end

-- Test 3: Memory Block Copy
function test_memory_copy()
    mmemu.log("Test 3: Memory Block Copy")

    local src_base = 0x0200
    local dst_base = 0x0300
    local size = 32

    -- Set up source pattern
    for i = 0, size - 1 do
        mmemu.write_byte(src_base + i, i * 2)
    end

    -- Copy
    for i = 0, size - 1 do
        local val = mmemu.read_byte(src_base + i)
        mmemu.write_byte(dst_base + i, val)
    end

    -- Verify copy
    local errors = 0
    for i = 0, size - 1 do
        local src = mmemu.read_byte(src_base + i)
        local dst = mmemu.read_byte(dst_base + i)
        if src ~= dst then
            errors = errors + 1
        end
    end

    if errors == 0 then
        mmemu.log("✓ Test 3 PASSED: " .. size .. " bytes copied correctly")
        return true
    else
        mmemu.log("✗ Test 3 FAILED: " .. errors .. " bytes mismatched")
        return false
    end
end

-- Run all tests
mmemu.log("")
mmemu.log("Running test suite...")
mmemu.log("")

local passed = 0
local failed = 0

if test_zeropage_writes() then passed = passed + 1 else failed = failed + 1 end
mmemu.log("")

if test_register_preservation() then passed = passed + 1 else failed = failed + 1 end
mmemu.log("")

if test_memory_copy() then passed = passed + 1 else failed = failed + 1 end
mmemu.log("")

-- Summary
mmemu.log("=== Test Results ===")
mmemu.log("Passed: " .. passed .. " / " .. (passed + failed))
mmemu.log("Failed: " .. failed .. " / " .. (passed + failed))

if failed == 0 then
    mmemu.log("✓ ALL TESTS PASSED")
else
    mmemu.log("✗ SOME TESTS FAILED")
end
