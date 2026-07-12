# Issue #100: GDB Protocol Compatibility and IDE Integration

## Overview

Issue #100 enhances mmemu's GDB Remote Serial Protocol (RSP) server to support IDE integration, enabling developers to use familiar tools like VS Code, CLion, and other GDB-compatible debuggers.

**Status:** ✓ GDB server already implemented (Phase 1), enhanced with metadata exchange (Phase 2)

## Architecture

### Phase 1: Basic GDB Protocol ✓ (Already Complete)

The GDB RSP server was implemented with support for:
- Register read/write (g, G)
- Memory read/write (m, M)
- Step/continue (s, c)
- Breakpoints (Z, z)
- Thread queries (qC, qfThreadInfo, etc.)
- Runs on configurable TCP port (`--gdb-port` CLI flag)

**Usage:**
```bash
mmemu-cli -m c64 --gdb-port 1234
```

Then connect with:
```bash
gdb target remote localhost:1234
```

### Phase 2: Metadata Exchange ✓ (NEW - Issue #100)

Enhanced the GDB protocol with mmemu-specific query commands to exchange debug metadata:

**New query commands:**
- `qMmemuSymbols:pattern` - Retrieve symbol table as JSON
- `qMmemuVariables:function` - Get variables for a function
- `qMmemuFrame` - Get current frame information

**Protocol flow:**
1. Debugger connects to mmemu GDB server
2. Debugger queries `qSupported` → mmemu advertises metadata extensions
3. Debugger can now request symbols, variables, frame info
4. Responses are hex-encoded JSON for compatibility

**Example responses:**

Symbols query:
```json
{
  "symbols": [
    {"name": "main", "addr": "0x2000"},
    {"name": "loop", "addr": "0x2010"},
    {"name": "exit", "addr": "0x2050"}
  ]
}
```

Variables query:
```json
{
  "variables": [
    {"name": "x", "addr": "0x10", "size": 2, "type": "int16"},
    {"name": "y", "addr": "0x12", "size": 2, "type": "int16"}
  ]
}
```

Frame query:
```json
{
  "pc": "0x2045",
  "sp": "0xfe",
  "frameSize": 256
}
```

## VS Code Integration

### Using mmemu with VS Code Debugger

**Step 1: Start mmemu with GDB server**
```bash
./bin/mmemu-cli -m c64 --gdb-port 1234
```

**Step 2: Create VS Code launch configuration** (`.vscode/launch.json`)
```json
{
  "version": "0.2.0",
  "configurations": [
    {
      "name": "mmemu (GDB)",
      "type": "cppdbg",
      "request": "launch",
      "MIMode": "gdb",
      "miDebuggerPath": "/usr/bin/gdb",
      "miDebuggerArgs": "target remote localhost:1234",
      "stopAtEntry": true,
      "preLaunchTask": "build",
      "cwd": "${workspaceFolder}"
    }
  ]
}
```

**Step 3: In VS Code, press F5 to start debugging**
- Set breakpoints by clicking line numbers
- Step through code with F10 (step over) / F11 (step into)
- View registers and memory in debug console
- Inspect variables (enhanced with mmemu metadata)

### CLion Integration

CLion supports GDB via its built-in GDB debugger:

1. **Settings → Build, Execution, Deployment → Debugger → GDB**
2. Set custom GDB server: `localhost:1234`
3. Create run configuration for mmemu
4. Press Shift+F9 to debug

## Implementation Details

### Query Response Format

All metadata responses use hex-encoded JSON for GDB compatibility:
1. Data is formatted as JSON
2. Each byte is converted to hex (`%02x`)
3. Response is sent as hex string (no binary data)

Example encoding:
```
String: {"pc":"0x2000"}
Hex:    7b22706322 3a223078 32303030 227d
```

### GDB Supported Features

mmemu now advertises in `qSupported` response:
```
PacketSize=4096;swbreak+;hwbreak-;mmemu-symbols+;mmemu-variables+;mmemu-frame+
```

