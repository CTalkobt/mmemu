-- Performance Monitor - Analyze execution patterns
-- Combines cycle events and memory monitoring

-- Performance data
local hotspots = {}
local memory_writes = 0
local memory_reads = 0
local cycle_count = 0

-- Called every 5000 cycles to track hotspots
function on_hotspot_check()
    cycle_count = cycle_count + 5000
    local pc = mmemu.get_pc()

    -- Track this PC
    if hotspots[pc] == nil then
        hotspots[pc] = 1
    else
        hotspots[pc] = hotspots[pc] + 1
    end

    -- Every 50k cycles, dump top hotspots
    if cycle_count % 50000 == 0 then
        mmemu.log("")
        mmemu.log("=== Top Hotspots @ " .. cycle_count .. " cycles ===")

        -- Sort hotspots by count
        local sorted = {}
        for pc, count in pairs(hotspots) do
            table.insert(sorted, { pc = pc, count = count })
        end

        table.sort(sorted, function(a, b)
            return a.count > b.count
        end)

        -- Display top 5
        for i = 1, math.min(5, #sorted) do
            local entry = sorted[i]
            mmemu.log(string.format("  PC=$%04X: %d samples", entry.pc, entry.count))
        end
    end
end

-- Memory access monitoring (called frequently)
function on_memory_check()
    local sp = mmemu.get_register("SP")
    local zp0 = mmemu.read_byte(0x00)

    -- Simple sanity checks
    if sp < 0x10 then
        mmemu.log("Warning: Stack pointer very low: SP=$" .. string.format("%02X", sp))
    end
end

-- Register handlers
mmemu.on_cycle(5000, "on_hotspot_check")
mmemu.on_cycle(1000, "on_memory_check")

mmemu.log("=== Performance Monitor Active ===")
mmemu.log("Tracking hotspots every 5k cycles")
mmemu.log("Memory checks every 1k cycles")
mmemu.log("")
