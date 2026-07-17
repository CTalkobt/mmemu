-- Snapshot Checkpoints - Phase 4.3
-- Demonstrates mmemu.save_snapshot() and mmemu.load_snapshot()
-- for creating machine state checkpoints

-- Checkpoint tracking
local checkpoints = {}
local test_count = 0
local test_passed = 0

-- Save initial machine state
function create_checkpoint(name)
    mmemu.log("Creating checkpoint: " .. name)
    local snap_id = mmemu.save_snapshot(name)
    checkpoints[name] = snap_id
    mmemu.log("  Checkpoint ID: " .. snap_id)
    return snap_id
end

-- Restore to checkpoint
function restore_checkpoint(name)
    if checkpoints[name] == nil then
        mmemu.log("ERROR: Checkpoint '" .. name .. "' not found")
        return false
    end

    mmemu.log("Restoring checkpoint: " .. name)
    local snap_id = checkpoints[name]
    local success = mmemu.load_snapshot(snap_id)

    if success then
        mmemu.log("  ✓ Restored successfully")
    else
        mmemu.log("  ✗ Restore failed")
    end

    return success
end

-- List all snapshots
function list_all_snapshots()
    mmemu.log("=== All Snapshots ===")
    local snapshots = mmemu.list_snapshots()

    if #snapshots == 0 then
        mmemu.log("(none)")
        return
    end

    for i = 1, #snapshots do
        local snap = snapshots[i]
        mmemu.log("  [" .. snap.id .. "] " .. snap.label)
    end
end

-- Run a test and auto-restore on failure
function test_with_checkpoint(test_name, test_func)
    test_count = test_count + 1

    mmemu.log("")
    mmemu.log("=== Test " .. test_count .. ": " .. test_name .. " ===")

    -- Save state before test
    local state_id = mmemu.save_snapshot(test_name)

    -- Run test
    local success, result = pcall(test_func)

    if success and result then
        mmemu.log("✓ PASSED: " .. test_name)
        test_passed = test_passed + 1
    else
        mmemu.log("✗ FAILED: " .. test_name)
        if result then
            mmemu.log("  Error: " .. result)
        end
        -- Optionally restore state
        mmemu.load_snapshot(state_id)
    end
end

-- Example test function
function verify_memory_pattern()
    -- Test: Check if first 16 bytes match pattern
    local pattern_good = true
    for i = 0, 15 do
        local val = mmemu.read_byte(i)
        if val ~= (i * 2) % 256 then
            mmemu.log("  Byte $" .. string.format("%02X", i) .. " = $" ..
                      string.format("%02X", val) .. " (expected $" ..
                      string.format("%02X", (i * 2) % 256) .. ")")
            pattern_good = false
        end
    end
    return pattern_good
end

-- Example test function 2
function verify_stack_initialized()
    local sp = mmemu.get_register("SP")
    if sp == 0xFF then
        mmemu.log("  Stack pointer initialized correctly: $FF")
        return true
    else
        mmemu.log("  Stack pointer incorrect: $" .. string.format("%02X", sp))
        return false
    end
end

-- Demo sequence
mmemu.log("=== Snapshot Checkpoint Demo ===")
mmemu.log("")

-- Create initial checkpoint
create_checkpoint("startup")

-- Run tests with auto-restore
test_with_checkpoint("Memory Pattern", verify_memory_pattern)
test_with_checkpoint("Stack Initialization", verify_stack_initialized)

-- Restore to known good state
restore_checkpoint("startup")

-- Print summary
mmemu.log("")
mmemu.log("=== Test Results ===")
mmemu.log("Passed: " .. test_passed .. " / " .. test_count)
mmemu.log("")

-- List all snapshots
list_all_snapshots()

mmemu.log("")
mmemu.log("Checkpoint system ready!")
mmemu.log("Use create_checkpoint(), restore_checkpoint() in your tests")
