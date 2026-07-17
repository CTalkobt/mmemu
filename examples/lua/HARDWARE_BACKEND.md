# Phase 5.2: Hardware Backend Serial Protocol

## Overview

The Hardware Backend enables running the same backend-agnostic test suite on real MEGA65 hardware. Communication occurs over serial port (USB-to-serial) at 115200 baud.

## Protocol Design

### Serial Configuration
- **Baud Rate**: 115200
- **Data Bits**: 8
- **Parity**: None
- **Stop Bits**: 1
- **Flow Control**: None
- **Line Endings**: LF (`\n`)

### Command Format
All commands are text-based, one per line:
```
<COMMAND> <ARG1> <ARG2> ... <ARGn>\n
```

Responses:
- **Success**: `OK\n` or value in hex
- **Error**: `ERROR: <reason>\n`

## Supported Commands

### Memory Operations

#### READ - Read single byte from memory
```
READ <addr16>
Response: <byte16>\n

Example:
  -> READ 0800
  <- 42
```

#### WRITE - Write single byte to memory
```
WRITE <addr16> <byte16>
Response: OK\n

Example:
  -> WRITE 0800 42
  <- OK
```

### Register Operations

#### GETREG - Get CPU register by name
```
GETREG <name>
Response: <byte16>\n

Valid names: A, X, Y, SP, P, PC (as 16-bit), etc.

Example:
  -> GETREG A
  <- FF
```

#### SETREG - Set CPU register by name
```
SETREG <name> <byte16>
Response: OK\n

Example:
  -> SETREG A 42
  <- OK
```

### Execution Control

#### GETPC - Get program counter (16-bit)
```
GETPC
Response: <addr16>\n

Example:
  -> GETPC
  <- FCE2
```

#### SETPC - Set program counter
```
SETPC <addr16>
Response: OK\n

Example:
  -> SETPC 0800
  <- OK
```

#### STEP - Execute single CPU instruction
```
STEP
Response: OK\n

Note: Requires breakpoint/pause support on hardware
```

### Device Information

#### INFO - Get device identification
```
INFO
Response: <device_string>\n

Example:
  -> INFO
  <- MEGA65 Hypervisor v1.0
```

## Implementation Status

### Lua Backend (✅ Complete)
- `backend_hardware.lua` — Full Lua implementation
- SerialPort wrapper for cross-platform support
- Mock mode for testing without hardware
- Error handling and timeouts

### Hardware Side (🔄 TODO)
The following must be implemented on the MEGA65 side:

1. **Serial Monitor Program**
   - Listen on USB serial port at 115200 baud
   - Parse text commands
   - Execute memory/register operations
   - Send responses

2. **Integration Points**
   - Access to CPU state (A, X, Y, SP, P, PC)
   - Access to system RAM ($0000-$FFFF in non-banked mode)
   - Single-step capability (requires hypervisor debugger support)
   - Device identification string

3. **Example Implementation (Hypervisor)**
   ```assembly
   ; MEGA65 Hypervisor Serial Monitor
   ; Location: $8000+ in hypervisor bank
   
   serial_monitor:
       jsr serial_init      ; Initialize serial port
   .loop:
       jsr serial_read_line ; Read command
       jsr parse_command    ; Parse command
       jsr execute_command  ; Execute command
       jsr serial_send_response
       jmp .loop
   ```

## Testing Hardware Backend

### Prerequisites
1. Lua 5.4 with luaserialport:
   ```bash
   luarocks install luaserialport
   ```

2. Real MEGA65 with serial monitor program, OR
3. Mock mode for testing without hardware

### Running Tests

#### With Mock Hardware (No Real Hardware Needed)
```bash
cd examples/lua
lua -e "
    local HW = require('backend_hardware')
    local hw = HW.new('mock', 115200)
    hw:connect()
    print('Mock connected:', hw:is_available())
    hw:disconnect()
"
```

#### With Real MEGA65
```bash
cd examples/lua
lua -e "
    local TestFramework = require('test_framework')
    local HW = require('backend_hardware')
    
    local hw = HW.new('/dev/ttyUSB0', 115200)
    if hw:connect() then
        local tests = TestFramework.create('hardware')
        tests:add_test('test_zero_page_pattern', function(b)
            for addr = 2, 11 do
                b:write_byte(addr, (addr * 2) & 0xFF)
            end
            for addr = 2, 11 do
                assert(b:read_byte(addr) == ((addr * 2) & 0xFF))
            end
            return true
        end)
        tests:run_all()
        hw:disconnect()
    end
"
```

#### Via mmemu CLI
```bash
cd examples/lua
echo -e "script run test_suite_backend_hardware.lua\\nquit" | ../../bin/mmemu-cli -m c64
```

## Lua API Usage

### Creating Backend Instance
```lua
local HardwareBackend = require("backend_hardware")

-- Real hardware
local hw = HardwareBackend.new("/dev/ttyUSB0", 115200)

-- Mock mode (for testing)
local hw = HardwareBackend.new("mock", 115200)
```

### Connecting
```lua
if hw:connect() then
    print("Connected!")
else
    print("Connection failed")
    return
end
```

### Memory Operations
```lua
-- Read byte
local value = hw:read_byte(0x0800)

-- Write byte
hw:write_byte(0x0800, 0x42)

-- Verify pattern
hw:write_byte(0x0801, 0xAA)
assert(hw:read_byte(0x0801) == 0xAA)
```

### Register Operations
```lua
-- Read register
local a_reg = hw:get_register("A")

-- Write register
hw:set_register("A", 0xFF)

-- PC manipulation
local pc = hw:get_pc()
hw:set_pc(0xFCE2)
```

### Logging
```lua
hw:log("This is a message")
```

### Cleanup
```lua
hw:disconnect()
```

## Backend-Agnostic Tests

The same test code runs on both backends:

```lua
local Backend = require("backend_interface")

function test_memory_pattern(backend)
    -- Works with EmulatorBackend or HardwareBackend
    for i = 2, 15 do
        backend:write_byte(i, (i * 2) & 0xFF)
    end
    
    for i = 2, 15 do
        if backend:read_byte(i) ~= ((i * 2) & 0xFF) then
            error("Mismatch at $" .. string.format("%02X", i))
        end
    end
    
    return true
end
```

## Error Handling

Errors are thrown as Lua exceptions and can be caught:

```lua
local success, err = pcall(function()
    hw:read_byte(0x0800)
end)

if not success then
    print("Error: " .. err)
end
```

## Debug Mode

Enable debug output:
```lua
hw.debug = true
hw:connect()
-- Now all commands/responses are printed
```

## Future Enhancements

1. **Network Backend** — TCP/IP instead of serial
2. **Breakpoint Support** — Pause/resume execution
3. **Real-time Trace** — Stream execution trace
4. **DMA Control** — Trigger DMA operations
5. **Device I/O** — Direct access to SID, VIC-II, etc.

## References

- Backend Interface: `backend_interface.lua`
- Test Framework: `test_framework.lua`
- Emulator Backend: `backend_emulator.lua`
- MEGA65 Serial Protocol: Hypervisor documentation
