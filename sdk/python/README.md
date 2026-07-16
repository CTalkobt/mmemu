# MMEMU Serial Monitor Python SDK

A Python SDK for interacting with MMEMU's Serial Monitor Protocol, providing high-level access to MEGA65 simulation and debugging capabilities.

## Installation

```bash
# Copy SDK to your project
cp sdk/python/mmemu_serial_monitor.py /your/project/

# Or add to Python path
export PYTHONPATH="/path/to/mmsim/sdk/python:$PYTHONPATH"
```

## Quick Start

```python
from mmemu_serial_monitor import SerialMonitor

# Connect to MMEMU
mm = SerialMonitor('localhost', 2000)
mm.connect()

# Read CPU registers
regs = mm.read_registers()
print(f"PC: ${regs['PC']:06X}")

# Read memory
memory = mm.read_memory(0x2000, 256)
print(f"Memory: {memory.hex()}")

# Disassemble
instrs = mm.disassemble(0x2000, 8)
for instr in instrs:
    print(f"  {instr}")

# Set breakpoint
mm.set_breakpoint(0x2050)

# Disconnect
mm.disconnect()
```

## API Reference

### Core Class: SerialMonitor

#### Constructor
```python
SerialMonitor(host='localhost', port=2000, timeout=5.0)
```

#### Connection Methods
- `connect()` - Connect to MMEMU serial monitor server
- `disconnect()` - Close connection
- `is_connected()` -> bool - Check connection status

#### CPU Control
- `read_registers()` -> Dict[str, int] - Read all CPU registers (PC, A, X, Y, SP, P)
- `set_pc(addr: int)` - Set program counter to address
- `enable_interrupts()` - Enable CPU interrupts
- `disable_interrupts()` - Disable CPU interrupts
- `get_interrupt_status()` -> bool - Check if interrupts enabled

#### Memory Operations
- `read_memory(addr=0, length=256)` -> bytes - Read memory block
- `write_memory(addr: int, value: int)` - Write single byte to memory
- `write_memory_block(addr: int, data: bytes)` - Write multiple bytes

#### Debugging
- `disassemble(addr=0, count=16)` -> List[Instruction] - Disassemble N instructions
- `set_breakpoint(addr: int)` - Set breakpoint at address
- `clear_breakpoints()` - Clear all breakpoints
- `get_flag(flag: str)` -> bool - Check CPU flag (N, V, B, D, I, Z, C)
- `enable_trace()` - Enable instruction trace
- `disable_trace()` - Disable instruction trace
- `get_trace_dump()` -> str - Get trace buffer contents
- `get_cpu_history()` -> str - Get last 32 instructions with details
- `get_cpu_view()` -> str - Get CPU memory view
- `help()` -> str - Get command help

### Helper Classes

#### Register
```python
Register(name, value, width=8)
```
Represents a CPU register with metadata.

#### CPUFlags
```python
flags = CPUFlags(p_value)
if flags['N']:  # Check Negative flag
    print("Negative flag is set")
print(repr(flags))  # Show all flags like "NV-DI-C"
```

#### Instruction
```python
instr = Instruction(addr, mnemonic, operands)
print(f"{instr.addr:06X} {instr.complete}")
```

### Exception Classes

- `SerialMonitorException` - Base exception for all protocol errors
- `ConnectionError` - Connection-related errors
- `ProtocolError` - Protocol parsing or command errors

## Examples

### Example 1: Memory Inspector (Interactive)

```python
from mmemu_serial_monitor import SerialMonitor

mm = SerialMonitor()
mm.connect()

# Read and display memory in hex
addr = int(input("Address: "), 16)
data = mm.read_memory(addr, 256)

for offset in range(0, len(data), 16):
    hex_part = ' '.join(f'{b:02X}' for b in data[offset:offset+16])
    ascii_part = ''.join(chr(b) if 32 <= b < 127 else '.' 
                        for b in data[offset:offset+16])
    print(f"{addr + offset:06X}: {hex_part:<48} {ascii_part}")

mm.disconnect()
```

### Example 2: Breakpoint Manager

```python
from mmemu_serial_monitor import SerialMonitor

mm = SerialMonitor()
mm.connect()

# Set multiple breakpoints
for addr in [0x2000, 0x2050, 0x3000]:
    mm.set_breakpoint(addr)
    print(f"Breakpoint set at ${addr:06X}")

# Check CPU state
regs = mm.read_registers()
if regs['PC'] == 0x2050:
    print("Hit breakpoint at $2050!")
    # Examine memory around PC
    data = mm.read_memory(0x2050, 32)
    print(f"Memory: {data.hex()}")

mm.disconnect()
```

### Example 3: Program Tracer

```python
from mmemu_serial_monitor import SerialMonitor

mm = SerialMonitor()
mm.connect()

# Enable tracing and run program
mm.enable_trace()
# (program runs...)
mm.disable_trace()

# Show execution history
history = mm.get_cpu_history()
print(history)

# Dump trace buffer
dump = mm.get_trace_dump()
print(dump)

mm.disconnect()
```

