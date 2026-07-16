# MEGA65 Serial Monitor Protocol Specification

## Overview

The Serial Monitor Protocol is a text-based command interface that runs on top of the JTAG connection to the MEGA65. This document specifies the protocol as implemented by MMEMU for full compatibility with MEGA65 development tools.

## Connection

- **Transport**: TCP socket (in MMEMU) or UART/JTAG (on real hardware)
- **Default Port**: 2000 (MMEMU)
- **Default Baud Rate**: 2,000,000 bps (40,500,000 / (divisor + 1), where divisor = 20)
- **Data Format**: ASCII text commands, hex address/value format
- **Line Terminator**: CR (0x0D) or LF (0x0A)

## Command Format

Commands are single uppercase letters optionally followed by hex arguments:

```
<COMMAND> [ARG1] [ARG2] [ARG3]
```

All addresses and values use hexadecimal notation without prefix (e.g., `2000` = 0x2000).
Addresses are 16-bit for C64/VIC-20, 28-bit for MEGA65.

## Core Commands (Phase 1)

### R - Read Registers

**Format**: `r`

**Response**: Single line with register dump
```
PC=002000 A=42 X=01 Y=02 SP=01F8 P=30
```

### M - Read Memory

**Format**: 
- `m [address]` - Read 256 bytes from address (or next 256 bytes if no address)

**Response**: 16 rows of 16 bytes each, hex + ASCII
```
002000: 4C 30 E5 A2 00 BF 00 F0  LO0…. (ASCII)
002010: 00 00 00 00 00 00 00 00  ........
...
```

### S - Set Memory

**Format**: `s <address> <value>`

**Response**: `OK`

### D - Disassemble

**Format**: `d [address]`

**Response**: 16 instructions disassembled
```
002000 4C 30 E5    JMP $E530
002003 A2 00       LDX #$00
...
```

### G - Set Program Counter

**Format**: `g <address>`

**Response**: `OK`

### B - Breakpoint

**Format**: 
- `b <address>` - Set breakpoint at address
- `b` - Clear all breakpoints

**Response**: `OK`

### ? - Help

**Format**: `?` or `h`

**Response**: Short command summary

### + - UART Divisor

**Format**: `+ <divisor>`

**Response**: `UART Divisor: 14 (2000000 bps)`

## Advanced Commands (Phase 2)

### T - Trace

**Format**:
- `t on` or `t 1` - Enable instruction tracing
- `t off` or `t 0` - Disable tracing
- `t dump` - Show last 16 traced instructions

**Response**:
```
Trace ON at PC=002000
```

or (for dump):
```
Trace buffer (last 16 instructions):
002000 JMP $E530
002003 LDX #$00
...
```

### Z - CPU History

**Format**: `z`

**Response**: Last 32 instructions with cycle count and register state
```
CPU History (last 32 instructions):
002000 JMP $E530 Cycles=3 A=00
002003 LDX #$00 Cycles=2 A=00
...
```

### E - Flag Watch

**Format**: `e <flag>`

**Flags**: N (Negative), V (Overflow), B (Break), D (Decimal), I (Interrupt), Z (Zero), C (Carry)

**Response**: 
```
Negative (N) = CLEAR
```

### I - Interrupts

**Format**:
- `i enable` or `i on` - Enable CPU interrupts
- `i disable` or `i off` - Disable CPU interrupts
- `i status` - Check interrupt status

**Response**:
```
Interrupts ENABLED
```

### @ - CPU Memory

**Format**: `@`

**Response**: CPU memory view and current address space
```
CPU View:
PC=002000
Memory configuration:
  Address space: 28-bit (256MB MEGA65)
```

### L - Load Memory

**Format**: `l <start_addr> <end_addr>`

**Response**: Acknowledgment for data transfer (Phase 3 will implement binary protocol)
```
LOAD 002000-002100 (256 bytes) ready for data
```

### W - Watchpoint

