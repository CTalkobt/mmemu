-- Backend Interface for Hardware/Emulator Abstraction
-- Issue #24 Phase 5: Backend Abstraction Layer
--
-- This module defines the unified interface for testing against both
-- mmemu emulator and real MEGA65 hardware.
--
-- Usage:
--   local Backend = require("backend_interface")
--   local backend = Backend.create("emulator")  -- or "hardware"
--   backend:read_byte(0x2000)

--- Backend interface (abstract contract)
local Backend = {}
Backend.__index = Backend

--- Create backend instance
-- @param backend_type: "emulator" or "hardware"
-- @return Backend instance
function Backend.create(backend_type)
    if backend_type == "emulator" then
        return require("backend_emulator").new()
    elseif backend_type == "hardware" then
        return require("backend_hardware").new()
    else
        error("Unknown backend type: " .. backend_type)
    end
end

--- Read single byte from memory
-- @param addr: 16-bit address (0x0000-0xFFFF for 6502; up to 0x28-bit for MEGA65)
-- @return uint8_t value
function Backend:read_byte(addr)
    error("Backend:read_byte() not implemented")
end

--- Write single byte to memory
-- @param addr: memory address
-- @param value: byte value to write (0x00-0xFF)
function Backend:write_byte(addr, value)
    error("Backend:write_byte() not implemented")
end

--- Read 16-bit word (little-endian)
-- @param addr: memory address
-- @return uint16_t value
function Backend:read_word(addr)
    local lo = self:read_byte(addr)
    local hi = self:read_byte(addr + 1)
    return lo | (hi << 8)
end

--- Write 16-bit word (little-endian)
-- @param addr: memory address
-- @param value: word value
function Backend:write_word(addr, value)
    self:write_byte(addr, value & 0xFF)
    self:write_byte(addr + 1, (value >> 8) & 0xFF)
end

--- Read CPU register by name
-- @param name: Register name ("A", "X", "Y", "SP", "P", "PC")
-- @return register value
function Backend:get_register(name)
    error("Backend:get_register() not implemented")
end

--- Write CPU register by name
-- @param name: Register name
-- @param value: value to write
function Backend:set_register(name, value)
    error("Backend:set_register() not implemented")
end

--- Get program counter
-- @return PC value
function Backend:get_pc()
    return self:get_register("PC")
end

--- Set program counter
-- @param addr: new PC value
function Backend:set_pc(addr)
    self:set_register("PC", addr)
end

--- Log message (may route to serial, console, or file)
-- @param message: message string
function Backend:log(message)
    error("Backend:log() not implemented")
end

--- Execute single CPU instruction
-- For emulator: step CPU
-- For hardware: return immediately (hardware runs continuously)
function Backend:step()
    error("Backend:step() not implemented")
end

--- Get backend name for diagnostics
-- @return string: "emulator" or "hardware"
function Backend:name()
    error("Backend:name() not implemented")
end

--- Check if backend is available/connected
-- For emulator: always true
-- For hardware: true if serial/network connection active
-- @return boolean
function Backend:is_available()
    error("Backend:is_available() not implemented")
end

--- Fill memory region with pattern
-- @param addr: start address
-- @param size: number of bytes
-- @param pattern: byte value or function(offset) -> byte
function Backend:fill(addr, size, pattern)
    for i = 0, size - 1 do
        local val
        if type(pattern) == "function" then
            val = pattern(i)
        else
            val = pattern
        end
        self:write_byte(addr + i, val)
    end
end

--- Verify memory region matches pattern
-- @param addr: start address
-- @param size: number of bytes
-- @param expected: expected pattern (value or function)
-- @return boolean: true if all match
function Backend:verify(addr, size, expected)
    for i = 0, size - 1 do
        local expected_val
        if type(expected) == "function" then
            expected_val = expected(i)
        else
            expected_val = expected
        end

        local actual_val = self:read_byte(addr + i)
        if actual_val ~= expected_val then
            return false
        end
    end
    return true
end

--- Dump memory region as hex
-- @param addr: start address
-- @param size: number of bytes
-- @param label: optional label
function Backend:dump(addr, size, label)
    if label then
        self:log(label)
    end

    for offset = 0, size - 1, 16 do
        local line = "  $" .. string.format("%04X", addr + offset) .. ":"
        for i = 0, 15 do
            if offset + i < size then
                local byte_val = self:read_byte(addr + offset + i)
                line = line .. " " .. string.format("%02X", byte_val)
            end
        end
        self:log(line)
    end
end

--- Get CPU state snapshot
-- @return table with {pc, a, x, y, sp, p}
function Backend:get_state()
    return {
        pc = self:get_pc(),
        a = self:get_register("A"),
        x = self:get_register("X"),
        y = self:get_register("Y"),
        sp = self:get_register("SP"),
        p = self:get_register("P"),
    }
end

--- Compare two states
-- @param state1, state2: state tables
-- @return string describing differences, or nil if identical
function Backend:diff_state(state1, state2)
    local diffs = {}

    if state1.pc ~= state2.pc then
        table.insert(diffs, "PC: $" .. string.format("%04X", state1.pc) ..
                            " → $" .. string.format("%04X", state2.pc))
    end
    if state1.a ~= state2.a then
        table.insert(diffs, "A: $" .. string.format("%02X", state1.a) ..
                            " → $" .. string.format("%02X", state2.a))
    end
    if state1.x ~= state2.x then
        table.insert(diffs, "X: $" .. string.format("%02X", state1.x) ..
                            " → $" .. string.format("%02X", state2.x))
    end
    if state1.y ~= state2.y then
        table.insert(diffs, "Y: $" .. string.format("%02X", state1.y) ..
                            " → $" .. string.format("%02X", state2.y))
    end
    if state1.sp ~= state2.sp then
        table.insert(diffs, "SP: $" .. string.format("%02X", state1.sp) ..
                            " → $" .. string.format("%02X", state2.sp))
    end
    if state1.p ~= state2.p then
        table.insert(diffs, "P: $" .. string.format("%02X", state1.p) ..
                            " → $" .. string.format("%02X", state2.p))
    end

    if #diffs > 0 then
        return table.concat(diffs, ", ")
    end
    return nil
end

return Backend
