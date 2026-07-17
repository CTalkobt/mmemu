-- Hardware Backend Implementation
-- Issue #24 Phase 5.2: Hardware Backend Serial Protocol
--
-- This backend communicates with real MEGA65 hardware via serial port.
-- Protocol: Text-based commands over serial (115200 baud, 8N1)
--
-- Supported commands:
--   READ <addr16> -> <byte16>\n (e.g., "READ 0800" -> "42\n")
--   WRITE <addr16> <byte16> -> OK\n
--   GETREG <name> -> <byte16>\n (e.g., "GETREG A" -> "FF\n")
--   SETREG <name> <byte16> -> OK\n
--   GETPC -> <addr16>\n
--   SETPC <addr16> -> OK\n
--   STEP -> OK\n
--   INFO -> version string

-- Serial port wrapper for unified interface
local SerialPort = {}
SerialPort.__index = SerialPort

function SerialPort.new(port, baudrate)
    local self = setmetatable({}, SerialPort)
    self.port = port
    self.baudrate = baudrate
    self.handle = nil
    self.buffer = ""
    return self
end

function SerialPort:write(data)
    if not self.handle then return false, "Port not open" end
    return self.handle:write(data)
end

function SerialPort:read_line(timeout)
    if not self.handle then return nil, "Port not open" end

    timeout = timeout or 2.0
    local deadline = os.time() + timeout

    while true do
        -- Check for newline in buffer
        local idx = self.buffer:find("\n")
        if idx then
            local line = self.buffer:sub(1, idx - 1)
            self.buffer = self.buffer:sub(idx + 1)
            return line
        end

        -- Read more data
        local chunk = self.handle:read(1024)
        if chunk and #chunk > 0 then
            self.buffer = self.buffer .. chunk
        else
            -- Check timeout
            if os.time() >= deadline then
                return nil, "Read timeout"
            end
            os.execute("sleep 0.01")  -- Small delay to avoid busy-waiting
        end
    end
end

function SerialPort:close()
    if self.handle then
        pcall(function() self.handle:close() end)
        self.handle = nil
    end
end

local Backend = require("backend_interface")
local HardwareBackend = setmetatable({}, {__index = Backend})
HardwareBackend.__index = HardwareBackend

--- Create new hardware backend instance
-- @param port: Serial port (e.g., "/dev/ttyUSB0" or "COM3") or nil for mock
-- @param baudrate: Serial baud rate (default 115200)
-- @param timeout: Read timeout in seconds (default 2.0)
function HardwareBackend.new(port, baudrate, timeout)
    local self = setmetatable({}, HardwareBackend)

    self.port = port or "/dev/ttyUSB0"
    self.baudrate = baudrate or 115200
    self.timeout = timeout or 2.0
    self.connected = false
    self.serial = nil
    self.debug = false  -- Enable debug logging
    self.mock_mode = (port == "mock")  -- Enable mock for testing

    return self
end

--- Send command and receive response
-- @private
-- @param cmd: Command string (without newline)
-- @return response: Response string (with newline stripped)
function HardwareBackend:send_command(cmd)
    if not self.connected then
        error("Hardware not connected")
    end

    if self.debug then
        print("[HW] -> " .. cmd)
    end

    -- Send command with newline
    local ok, err = self.serial:write(cmd .. "\n")
    if not ok then
        error("Failed to send command: " .. tostring(err))
    end

    -- Read response line (up to newline)
    local response, err = self.serial:read_line(self.timeout)
    if not response then
        error("Failed to read response: " .. tostring(err))
    end

    if self.debug then
        print("[HW] <- " .. response)
    end

    return response
end

--- Read single byte from memory
-- Protocol: "READ <addr16>" -> "<byte16>"
function HardwareBackend:read_byte(addr)
    if not self.connected then
        error("Hardware backend not connected")
    end

    local cmd = string.format("READ %04X", addr & 0xFFFF)
    local response = self:send_command(cmd)
    local value = tonumber(response, 16)

    if not value then
        error("Invalid response from hardware: " .. response)
    end

    return value & 0xFF
end

--- Write single byte to memory
-- Protocol: "WRITE <addr16> <byte16>" -> "OK"
function HardwareBackend:write_byte(addr, value)
    if not self.connected then
        error("Hardware backend not connected")
    end

    local cmd = string.format("WRITE %04X %02X", addr & 0xFFFF, value & 0xFF)
    local response = self:send_command(cmd)

    if response ~= "OK" then
        error("Write failed: " .. response)
    end
end

--- Read CPU register by name
-- Protocol: "GETREG <name>" -> "<byte16>"
function HardwareBackend:get_register(name)
    if not self.connected then
        error("Hardware backend not connected")
    end

    local cmd = string.format("GETREG %s", name:upper())
    local response = self:send_command(cmd)
    local value = tonumber(response, 16)

    if not value then
        error("Invalid register value: " .. response)
    end

    return value & 0xFF
end