### Example 4: Flag Inspector

```python
from mmemu_serial_monitor import SerialMonitor, CPUFlags

mm = SerialMonitor()
mm.connect()

regs = mm.read_registers()
flags = CPUFlags(regs['P'])

print(f"Flags: {flags}")  # Shows like "NV-DI-C"

# Check individual flags
if flags['Z']:
    print("Zero flag is set")
if flags['C']:
    print("Carry flag is set")

mm.disconnect()
```

## Bundled Example Tools

The SDK includes several ready-to-use example tools:

### Memory Inspector
```bash
python3 sdk/python/examples/memory_inspector.py --host localhost --port 2000

# Interactive commands:
read 2000 100    # Read 256 bytes from $2000
write 2050 42    # Write $42 to $2050
disasm 2000 16   # Disassemble 16 instructions
regs             # Show all registers
break 3000       # Set breakpoint
clear            # Clear breakpoints
```

### Breakpoint Manager
```bash
python3 sdk/python/examples/breakpoint_manager.py --host localhost --port 2000 add 2000 2050 3000

python3 sdk/python/examples/breakpoint_manager.py --host localhost --port 2000 list

python3 sdk/python/examples/breakpoint_manager.py --host localhost --port 2000 clear

python3 sdk/python/examples/breakpoint_manager.py --host localhost --port 2000 trace on
```

## Integration Patterns

### Pattern 1: IDE Integration

Integrate MMEMU debugging into your IDE:

```python
class MmemuDebugger:
    def __init__(self, project):
        self.mm = SerialMonitor()
        self.breakpoints = {}
    
    def connect(self):
        self.mm.connect()
    
    def set_breakpoint(self, file, line):
        addr = self.resolve_address(file, line)
        self.mm.set_breakpoint(addr)
        self.breakpoints[addr] = (file, line)
    
    def step(self):
        # Fetch current state
        regs = self.mm.read_registers()
        # Check which breakpoint hit
        if regs['PC'] in self.breakpoints:
            file, line = self.breakpoints[regs['PC']]
            return f"Hit breakpoint at {file}:{line}"
```

### Pattern 2: Automated Testing

Test assembly programs:

```python
def run_test(program_addr, expected_result):
    mm = SerialMonitor()
    mm.connect()
    
    # Set breakpoint at test end
    mm.set_breakpoint(program_addr + 100)
    
    # Run program (via separate control)
    mm.set_pc(program_addr)
    
    # Wait for breakpoint (poll)
    while True:
        regs = mm.read_registers()
        if regs['PC'] == program_addr + 100:
            break
        time.sleep(0.01)
    
    # Verify result
    if regs['A'] == expected_result:
        print("✓ Test passed")
    else:
        print(f"✗ Test failed: expected {expected_result}, got {regs['A']}")
    
    mm.disconnect()
```

### Pattern 3: Real-time Monitoring

Monitor CPU state during execution:

```python
def monitor_cpu(duration=5):
    mm = SerialMonitor()
    mm.connect()
    
    import time
    start = time.time()
    
    while time.time() - start < duration:
        regs = mm.read_registers()
        print(f"PC=${regs['PC']:06X} A=${regs['A']:02X} X=${regs['X']:02X} Y=${regs['Y']:02X}")
        time.sleep(0.1)
    
    mm.disconnect()
```

## Performance Notes

### Typical Latencies
- Single register read: ~5 ms
- 256-byte memory read: ~10 ms
- Disassemble 16 instructions: ~20 ms
- Set breakpoint: ~5 ms

### Optimization Tips

1. **Batch operations**: Read all registers once, not individually
2. **Reuse connections**: Keep connection open for multiple operations
3. **Limit trace dumps**: Don't dump massive trace buffers frequently
4. **Use addresses directly**: Symbols are resolved at protocol level

## Troubleshooting

### "Connection refused"
```python
# Check if MMEMU is running
import socket
try:
    sock = socket.socket()
    sock.connect(('localhost', 2000))
    sock.close()
    print("MMEMU is running")
except:
    print("MMEMU is not running on port 2000")
```

### "Timeout waiting for response"
```python
# Increase timeout for slow operations
mm = SerialMonitor(timeout=10.0)  # 10 seconds instead of default 5
```

### "Protocol error: Invalid format"
Ensure MMEMU's serial monitor server is running:
```bash
./bin/mmemu-cli -m mega65 --serial-monitor-port 2000
```

## See Also

- [Serial Monitor Protocol Specification](../../doc/SERIAL_MONITOR_PROTOCOL.md)
- [Tool Integration Guide](../../doc/TOOL_INTEGRATION_GUIDE.md)
- [Integration Tests](../../tests/serial_monitor_integration_test.py)
- MEGA65 Book: Chapter 12 (Remote Debugging)
