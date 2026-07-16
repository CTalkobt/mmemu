# MMEMU Serial Monitor SDKs

High-level client libraries for integrating MMEMU debugging into your applications.

## Overview

The MMEMU Serial Monitor protocol allows external tools to connect to the simulator and control CPU execution, inspect memory, set breakpoints, and analyze program behavior. This SDK directory contains language-specific bindings and tools for quick integration.

## Available SDKs

### Python SDK

Full-featured Python SDK with high-level API and interactive tools.

**Features:**
- Clean Pythonic interface with automatic response parsing
- Exception-based error handling
- Helper classes for registers, flags, and instructions
- Built-in example tools (memory inspector, breakpoint manager)
- Type hints for IDE integration

**Quick Start:**
```bash
python3 sdk/python/examples/memory_inspector.py --host localhost --port 2000
```

**Documentation:** See [python/README.md](python/README.md)

### C++ SDK

Native C++ library for high-performance integration.

**Features:**
- Zero-copy socket communication
- Standard C++ exceptions
- STL container support
- CMake build system
- Example programs included

**Build:**
```bash
mkdir build && cd build
cmake ..
make
./memory_inspector --host localhost --port 2000
```

**Header:** [cpp/include/mmemu_serial_monitor.h](cpp/include/mmemu_serial_monitor.h)

## Protocol Overview

The Serial Monitor Protocol is a text-based TCP command interface:

```
Connected to MMEMU Serial Monitor on port 2000

# Read CPU registers
Command: r
Response: PC=002000 A=42 X=01 Y=02 SP=01F8 P=30

# Read memory at address
Command: m 2000
Response: 002000: 4C 30 E5 A2 ...

# Set breakpoint
Command: b 2050
Response: OK

# Disassemble from address
Command: d 2000
Response: 002000 4C 30 E5    JMP $E530
         002003 A2 00       LDX #$00
```

**14 core commands:**
- `R` - Read registers
- `M` - Read memory
- `S` - Set memory
- `D` - Disassemble
- `G` - Set program counter
- `B` - Breakpoints
- `E` - Flag watch
- `I` - Interrupts
- `T` - Trace control
- `Z` - CPU history
- `@` - CPU memory view
- `+` - UART divisor
- `?` - Help

See [doc/SERIAL_MONITOR_PROTOCOL.md](../doc/SERIAL_MONITOR_PROTOCOL.md) for complete specification.

## Common Integration Patterns

### Pattern 1: Interactive Debugger

```python
from mmemu_serial_monitor import SerialMonitor

mm = SerialMonitor()
mm.connect()

while True:
    cmd = input("> ")
    if cmd == "quit":
        break
    
    if cmd.startswith("b "):
        addr = int(cmd[2:], 16)
        mm.set_breakpoint(addr)
    elif cmd == "regs":
        regs = mm.read_registers()
        for name, val in regs.items():
            print(f"{name}=${val:06X}")
```

### Pattern 2: Automated Testing

```cpp
SerialMonitor mm("localhost", 2000);
mm.connect();

// Run test program
mm.setPc(0x2000);
std::vector<uint8_t> program = loadTestProgram();
mm.writeMemoryBlock(0x2000, program);

// Set breakpoint at end
mm.setBreakpoint(0x2050);

// Wait for hit (poll)
while (true) {
    auto regs = mm.readRegisters();
    if (regs["PC"] == 0x2050) {
        break;
    }
}

// Verify result
if (regs["A"] == 0x42) {
    std::cout << "Test PASSED" << std::endl;
}
```

### Pattern 3: Real-time Monitoring

```python
import time
from mmemu_serial_monitor import SerialMonitor

mm = SerialMonitor()
mm.connect()
mm.enable_trace()

# Monitor for 10 seconds
start = time.time()
while time.time() - start < 10:
    regs = mm.read_registers()
    print(f"PC=${regs['PC']:06X} A=${regs['A']:02X}")
    time.sleep(0.1)

# Analyze trace
history = mm.get_cpu_history()
print(history)
```

## Example Tools Included

### Memory Inspector (Python)
Interactive memory viewer with hex dump and disassembly.
```bash
python3 sdk/python/examples/memory_inspector.py --host localhost --port 2000

Commands:
  read 2000 100    - Read 256 bytes from $2000
  write 2050 42    - Write $42 to $2050
  disasm 2000 16   - Disassemble 16 instructions
  break 3000       - Set breakpoint
  regs             - Show all registers
  clear            - Clear breakpoints
```

