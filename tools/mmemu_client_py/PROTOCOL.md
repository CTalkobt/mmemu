# MEGA65 Serial Monitor Protocol

This document describes the text-based serial monitor protocol implemented by mmemu.

## Overview

The serial monitor is a TCP-based debugging interface that allows remote tools to interact with the emulator. Commands are sent as ASCII text, one per line, and responses are returned as plain text.

**Connection Details:**
- Protocol: TCP/IP over localhost
- Port: Configurable (default 6502)
- Baud rate simulation: Configurable via `+` command
- Default: 2,000,000 bps (UART divisor = 20)

## Command Reference

### Registers

#### R - Read Registers
Read all CPU registers.

**Request:**
```
R
```

**Response:**
```
A=42 X=00 Y=FF SP=01FE PC=2000 P=30
```

Register format: `REG=HH` (2-digit hex)

### Memory

#### M - Read Memory
Read memory at specified address.

**Request:**
```
M 2000
```

**Response:**
```
2000 AD LDA 00 20 00
2002 29 AND #$FF
```

Format: `ADDR BYTE1 BYTE2 ...` (hex)
- First line contains address and bytes
- Multiple lines for larger reads

#### S - Write Memory
Write single byte to memory.

**Request:**
```
S 2000 42
```

**Response:**
```
OK
```

or on error:
```
ERROR: Invalid address
```

**Note:** Writing multiple bytes requires sending multiple S commands.

### Program Control

#### G - Set Program Counter (GOTO)
Set the program counter to a new address.

**Request:**
```
G 2000
```

**Response:**
```
PC set to $2000
```

### Disassembly

#### D - Disassemble
Disassemble instructions starting at address.

**Request:**
```
D 2000 10
```

Response (10 instructions):
```
2000 AD 00 20    LDA $2000
2003 29 FF       AND #$FF
2005 60          RTS
```

Format:
- `ADDR` - Instruction address (4-digit hex)
- Instruction bytes (2-digit hex each)
- Mnemonic and operands

### Breakpoints

#### B - Breakpoint Management
Set or list breakpoints.

**List breakpoints:**
```
B
```

Response:
```
$2000 (enabled)
$2050 (enabled)
```

**Set breakpoint:**
```
B 2050
```

Response:
```
Breakpoint set at $2050
```

### Watchpoints

#### W - Watchpoint Management
Set or list watchpoints.

**Similar to breakpoints:**
```
W 2000
```

### Execution Trace

#### T - Trace Control
Get or control execution trace.

**Request:**
```
T all
```

Modes:
- `all` - Full execution trace
- `memory` - Memory access trace
- `calls` - Function call trace

**Response:**
```
[trace entries...]
```

### History

#### Z - Execution History
Get execution history (requires trace buffer enabled).

**Request:**
```
Z
```

**Response:**
```
[history entries...]
```

### Debug Metadata

#### V - List Variables
List variables for a function (requires debug metadata).

**List all variables:**
```
V
```

**List function variables:**
```
V main
```

**Response:**
```
x @0000 size=2 type=int16 scope=parameter
y @0002 size=2 type=int16 scope=parameter
result @0004 size=2 type=int16 scope=local
```

Format: `NAME @OFFSET size=SIZE type=TYPE scope=SCOPE`

### Configuration

#### + - Set UART Divisor
Configure UART divisor (affects simulated baud rate).

**Request:**
```
+ 100
```

**Response:**
```
UART divisor set to 100
```

Baud rate = 100,000,000 / divisor

Common values:
- 20 = 2,000,000 bps
- 100 = 1,000,000 bps
- 200 = 500,000 bps

### Information

#### ? - Help
Display available commands and help.

**Request:**
```
?
```

**Response:**
```
Serial Monitor Commands:
R - Read registers
M [addr] - Read memory
...
```

## Address Formats

Addresses can be specified in several formats:

- **Hex** (preferred): `2000`, `0x2000`, `$2000`, `2000h`
- **Decimal**: `8192`
- **Binary**: `0b0010000000000000`

The parser will auto-detect the format based on the input.

## Response Codes

### Success
- `OK` - Command succeeded
- Direct data response - Command succeeded with data

### Errors
- `ERROR: ...` - Error message
- `INVALID ADDRESS` - Address out of range
- `INVALID COMMAND` - Unknown command

## Examples

### Reading CPU state
```
$ echo "R" | nc localhost 6502
A=00 X=00 Y=00 SP=01FF PC=2000 P=30
```

### Dumping memory
```
$ echo -e "M 2000\nM 2010" | nc localhost 6502
2000 AD 00 20 00 ...
2010 FF FF FF FF ...
```

### Setting breakpoint and reading code
```
$ echo -e "B 2050\nD 2050 5" | nc localhost 6502
Breakpoint set at $2050
2050 60          RTS
```

## Protocol Implementation Notes

1. **Line Endings**: Commands and responses use `\n` or `\r\n` (both accepted)
2. **Hex Format**: All hex values are uppercase (A-F)
3. **Timeouts**: Default 5 second timeout for responses
4. **Connection**: Single connection per client (not multiplexed)
5. **Buffering**: Commands are buffered per-line (no partial command processing)

## Future Extensions

Planned protocol extensions for Phase 4+:

- Memory breakpoint conditions (when value matches pattern)
- Hardware trace buffer commands
- Snapshot save/load
- Stack trace inspection
- Expression evaluation in breakpoints
- IDE-specific optimizations (batch reads, subscriptions)

## Related

- [Python Client Library](README.md)
- mmemu serial monitor implementation: `src/cli/main/serial_monitor_server.cpp`
