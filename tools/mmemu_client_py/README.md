# mmemu-client-py

Python client library for MEGA65 emulator (mmemu) serial monitor interface.

## Installation

```bash
pip install mmemu-client
```

## Quick Start

```python
from mmemu_client import MMemuClient

# Connect to emulator
client = MMemuClient('localhost', 6502)

# Read registers
regs = client.read_registers()
print(f"A=${regs['A']:02X} X=${regs['X']:02X}")

# Read memory
data = client.read_memory(0x2000, 16)
print(' '.join(f'{b:02X}' for b in data))

# Set breakpoint
client.set_breakpoint(0x2050)

# Disassemble
disasm = client.disassemble(0x2000, count=10)
for addr, instr in disasm:
    print(f"${addr:04X}  {instr}")

# List variables (requires debug metadata)
vars = client.list_variables('main')
for var in vars:
    print(f"  {var['name']} @ ${var['offset']:04X} ({var['type']})")
```

## API Reference

### MMemuClient

Main client class for communicating with mmemu serial monitor.

#### Methods

- `read_registers()` → dict
  - Returns: {'A', 'X', 'Y', 'SP', 'PC', 'P'}

- `read_memory(addr, length)` → bytes
  - addr: Start address (int or hex string)
  - length: Number of bytes to read
  - Returns: bytes

- `write_memory(addr, data)` → bool
  - addr: Start address
  - data: bytes or bytearray

- `set_pc(addr)` → bool
  - Set program counter

- `disassemble(addr, count=16)` → List[Tuple[int, str]]
  - Returns: [(address, instruction_string), ...]

- `set_breakpoint(addr)` → bool
  - Set breakpoint at address

- `list_breakpoints()` → List[dict]
  - Returns: [{'addr': int, 'enabled': bool}, ...]

- `set_watchpoint(addr)` → bool
  - Set watchpoint at address

- `get_history()` → List[dict]
  - Returns execution history

- `list_variables(function_name)` → List[dict]
  - Returns: [{'name': str, 'offset': int, 'type': str, 'scope': str}, ...]

- `get_trace(mode)` → List[dict]
  - Get execution trace
  - mode: 'all' | 'memory' | 'calls'

### Utilities

- `parse_addr(s: str) → int` - Parse hex/decimal/binary address
- `format_regs(regs: dict) → str` - Format registers for display

## Examples

See `examples/` directory for complete working examples:

- `memory_dump.py` - Dump memory region to file
- `breakpoint_manager.py` - Interactive breakpoint editor
- `screenshot.py` - Capture and decode screen memory
- `disasm_with_symbols.py` - Disassemble with symbol annotations

## Protocol Details

The serial monitor protocol is text-based over TCP:

```
Client: R                    # Read registers
Server: A=42 X=00 Y=FF ...   # Response

Client: M 2000               # Read memory at $2000
Server: AD LDA 00            # Response

Client: D 2000 10            # Disassemble 10 instructions at $2000
Server: 2000 AD 00 20 LDA ... # Multiple lines
        2002 ...
```

See PROTOCOL.md for complete command reference.

## Development

```bash
cd tools/mmemu_client_py
pip install -e .  # Install in development mode
pytest tests/     # Run tests
```

## License

Same as mmemu (MIT)