--- Write CPU register by name
-- Protocol: "SETREG <name> <byte16>" -> "OK"
function HardwareBackend:set_register(name, value)
    if not self.connected then
        error("Hardware backend not connected")
    end

    local cmd = string.format("SETREG %s %02X", name:upper(), value & 0xFF)
    local response = self:send_command(cmd)

    if response ~= "OK" then
        error("Register write failed: " .. response)
    end
end

--- Get program counter (16-bit)
function HardwareBackend:get_pc()
    if not self.connected then
        error("Hardware backend not connected")
    end

    local cmd = "GETPC"
    local response = self:send_command(cmd)
    local value = tonumber(response, 16)

    if not value then
        error("Invalid PC value: " .. response)
    end

    return value & 0xFFFF
end

--- Set program counter (16-bit)
function HardwareBackend:set_pc(addr)
    if not self.connected then
        error("Hardware backend not connected")
    end

    local cmd = string.format("SETPC %04X", addr & 0xFFFF)
    local response = self:send_command(cmd)

    if response ~= "OK" then
        error("PC write failed: " .. response)
    end
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
-- @return success: true if connected, false otherwise
function HardwareBackend:connect()
    if self.connected then
        return true
    end

    if self.mock_mode then
        -- Mock mode for testing without hardware
        self:log("Connecting in MOCK mode (no real hardware)")
        local mock_serial = SerialPort.new("mock", self.baudrate)
        local debug_flag = self.debug
        local last_command = ""
        local mock_memory = {}  -- Simulated memory
        local mock_registers = {A = 0, X = 0, Y = 0, SP = 0xFD, P = 0x24, PC = 0xFCE2}

        -- Override write for mock
        function mock_serial:write(data)
            last_command = data:gsub("\n", "")
            if debug_flag then print("[MOCK] -> " .. last_command) end
            return true
        end

        -- Override read_line for mock responses
        function mock_serial:read_line(timeout)
            local response = "OK"

            -- Parse command and generate appropriate response
            local parts = {}
            for part in last_command:gmatch("%S+") do
                table.insert(parts, part)
            end

            if parts[1] == "READ" then
                local addr = tonumber(parts[2], 16) or 0
                response = string.format("%02X", mock_memory[addr] or 0)
            elseif parts[1] == "WRITE" then
                local addr = tonumber(parts[2], 16) or 0
                local val = tonumber(parts[3], 16) or 0
                mock_memory[addr] = val & 0xFF
                response = "OK"
            elseif parts[1] == "GETREG" then
                local reg = parts[2]:upper()
                response = string.format("%02X", mock_registers[reg] or 0)
            elseif parts[1] == "SETREG" then
                local reg = parts[2]:upper()
                local val = tonumber(parts[3], 16) or 0
                mock_registers[reg] = val & 0xFF
                response = "OK"
            elseif parts[1] == "GETPC" then
                response = string.format("%04X", mock_registers.PC or 0xFCE2)
            elseif parts[1] == "SETPC" then
                local pc_val = tonumber(parts[2], 16) or 0
                mock_registers.PC = pc_val & 0xFFFF
                response = "OK"
            elseif parts[1] == "INFO" then
                response = "MEGA65 Mock Hardware"
            end

            if debug_flag then print("[MOCK] <- " .. response) end
            return response
        end

        self.serial = mock_serial
        self.connected = true
        self:log("Mock hardware connected")
        return true
    end

    -- Try to open real serial port
    -- Note: Requires luarocks package "luaserialport"
    -- Install with: luarocks install luaserialport
    local ok, serialport = pcall(require, "serialport")
    if not ok then
        self:log("ERROR: luaserialport not found")
        self:log("Install with: luarocks install luaserialport")
        self:log("Or use mock mode: backend:new('mock', 115200)")
        return false
    end

    local port, err = serialport.open(self.port)
    if not port then
        self:log("ERROR: Cannot open port " .. self.port .. ": " .. tostring(err))
        return false
    end

    -- Configure serial port
    port:set_baud_rate(self.baudrate)
    port:set_data_bits(8)
    port:set_parity("N")
    port:set_stop_bits(1)
    port:set_flow_control("N", "N")

    -- Wrap in our serial wrapper
    local serial_wrapper = SerialPort.new(self.port, self.baudrate)
    serial_wrapper.handle = port

    self.serial = serial_wrapper

    -- Test connection with handshake
    local ok, err = pcall(function()
        local response = self:send_command("INFO")
        if not response:match("MEGA65") then
            error("Device identification failed: " .. response)
        end
    end)

    if not ok then
        port:close()
        self:log("ERROR: Handshake failed: " .. tostring(err))
        return false
    end

    self.connected = true
    self:log("Connected to MEGA65 hardware on " .. self.port)
    return true
end

--- Disconnect from hardware
function HardwareBackend:disconnect()
    if self.serial then
        self.serial:close()
        self.serial = nil
    end
    self.connected = false
    self:log("Disconnected from hardware")
end

return HardwareBackend
