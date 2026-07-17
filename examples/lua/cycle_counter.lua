-- Cycle Counter - Track CPU execution
-- Demonstrates mmemu.on_cycle() for monitoring execution patterns

-- Global state
local total_cycles = 0
local instruction_count = 0
local cycle_samples = {}

-- Called every 1000 cycles
function on_cycle_1k()
    total_cycles = total_cycles + 1000
    instruction_count = instruction_count + 1

    local pc = mmemu.get_pc()
    table.insert(cycle_samples, {
        cycles = total_cycles,
        pc = pc
    })

    -- Keep only last 10 samples
    if #cycle_samples > 10 then
        table.remove(cycle_samples, 1)
    end
end

-- Called every 10000 cycles for status
function on_cycle_10k()
    mmemu.log("=== Cycle Status @ " .. total_cycles .. " ===")
    local a = mmemu.get_register("A")
    local x = mmemu.get_register("X")
    local y = mmemu.get_register("Y")
    local sp = mmemu.get_register("SP")

    mmemu.log("Registers: A=$" .. string.format("%02X", a) ..
              " X=$" .. string.format("%02X", x) ..
              " Y=$" .. string.format("%02X", y) ..
              " SP=$" .. string.format("%02X", sp))
end

-- Register event handlers
mmemu.on_cycle(1000, "on_cycle_1k")
mmemu.on_cycle(10000, "on_cycle_10k")

mmemu.log("=== Cycle Counter Active ===")
mmemu.log("Tracking cycles every 1k and logging every 10k")
mmemu.log("Use 'run 100000' to execute 100k cycles")
