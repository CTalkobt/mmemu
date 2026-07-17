-- Interrupt Tracer - Monitor IRQ/NMI/BRK events
-- Demonstrates mmemu.on_interrupt() for interrupt handling

-- Event counters
local irq_count = 0
local nmi_count = 0
local brk_count = 0
local last_irq_pc = 0

-- IRQ handler
function on_irq()
    irq_count = irq_count + 1
    local pc = mmemu.get_pc()
    local a = mmemu.get_register("A")
    local p = mmemu.get_register("P")

    mmemu.log("[IRQ #" .. irq_count .. "] PC=$" .. string.format("%04X", pc) ..
              " A=$" .. string.format("%02X", a) ..
              " P=$" .. string.format("%02X", p))

    last_irq_pc = pc
    return true  -- Continue execution
end

-- NMI handler
function on_nmi()
    nmi_count = nmi_count + 1
    local pc = mmemu.get_pc()

    mmemu.log("[NMI #" .. nmi_count .. "] PC=$" .. string.format("%04X", pc))
    return true  -- Continue execution
end

-- BRK handler
function on_brk()
    brk_count = brk_count + 1
    local pc = mmemu.get_pc()

    mmemu.log("[BRK #" .. brk_count .. "] PC=$" .. string.format("%04X", pc))
    return false  -- Pause execution on BRK
end

-- Register event handlers
mmemu.on_interrupt("IRQ", "on_irq")
mmemu.on_interrupt("NMI", "on_nmi")
mmemu.on_interrupt("BRK", "on_brk")

mmemu.log("=== Interrupt Tracer Active ===")
mmemu.log("Logging IRQ, NMI, and BRK events")
mmemu.log("BRK will pause execution; IRQ/NMI will continue")