### Breakpoint Manager (Python)
Manage breakpoints and instruction tracing.
```bash
python3 sdk/python/examples/breakpoint_manager.py add 2000 2050 3000
python3 sdk/python/examples/breakpoint_manager.py list
python3 sdk/python/examples/breakpoint_manager.py trace on
python3 sdk/python/examples/breakpoint_manager.py dump-trace
```

### Memory Inspector (C++)
C++ version of memory inspector tool.
```bash
./build/memory_inspector --host localhost --port 2000
```

## Building MMEMU with Serial Monitor

```bash
# Start MMEMU with serial monitor server on default port 2000
./bin/mmemu-cli -m mega65 --serial-monitor-port 2000

# Or use custom port
./bin/mmemu-cli -m mega65 --serial-monitor-port 3000
```

## Testing

Integration tests are provided to verify protocol compliance:

```bash
# In terminal 1: Start MMEMU
./bin/mmemu-cli -m mega65 --serial-monitor-port 2000

# In terminal 2: Run tests
python3 tests/serial_monitor_integration_test.py
```

## Performance Characteristics

### Latencies (typical)
- Single register read: ~5 ms
- 256-byte memory read: ~10 ms
- Disassemble 16 instructions: ~20 ms
- Set breakpoint: ~5 ms

### Throughput
- Default baud rate: 2,000,000 bps (200 KB/sec)
- Configurable via `+` command (divisor 1-65535)
- Single byte: ~5 µs
- 256-byte block: ~1.2 ms
- Full 256 KB dump: ~1.3 sec

## Troubleshooting

### "Connection refused"
MMEMU is not running or port is different:
```bash
# Check if running
netstat -an | grep 2000

# Start MMEMU
./bin/mmemu-cli -m mega65 --serial-monitor-port 2000
```

### "Timeout waiting for response"
Increase timeout when connecting:
```python
mm = SerialMonitor(timeout=10.0)  # 10 seconds
```

### Python SDK import errors
Add SDK to Python path:
```bash
export PYTHONPATH="/path/to/mmsim/sdk/python:$PYTHONPATH"
```

## Architecture

### Client-Server Model

```
┌──────────────┐
│ Your App     │
├──────────────┤
│ SDK Client   │ (Python or C++)
│ (Protocol)   │
└──────────────┘
      ↓ TCP
   ┌──────────────────┐
   │ MMEMU CLI        │
   ├──────────────────┤
   │ Serial Monitor   │
   │ Server           │
   └──────────────────┘
      ↓ Internal
   ┌──────────────────┐
   │ Simulator Core   │
   │ (CPU, Memory)    │
   └──────────────────┘
```

### Protocol State Machine

Each connection is independent:
- State persists within a session (breakpoints, trace buffer)
- Commands are stateless (each query independent)
- Last address remembered for memory/disasm continuation

## Extension Points

Future phases may add:
- **Binary protocol** (Phase 3): Raw byte transfer for fast bulk loads
- **Structured responses** (Phase 4): JSON output for tool integration
- **Event streaming**: Real-time breakpoint/trace events
- **IDE plugins**: VS Code, JetBrains IDE integration

## API Comparison

### Python
```python
mm = SerialMonitor('localhost', 2000)
mm.connect()
regs = mm.read_registers()
mm.set_breakpoint(0x2050)
mm.disconnect()
```

### C++
```cpp
mmemu::SerialMonitor mm("localhost", 2000);
mm.connect();
auto regs = mm.readRegisters();
mm.setBreakpoint(0x2050);
mm.disconnect();
```

## Contributing

To add support for additional languages:

1. Study [SERIAL_MONITOR_PROTOCOL.md](../doc/SERIAL_MONITOR_PROTOCOL.md)
2. Implement client library with 14 core commands
3. Add example tool and documentation
4. Submit PR with tests

## Support

- Full protocol specification: [SERIAL_MONITOR_PROTOCOL.md](../doc/SERIAL_MONITOR_PROTOCOL.md)
- Tool integration guide: [TOOL_INTEGRATION_GUIDE.md](../doc/TOOL_INTEGRATION_GUIDE.md)
- Integration tests: [tests/serial_monitor_integration_test.py](../tests/serial_monitor_integration_test.py)
- MEGA65 Book: Chapter 12 (Remote Debugging)
