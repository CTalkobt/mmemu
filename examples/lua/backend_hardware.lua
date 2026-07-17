-- Hardware Backend Implementation (Stub)
-- Issue #24 Phase 5: Backend Abstraction Layer
--
-- This backend communicates with real MEGA65 hardware via serial/network.
-- Currently a stub; full implementation depends on hardware protocol.
--
-- TODO: Implement actual hardware communication
--   - Serial port (luarocks: luaserialport)
--   - Network socket (Lua sockets)
--   - Protocol: Simple text-based commands or binary protocol

local Backend = require("backend_interface")
local HardwareBackend = setmetatable({}, {__index = Backend})
HardwareBackend.__index = HardwareBackend

--- Create new hardware backend instance
-- @param port: Serial port (e.g., "/dev/ttyUSB0" or "COM3")
-- @param baudrate: Serial baud rate (default 115200)
function HardwareBackend.new(port, baudrate)
    local self = setmetatable({}, HardwareBackend)

    self.port = port or "/dev/ttyUSB0"
    self.baudrate = baudrate or 115200
    self.connected = false
    self.serial = nil

    -- TODO: Initialize serial connection
    -- local serial = require("serialport")
    -- self.serial = serial.open(self.port)
    -- self.serial:set_baud_rate(self.baudrate)
    -- self.connected = true

    return self
end

--- Read single byte from memory
-- Protocol: "READ <addr16>\n" -> "<byte>\n"
function HardwareBackend:read_byte(addr)
    if not self.connected then
        error("Hardware backend not connected")
    end

    -- TODO: Implement serial communication
    -- local cmd = string.format("READ %04X\n", addr)
    -- self.serial:write(cmd)
    -- local response = self.serial:read(2)  -- "XX\n"
    -- return tonumber(response, 16)

    error("HardwareBackend:read_byte() not yet implemented")
end

--- Write single byte to memory
-- Protocol: "WRITE <addr16> <byte>\n" -> "OK\n"
function HardwareBackend:write_byte(addr, value)
    if not self.connected then
        error("Hardware backend not connected")
    end

    -- TODO: Implement serial communication
    -- local cmd = string.format("WRITE %04X %02X\n", addr, value & 0xFF)
    -- self.serial:write(cmd)
    -- local response = self.serial:read(3)  -- "OK\n"

    error("HardwareBackend:write_byte() not yet implemented")
end

--- Read CPU register by name
-- Protocol: "GETREG <name>\n" -> "<byte>\n"
function HardwareBackend:get_register(name)
    if not self.connected then
        error("Hardware backend not connected")
    end

    -- TODO: Implement serial communication
    -- local cmd = string.format("GETREG %s\n", name)
    -- self.serial:write(cmd)
    -- local response = self.serial:read(2)  -- "XX\n"
    -- return tonumber(response, 16)

    error("HardwareBackend:get_register() not yet implemented")
end

--- Write CPU register by name
-- Protocol: "SETREG <name> <byte>\n" -> "OK\n"
function HardwareBackend:set_register(name, value)
    if not self.connected then
        error("Hardware backend not connected")
    end

    -- TODO: Implement serial communication
    -- local cmd = string.format("SETREG %s %02X\n", name, value & 0xFF)
    -- self.serial:write(cmd)
    -- local response = self.serial:read(3)  -- "OK\n"

    error("HardwareBackend:set_register() not yet implemented")
end

--- Get program counter
function HardwareBackend:get_pc()
    return self:get_register("PC")
end

--- Set program counter
function HardwareBackend:set_pc(addr)
    self:set_register("PC", addr)
end

--- Log message
function HardwareBackend:log(message)
    if self.connected then
        -- Route to hardware serial monitor or file
        print(message)
    end
end

--- Execute single CPU instruction
-- Note: Hardware runs continuously, this is a no-op
function HardwareBackend:step()
    -- For hardware, we can't stop and single-step via serial
    -- Would need breakpoint/pause functionality on device
    self:log("(Hardware: Execution continues automatically)")
end

--- Get backend name
function HardwareBackend:name()
    return "hardware"
end

--- Check if backend is available/connected
function HardwareBackend:is_available()
    return self.connected
end

--- Connect to hardware
function HardwareBackend:connect()
    if self.connected then
        return true
    end

    -- TODO: Establish serial/network connection
    -- try:
    --   self.serial = open_serial_port(self.port, self.baudrate)
    --   send_handshake()
    --   check_response()
    --   self.connected = true
    -- catch:
    --   self:log("Failed to connect to " .. self.port)
    --   return false

    return false
end

--- Disconnect from hardware
function HardwareBackend:disconnect()
    if self.serial then
        -- self.serial:close()
        self.connected = false
    end
end

return HardwareBackend
