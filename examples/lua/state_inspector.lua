-- State Inspector and Diagnostic Tool
-- Captures and analyzes machine state for debugging

mmemu.log("=== Machine State Inspector ===")

-- Helper function to format hex values
local function hex8(val)
    return string.format("%02X", val & 0xFF)
end

local function hex16(val)
    return string.format("%04X", val & 0xFFFF)
end

-- Function to capture CPU state snapshot
function capture_cpu_state()
    local state = {}
    state.pc = mmemu.get_pc()
    state.a = mmemu.get_register("A")
    state.x = mmemu.get_register("X")
    state.y = mmemu.get_register("Y")
    state.sp = mmemu.get_register("SP")
    state.p = mmemu.get_register("P")
    return state
end

-- Function to display CPU state
function display_state(state, label)
    if label then
        mmemu.log(label)
    end
    mmemu.log("  PC: $" .. hex16(state.pc) ..
              "  A: $" .. hex8(state.a) ..
              "  X: $" .. hex8(state.x) ..
              "  Y: $" .. hex8(state.y))
    mmemu.log("  SP: $" .. hex8(state.sp) ..
              "  P: $" .. hex8(state.p))
end

-- Function to compare two states
function compare_states(state1, state2)
    local changes = {}

    if state1.pc ~= state2.pc then
        table.insert(changes, "PC: $" .. hex16(state1.pc) .. " → $" .. hex16(state2.pc))
    end
    if state1.a ~= state2.a then
        table.insert(changes, "A: $" .. hex8(state1.a) .. " → $" .. hex8(state2.a))
    end
    if state1.x ~= state2.x then
        table.insert(changes, "X: $" .. hex8(state1.x) .. " → $" .. hex8(state2.x))
    end
    if state1.y ~= state2.y then
        table.insert(changes, "Y: $" .. hex8(state1.y) .. " → $" .. hex8(state2.y))
    end
    if state1.sp ~= state2.sp then
        table.insert(changes, "SP: $" .. hex8(state1.sp) .. " → $" .. hex8(state2.sp))
    end
    if state1.p ~= state2.p then
        table.insert(changes, "P: $" .. hex8(state1.p) .. " → $" .. hex8(state2.p))
    end

    return changes
end

-- Function to dump memory region as hex
function dump_memory(base_addr, size, label)
    if label then
        mmemu.log(label)
    end

    for offset = 0, size - 1, 16 do
        local line = "  $" .. hex16(base_addr + offset) .. ":"
        for i = 0, 15 do
            if offset + i < size then
                line = line .. " " .. hex8(mmemu.read_byte(base_addr + offset + i))
            end
        end
        mmemu.log(line)
    end
end

-- Function to detect stack corruption
function check_stack_health()
    mmemu.log("=== Stack Health Check ===")

    local sp = mmemu.get_register("SP")
    local stack_top = 0x0100 + sp

    -- Check common stack sentinel patterns
    local sentinel_good = true

    -- Typical: check for obvious corruption patterns
    local val1 = mmemu.read_byte(0x0100 + sp)
    local val2 = mmemu.read_byte(0x0100 + ((sp + 1) & 0xFF))

    mmemu.log("Current SP: $" .. hex8(sp))
    mmemu.log("Stack top values:")
    mmemu.log("  [$01" .. hex8(sp) .. "]: $" .. hex8(val1))
    mmemu.log("  [$01" .. hex8((sp + 1) & 0xFF) .. "]: $" .. hex8(val2))

    if val1 == 0x00 and val2 == 0x00 then
        mmemu.log("⚠ Stack may be uninitialized or cleared")
    else
        mmemu.log("✓ Stack appears initialized")
    end
end

-- Example usage: capture initial state
mmemu.log("")
mmemu.log("Capturing initial machine state...")
mmemu.log("")

local initial_state = capture_cpu_state()
display_state(initial_state, "Initial CPU State:")

mmemu.log("")
mmemu.log("Memory regions:")
mmemu.log("")

dump_memory(0x0000, 32, "Zero Page ($0000-$001F):")
mmemu.log("")

dump_memory(0x0100, 32, "Stack Page ($0100-$011F):")
mmemu.log("")

dump_memory(0x2000, 32, "User Memory ($2000-$201F):")
mmemu.log("")

-- Check stack health
check_stack_health()

mmemu.log("")
mmemu.log("=== State Capture Complete ===")
mmemu.log("Use 'break <addr> action' to capture state at specific points")
