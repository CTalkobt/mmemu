-- Emulator Backend Implementation
-- Issue #24 Phase 5: Backend Abstraction Layer
--
-- This backend delegates to mmemu's native Lua API.
-- Works with mmemu CLI and GUI.

local Backend = require("backend_interface")
local EmulatorBackend = setmetatable({}, {__index = Backend})
EmulatorBackend.__index = EmulatorBackend

--- Create new emulator backend instance
function EmulatorBackend.new()
    local self = setmetatable({}, EmulatorBackend)
    self.mmemu = mmemu  -- Global mmemu object from mmemu Lua API

    if not self.mmemu then
        error("EmulatorBackend requires mmemu Lua API (run from mmemu CLI/GUI)")
    end

    return self
end

--- Read single byte from memory
function EmulatorBackend:read_byte(addr)
    return self.mmemu.read_byte(addr)
end

--- Write single byte to memory
function EmulatorBackend:write_byte(addr, value)
    self.mmemu.write_byte(addr, value & 0xFF)
end

--- Read CPU register by name
function EmulatorBackend:get_register(name)
    return self.mmemu.get_register(name)
end

--- Write CPU register by name
function EmulatorBackend:set_register(name, value)
    self.mmemu.set_register(name, value)
end

--- Get program counter
function EmulatorBackend:get_pc()
    return self.mmemu.get_pc()
end

--- Set program counter
function EmulatorBackend:set_pc(addr)
    self.mmemu.set_pc(addr)
end

--- Log message
function EmulatorBackend:log(message)
    self.mmemu.log(message)
end

--- Execute single CPU instruction
function EmulatorBackend:step()
    -- Note: Not directly available in Lua API
    -- Would require CLI "step" command or MCP tool
    self:log("(step() not available in Lua - use CLI 'step' command)")
end

--- Get backend name
function EmulatorBackend:name()
    return "emulator"
end

--- Check if backend is available
function EmulatorBackend:is_available()
    return self.mmemu ~= nil
end

return EmulatorBackend
