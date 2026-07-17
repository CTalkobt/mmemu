-- Hardware Backend Implementation - JTAG Serial Monitor
-- Issue #24 Phase 5.2: Hardware Backend Serial Protocol
--
-- This backend communicates with real MEGA65 hardware via JTAG serial monitor.
-- It uses the Matrix Mode Monitor protocol from the MEGA65 book.
--
-- Protocol: Text-based Matrix Mode Monitor commands
-- Transport: JTAG loopback TCP socket
-- Default: localhost:6502 (mmemu serial monitor server)
-- Real hardware: TE0790-03 JTAG adapter via USB

local Backend = require("backend_interface")
local JTAGLoopback = require("jtag_loopback")

local HardwareBackend = setmetatable({}, {__index = Backend})
HardwareBackend.__index = HardwareBackend

--- Create new hardware backend instance
-- @param host: JTAG loopback host (default "localhost" for mmemu, or FTDI device)
-- @param port: JTAG loopback port (default 6502 for mmemu serial monitor)
function HardwareBackend.new(host, port)
    local self = setmetatable({}, HardwareBackend)

    self.host = host or "localhost"
    self.port = port or 6502
    self.jtag = nil
    self.connected = false
    self.debug = false

    return self
end

--- Connect to hardware via JTAG loopback
-- @return success: true if connected, false otherwise
function HardwareBackend:connect()
    if self.connected then
        return true
    end

    self.jtag = JTAGLoopback.new(self.host, self.port)
    self.jtag.debug = self.debug

    if not self.jtag:connect() then
        self:log("ERROR: Failed to connect to JTAG loopback at " .. self.host .. ":" .. self.port)
        return false
    end

    self.connected = true
    self:log("Connected to MEGA65 hardware via JTAG loopback")
    return true
end

--- Disconnect from hardware
function HardwareBackend:disconnect()
    if self.jtag then
        self.jtag:disconnect()
        self.jtag = nil
    end
    self.connected = false
    self:log("Disconnected from hardware")
end

--- Read single byte from memory
-- Matrix Mode: M <addr> shows 256 bytes at address
-- We parse the output to extract the specific byte
function HardwareBackend:read_byte(addr)
    if not self.connected then
        error("Hardware backend not connected")
    end

    -- Use Matrix Mode "M" command to get memory
    -- M<addr16> displays 16 rows of 16 bytes each
    local cmd = string.format("M%04X", addr & 0xFFFF)
    local response = self.jtag:send_command(cmd)

    if not response then
        error("No response from hardware")
    end

    -- Parse response: first line typically contains address and bytes
    -- Format: "addr: XX XX XX XX ..." or similar
    -- We want the byte at the target address
    local hex_values = response:gmatch("%x%x")
    for val_str in hex_values do
        local val = tonumber(val_str, 16)
        if val then
            return val & 0xFF
        end
    end

    error("Failed to parse memory response: " .. response)
end

--- Write single byte to memory
-- Matrix Mode: S <addr16> <byte> sets memory
function HardwareBackend:write_byte(addr, value)
    if not self.connected then
        error("Hardware backend not connected")
    end

    -- Use Matrix Mode "S" command to set memory
    local cmd = string.format("S%04X%02X", addr & 0xFFFF, value & 0xFF)
    local response = self.jtag:send_command(cmd)

    if response and response ~= "" and not response:match("ERROR") then
        return
    end

    error("Write failed: " .. tostring(response))
end

--- Get CPU register by name
-- Matrix Mode: R shows all registers in format "A=XX X=XX Y=XX SP=XX P=XX PC=XXXX"
function HardwareBackend:get_register(name)
    if not self.connected then
        error("Hardware backend not connected")
    end

    local cmd = "R"
    local response = self.jtag:send_command(cmd)

    if not response then
        error("No response from hardware")
    end

    -- Parse register response
    -- Format: "A=XX X=XX Y=XX SP=XX P=XX PC=XXXX" or similar
    local reg_upper = name:upper()

    -- Special handling for different register name formats
    if reg_upper == "PC" or reg_upper == "6" then
        -- PC is often shown as separate or at end
        local pc_match = response:match("PC=(%x%x%x%x)")
        if pc_match then
            return tonumber(pc_match, 16) & 0xFFFF
        end
    else
        -- Other registers (A, X, Y, SP, P)
        local pattern = reg_upper .. "=(%x%x)"
        local hex_match = response:match(pattern)
        if hex_match then
            return tonumber(hex_match, 16) & 0xFF
        end
    end

    error("Register not found in response: " .. response)
end

--- Set CPU register by name
-- Matrix Mode: Direct register modification via specialized commands
function HardwareBackend:set_register(name, value)
    if not self.connected then
        error("Hardware backend not connected")
    end

    local reg_upper = name:upper()

    if reg_upper == "PC" or reg_upper == "6" then
        -- Use G command to set PC
        local cmd = string.format("G%04X", value & 0xFFFF)
        local response = self.jtag:send_command(cmd)
        if response and response ~= "" and not response:match("ERROR") then
            return
        end
        error("PC set failed: " .. tostring(response))
    else
        -- For other registers, use direct assignment if supported
        -- Format may be like "A=XX" to set accumulator
        local cmd = string.format("%s=%02X", reg_upper, value & 0xFF)
        local response = self.jtag:send_command(cmd)
        if response and response ~= "" and not response:match("ERROR") then
            return
        end
        error("Register set failed: " .. tostring(response))
    end
end

--- Get program counter (16-bit)
function HardwareBackend:get_pc()
    return self:get_register("PC")
end

--- Set program counter (16-bit)
function HardwareBackend:set_pc(addr)
    self:set_register("PC", addr)
end

--- Log message to console
function HardwareBackend:log(message)
    print(message)
end

--- Get backend name
function HardwareBackend:name()
    return "hardware-jtag"
end

--- Check if backend is available/connected
function HardwareBackend:is_available()
    return self.connected
end

return HardwareBackend