This tells the client that mmemu supports:
- `swbreak+` - Software breakpoints
- `mmemu-symbols+` - Symbol table queries
- `mmemu-variables+` - Variable information queries
- `mmemu-frame+` - Frame information queries

## Use Cases

### 1. IDE Breakpoint Setting

Instead of CLI:
```bash
break $2050
```

In IDE, just click the line number to set a breakpoint. IDE sends `Z0,2050,1` to mmemu.

### 2. Variable Inspection

IDE can query variables for current function:
```
qMmemuVariables:_main
```

Responds with all variables in `_main`, IDE displays them in Variables pane.

### 3. Symbol Navigation

IDE can request all symbols:
```
qMmemuSymbols:
```

Use this to populate symbol completion, go-to-definition, etc.

### 4. Stack Frame Display

IDE queries frame info to show in stack pane:
```
qMmemuFrame
```

Shows current PC, SP, frame size for context.

## Limitations & Compatibility

### Current Limitations

1. **VS Code C/C++ Debugger** - Works but requires manual setup
2. **CLion** - Works via GDB integration
3. **GDB CLI** - Full compatibility
4. **LLDB** - Not directly supported (uses different protocol)

### Workarounds

For IDEs that don't support GDB directly, use:
1. GDB CLI → IDE via gdb.mi2
2. Custom IDE extensions (VS Code extension recommended)
3. GDB server proxy with protocol translation

## Future Enhancements (Phase 3)

Planned additions to GDB protocol:
- [ ] Conditional breakpoint expressions via GDB protocol
- [ ] Memory watch expressions
- [ ] Call stack frames with local variables
- [ ] Source-level stepping (line-based, not instruction-based)
- [ ] Expression evaluation (evaluate C expressions)
- [ ] Custom breakpoint actions (break → log)

## VS Code Extension (Future)

A native VS Code extension could:
1. Provide UI for mmemu-specific features
2. Auto-start mmemu server
3. Display emulation-specific info (memory maps, I/O devices, etc.)
4. Integrate with ROM/PRG file loading
5. Show VIC display as overlay during debugging

## Testing

### Manual Testing Steps

```bash
# Terminal 1: Start mmemu with GDB server
./bin/mmemu-cli -m c64 --gdb-port 1234

# Terminal 2: Connect with gdb
gdb
(gdb) target remote localhost:1234
(gdb) monitor info breakpoints    # mmemu-specific
(gdb) c                            # Continue
(gdb) s                            # Step
```

### Test Queries

```gdb
# Query supported features
qSupported

# Query symbol table
qMmemuSymbols:

# Query variables for _main function
qMmemuVariables:_main

# Query frame info
qMmemuFrame

# Set breakpoint at 0x2050
Z0,2050,1

# Continue execution
c

# Read registers
g

# Step one instruction
s
```

## Integration Points

- **GdbServer** - Enhanced with metadata handler methods
- **DebugContext** - Provides symbols, variables, frame info
- **VariableSymbolTable** - Supplies variable metadata (Issue #98)
- **SymbolTable** - Provides symbol-to-address mapping

## References

- [GDB Remote Serial Protocol](https://sourceware.org/gdb/onlinedocs/gdb/Remote-Protocol.html)
- [VS Code Debug Adapter Protocol](https://microsoft.github.io/debug-adapter-protocol/)
- [CLion GDB Configuration](https://www.jetbrains.com/help/clion/clion-with-remote-gdb-servers.html)

## Acceptance Criteria

- [x] GDB RSP server already implemented
- [x] New metadata query commands added
- [x] Symbol table exchange via GDB protocol
- [x] Variable information exchange via GDB protocol
- [x] Frame information exchange via GDB protocol
- [x] Enhanced `qSupported` response with mmemu capabilities
- [x] Documentation for VS Code integration
- [ ] VS Code extension (future work)
- [ ] CLion/GDB integration tested
- [ ] Expression evaluation support (future)

## Related Issues

- #93 - Stack Trace Viewer
- #94 - Variable Symbol Table  
- #95 - Source Location Hyperlinking
- #96 - Frame Inspection
- #98 - Debug Metadata Format
- #99 - Execution History
