# Phase 5.1 & 5.2: JTAG Serial Monitor Integration

## Overview

Phase 5.1 and 5.2 implement hardware-validated testing through the MEGA65's standard JTAG serial monitor protocol. The architecture uses a JTAG loopback device to communicate with the Matrix Mode Monitor.

## Architecture

```
┌──────────────────────────────────────────────────────────────┐
│ Lua Test Suite (backend-agnostic)                            │
└──────────────────┬───────────────────────────────────────────┘
                   │
┌──────────────────▼───────────────────────────────────────────┐
│ Backend Abstraction Interface                                │
└──────────────────┬─────────────────────┬────────────────────┘
                   │                     │
    ┌──────────────▼──────┐   ┌──────────▼──────────────┐
    │ EmulatorBackend     │   │ HardwareBackendJTAG     │
    │ (Phase 5)           │   │ (Phase 5.2)            │
    └──────────────┬──────┘   └──────────┬──────────────┘
                   │                     │
                   │          ┌──────────▼──────────────┐
                   │          │ JTAG Loopback Device   │
                   │          │ (Phase 5.1)            │
                   │          └──────────┬──────────────┘
                   │                     │
                   │          ┌──────────▼──────────────┐
                   │          │ Serial Monitor Server  │
                   │          │ (existing in mmemu)    │
                   │          └──────────┬──────────────┘
                   │                     │
    ┌──────────────▼──────────────────────▼──────────────────┐
    │ mmemu CPU / Memory / Debug Context                     │
    └───────────────────────────────────────────────────────┘
```

## Phase 5.1: JTAG Loopback Device

The JTAG Loopback Device abstracts the underlying TCP connection to the mmemu Serial Monitor Server.

### File: `jtag_loopback.lua`

**Purpose**: Provide a thin TCP client wrapper around the Serial Monitor Server

**Key Methods**:
```lua
jtag = JTAGLoopback.new(host, port)  -- Create instance
jtag:connect()                         -- Connect to Serial Monitor Server
jtag:send_command(cmd)                 -- Send text command, get response
jtag:disconnect()                      -- Close connection
```

**Transport**: TCP over localhost (mmemu) or USB JTAG adapter (real hardware)

**Default Endpoints**:
- mmemu: `localhost:6502` (SerialMonitorServer default port)
- Real MEGA65: TE0790-03 JTAG adapter with FTDI USB driver

## Phase 5.2: Hardware Backend with JTAG Serial Monitor

The Hardware Backend implements the backend abstraction interface using MEGA65 Matrix Mode Monitor commands.

### File: `backend_hardware_jtag.lua`

**Purpose**: Translate backend abstraction calls into Matrix Mode Monitor commands

**Supported Commands** (from MEGA65 book section K):
- `R` — Get register state (A, X, Y, SP, P, PC)
- `M<addr>` — Memory dump at address
- `S<addr><val>` — Set memory byte
- `G<addr>` — Set program counter (SETPC)
- `D<addr>` — Disassemble
- `B<addr>` — Set breakpoint
- etc.

**Example Usage**:
```lua
local HardwareBackend = require("backend_hardware_jtag")

local hw = HardwareBackend.new("localhost", 6502)
if hw:connect() then
    hw:write_byte(0x0800, 0x42)
    local val = hw:read_byte(0x0800)
    assert(val == 0x42)
    hw:disconnect()
end
```

## Running Tests on mmemu (Mock Hardware)

The SerialMonitorServer is already built into mmemu CLI:

```bash
# Start mmemu with serial monitor enabled (default)
./bin/mmemu-cli -m c64

# In another terminal, test JTAG connection
lua -e "
    local jtag = require('jtag_loopback')
    local device = jtag.new('localhost', 6502)
    if device:connect() then
        print('Connected!')
        print(device:send_command('r'))  -- Get registers
        device:disconnect()
    end
"
```

## Running Backend Tests on mmemu

```bash
cd examples/lua

# Test emulator backend (works directly)
echo -e "script run test_suite_backend.lua\nquit" | ../../bin/mmemu-cli -m c64

# Test hardware backend via JTAG loopback
# Terminal 1: Start mmemu
./bin/mmemu-cli -m c64

# Terminal 2: Run test suite
lua -e "
    local HW = require('backend_hardware_jtag')
    local TestFramework = require('test_framework')
    
    local hw = HW.new('localhost', 6502)
    if hw:connect() then
        local tests = TestFramework.create(hw)
        tests:add_test('test_name', function(backend)
            -- Test code here
            return true
        end)
        tests:run_all()
        hw:disconnect()
    end
"
```

## Matrix Mode Monitor Protocol Details

### Command Format
```
<COMMAND><ARGS>
```

Commands are case-insensitive and terminated with newline (`\n` or `\r`).

### Register Commands
```
R               → "A=XX X=XX Y=XX SP=XX P=XX PC=XXXX"
```

### Memory Commands
```
M<addr16>       → 256 bytes of memory dump (16 rows × 16 bytes)
S<addr16><val>  → Set memory byte at address
```

### Execution Commands
```
G<addr16>       → Set program counter
D<addr>         → Disassemble instruction
B<addr>         → Set breakpoint
```

### Response Format
- **Success**: Formatted data (hex values, register state, etc.)
- **Error**: Line beginning with "ERROR"
- **Multi-line**: Multiple response lines for dumps

## MEGA65 Hardware Implementation (TODOs)

To run tests on real MEGA65 hardware, the following must be implemented:

1. **Serial Monitor Program** in MEGA65 hypervisor ($8000+)
   - Listen on USB serial (FTDI TE0790-03)
   - Parse text commands
   - Execute Matrix Mode Monitor commands
   - Return formatted responses

2. **Baud Rate Control**
   - Default: 2,000,000 bps (controllable via `+` command)
   - Allows testing at various speeds

3. **Memory Access**
   - Read 28-bit address space
   - Write to RAM/registers
   - Handle DMA and banking transparently

## Testing Workflow

### 1. Development Phase (mmemu)
```bash
# Rapid iteration with built-in Serial Monitor Server
./bin/mmemu-cli -m c64
# Tests run via JTAG loopback to localhost:6502
```

### 2. Hardware Validation Phase (Real MEGA65)
```bash
# Same test code, different endpoint
HardwareBackend.new("ftdi://0", 6502)  -- Connect to real hardware
# Tests run via TE0790-03 JTAG adapter
```

### 3. CI/CD Pipeline
```bash
# Stage 1: Test on emulator (fast)
make test-backend-emulator

# Stage 2: Test on hardware (if available)
make test-backend-hardware HARDWARE_PORT=/dev/ttyUSB0
```

## References

- MEGA65 Book Section K: Machine Language Monitor
- MEGA65 Book Section J: Hypervisor Calls
- Serial Monitor Server: `src/cli/main/serial_monitor_server.h`
- Matrix Mode Monitor: https://github.com/MEGA65/mega65-rom

## Future Enhancements

1. **GDB Integration** — Use GDB RSP over serial monitor
2. **Real-time Profiling** — Capture cycle-accurate traces
3. **Device I/O** — Direct SID/VIC-II manipulation
4. **Performance** — Optimize JTAG throughput (200KB/sec → target faster)
