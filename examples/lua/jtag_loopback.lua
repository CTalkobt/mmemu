-- JTAG Loopback Device
-- Issue #24 Phase 5.1: JTAG Loopback
--
-- This module provides a JTAG loopback interface to the mmemu Serial Monitor Server.
-- It abstracts the underlying TCP connection to the serial monitor.
--
-- Usage:
--   local jtag = require("jtag_loopback")
--   local device = jtag.new("localhost", 6502)
--   device:connect()
--   device:send_command("r")        -- Get registers
--   device:disconnect()

local JTAGLoopback = {}
JTAGLoopback.__index = JTAGLoopback

--- Create a new JTAG loopback connection
-- @param host: Host to connect to (default "localhost")
-- @param port: Port to connect to (default 6502)
function JTAGLoopback.new(host, port)
    local self = setmetatable({}, JTAGLoopback)

    self.host = host or "localhost"
    self.port = port or 6502
    self.socket = nil
    self.connected = false
    self.debug = false

    return self
end

--- Connect to the JTAG loopback device
-- @return success: true if connected, false otherwise
function JTAGLoopback:connect()
    if self.connected then
        return true
    end

    -- Try to require socket library (from luarocks: luasocket)
    local ok, socket = pcall(require, "socket")
    if not ok then
        print("ERROR: luasocket not found. Install with: luarocks install luasocket")
        return false
    end

    local sock, err = socket.connect(self.host, self.port)
    if not sock then
        print("ERROR: Cannot connect to " .. self.host .. ":" .. self.port .. " - " .. tostring(err))
        return false
    end

    -- Set socket timeout
    sock:settimeout(2.0)

    self.socket = sock
    self.connected = true

    if self.debug then
        print("[JTAG] Connected to " .. self.host .. ":" .. self.port)
    end

    return true
end

--- Send a command to the JTAG loopback and get the response
-- @param cmd: Command string (will be newline-terminated)
-- @return response: Response string (without trailing newline) or nil on error
function JTAGLoopback:send_command(cmd)
    if not self.connected or not self.socket then
        error("JTAG loopback not connected")
    end

    if self.debug then
        print("[JTAG] -> " .. cmd)
    end

    -- Send command with newline
    local ok, err = self.socket:send(cmd .. "\n")
    if not ok then
        error("Failed to send command: " .. tostring(err))
    end

    -- Read response (up to first newline)
    local response, err = self.socket:receive("*l")
    if not response then
        error("Failed to receive response: " .. tostring(err))
    end

    if self.debug then
        print("[JTAG] <- " .. response)
    end

    return response
end

--- Disconnect from the JTAG loopback device
function JTAGLoopback:disconnect()
    if self.socket then
        pcall(function() self.socket:close() end)
        self.socket = nil
    end
    self.connected = false

    if self.debug then
        print("[JTAG] Disconnected")
    end
end

--- Get device name
function JTAGLoopback:name()
    return "JTAG Loopback"
end

--- Check if connected
function JTAGLoopback:is_connected()
    return self.connected
end

return JTAGLoopback