**Format**: 
- `w <address>` - Set write watchpoint at address
- `w` - Clear all watchpoints

**Response**: `OK`

## Error Responses

All error conditions return a response starting with `ERROR:`:

```
ERROR: No CPU
ERROR: No bus
ERROR: No debug context
ERROR: Unknown command 'X'
ERROR: Invalid address or step count '...'
ERROR: <flag> (flag lookup error)
ERROR: <flag> (N,V,B,D,I,Z,C)
ERROR: R <addr> <value>
ERROR: ... (command format errors)
```

## Address Encoding

### 16-bit Addresses (C64/VIC-20)
- Format: 4 hex digits
- Range: $0000 - $FFFF
- Example: `2000` = 0x2000 = 8192

### 28-bit Addresses (MEGA65)
- Format: Up to 7 hex digits (28-bit = 0x0XXXXXX)
- Range: $0000000 - $FFFFFFF (268MB)
- Bank notation: $MM:XXXXX (megabyte:offset)
- Example: `01:2000` = 0x012000

## Value Encoding

All values use hexadecimal:
- Single byte: 2 hex digits (00-FF)
- 16-bit word: 4 hex digits (0000-FFFF)
- 28-bit address: Up to 7 hex digits

## Protocol State

The serial monitor maintains state between commands:

- **Last memory address**: Updated by `M` command, used for continuation
- **Last search address**: Used by future `findnext`/`findprior` commands
- **UART divisor**: Current baud rate divisor
- **Trace buffer**: Circular buffer of last ~1000 instructions
- **Breakpoints**: Persistent across commands until cleared

## Performance Characteristics

### Baud Rate
Default 2,000,000 bps (~250 KB/sec theoretical)
Can be adjusted with `+` command:
- Minimum: 1 (~40 MB/s) - practically limited by serial hardware
- Maximum: 65535 (~617 bps)

### Memory Operations
- Single byte read/write: ~10 µs
- 256-byte block: ~1-2 ms at 2M bps
- Full page (64KB) dump: ~250 ms

### Instruction Fetch
- Disassembly: ~100 µs per instruction
- Full history (32 instructions): ~3 ms

## Compatibility Notes

### With m65 CLI Tool
The m65 tool expects:
- Hex address/value format (no 0x prefix)
- Case-insensitive commands
- Response format matching above specification

### With Real Hardware
MMEMU's serial monitor is designed to be protocol-compatible with real MEGA65:
- Same command set
- Same response formats
- Same address/value encoding
- Equivalent performance characteristics

## Extension Points

Future phases may add:
- **Binary memory loading** (Phase 3): Raw byte transfer protocol for fast bulk loads
- **IDE support** (Phase 4): JSON responses for structured data
- **Tape operations**: Datasette control commands
- **Cartridge control**: Cartridge image loading and swapping

## Examples

### Read and display registers
```
> r
PC=002000 A=42 X=01 Y=02 SP=01F8 P=30
```

### Read memory at address 0x2000
```
> m 2000
002000: 4C 30 E5 A2 00 BF 00 F0  8D 78 D0 A5 01 F0 03 60  |LO0……….x……`.
002010: A9 00 8D 78 D0 4C F1 E5  FF FF FF FF FF FF FF FF  |….x.L…………|
...
```

### Set breakpoint and run
```
> b 2050
OK
> (program will halt when PC reaches $2050)
```

### Check CPU flags
```
> e z
Zero (Z) = SET
> e c
Carry (C) = CLEAR
```

### Enable interrupts
```
> i on
Interrupts ENABLED
```

### Show CPU trace
```
> t dump
Trace buffer (last 16 instructions):
002000 JMP $E530
002003 LDX #$00
...
```

## See Also

- MEGA65 Book: Serial Monitor Interface (Chapter K)
- MMEMU Source: `src/cli/main/serial_monitor_server.cpp`
- m65 CLI Tool: https://github.com/mega65/mega65-tools
