-- Interrupt Handler Demo - Phase 4.2.2
-- Demonstrates mmemu.on_interrupt() for IRQ/NMI/BRK events

-- Event counters
local irq_count = 0
local nmi_count = 0
local brk_count = 0

-- IRQ handler - called when IRQ occurs
function on_irq()
    irq_count = irq_count + 1
    local pc = mmemu.get_pc()
    local a = mmemu.get_register("A")

    mmemu.log("[IRQ #" .. irq_count .. "] PC=$" .. string.format("%04X", pc) ..
              " A=$" .. string.format("%02X", a))

    return true  -- Continue execution
end

-- NMI handler - called when NMI occurs
function on_nmi()
    nmi_count = nmi_count + 1
    local pc = mmemu.get_pc()

    mmemu.log("[NMI #" .. nmi_count .. "] PC=$" .. string.format("%04X", pc))
    return true  -- Continue execution
end

-- BRK handler - called when BRK instruction executes
function on_brk()
    brk_count = brk_count + 1
    local pc = mmemu.get_pc()

    mmemu.log("[BRK #" .. brk_count .. "] PC=$" .. string.format("%04X", pc))

    -- Optional: print CPU state
    local sp = mmemu.get_register("SP")
    local p = mmemu.get_register("P")
    mmemu.log("  SP=$" .. string.format("%02X", sp) ..
              " P=$" .. string.format("%02X", p))

    return false  -- Pause execution on BRK
end

-- Summary function
function print_summary()
    mmemu.log("")
    mmemu.log("=== Interrupt Summary ===")
    mmemu.log("IRQ count: " .. irq_count)
    mmemu.log("NMI count: " .. nmi_count)
    mmemu.log("BRK count: " .. brk_count)
end

-- Register handlers
mmemu.on_interrupt("IRQ", "on_irq")
mmemu.on_interrupt("NMI", "on_nmi")
mmemu.on_interrupt("BRK", "on_brk")

mmemu.log("=== Interrupt Handler Active ===")
mmemu.log("IRQ: Continue execution")
mmemu.log("NMI: Continue execution")
mmemu.log("BRK: Pause execution")
mmemu.log("")
mmemu.log("Note: Handlers are currently manual.")
mmemu.log("Use cycle/breakpoint events to trigger detection.")
