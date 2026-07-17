# mmemu Lua Debugger for VS Code

VS Code integration for mmemu Lua scripting with comprehensive debugging and IDE features.

## Features

### 🎯 Script Execution
- **Run Current Script** (`Ctrl+Shift+E` / `Cmd+Shift+E`)
  - Execute Lua scripts directly from the editor
  - Automatic path handling for script discovery
  - Live console output

### 🔴 Breakpoint Management
- **Toggle Breakpoint** (`Ctrl+Shift+B` / `Cmd+Shift+B`)
  - Set/clear breakpoints at any line
  - Automatic sync with mmemu debugger
  - Visual gutter indicators

### 👟 Step Execution
- **Step Into** (`Ctrl+Shift+I` / `Cmd+Shift+I`)
  - Step into Lua functions
  - Watch variables change in real-time

### ▶️ Execution Control
- **Continue** (`Ctrl+Shift+C` / `Cmd+Shift+C`)
  - Resume execution after breakpoint
  - Pause at next breakpoint

### 🔍 Variable Inspection
- **Inspect Variable at Cursor**
  - Hover over variables to see values
  - View CPU registers: A, X, Y, PC, SP, P
  - Inspect mmemu backend state

### 💾 Memory Viewer
- **Show Memory Region**
  - Display memory as hex dump
  - Jump to specific addresses
  - Support for address expressions (e.g., `$100`, `0x2000`)

### 🎨 Syntax Highlighting
- **Lua with mmemu Extensions**
  - Highlight mmemu API functions
  - Color-coded stdlib/device_io calls
  - Device constants and register names

## Installation

### From VS Code Marketplace
1. Search for "mmemu Lua Debugger" in Extensions
2. Click Install
3. Restart VS Code

### From Source
```bash
cd vscode-mmemu-lua
npm install
npm run compile
vsce package
# Install the .vsix file in VS Code
```

## Configuration

Open VS Code settings (`Ctrl+,` / `Cmd+,`) and configure:

```json
{
  "mmemu.host": "localhost",           // mmemu server host
  "mmemu.port": 9999,                  // mmemu debugger port
  "mmemu.machine": "c64",              // Default machine (c64, vic20, mega65, pet)
  "mmemu.autoConnect": true,           // Auto-connect on startup
  "mmemu.showVariablesOnBreakpoint": true  // Show vars when paused
}
```

## Quick Start

### 1. Start mmemu CLI
```bash
./bin/mmemu-cli -m c64
```

### 2. Open Lua Script in VS Code
```bash
code examples/lua/test_suite.lua
```

### 3. Connect to mmemu
Press `Ctrl+Shift+P` → "mmemu: Start Debugger"

### 4. Run or Debug
- **Run**: `Ctrl+Shift+E` to execute script
- **Debug**: Set breakpoint with `Ctrl+Shift+B`, then run
- **Step**: Use `Ctrl+Shift+I` to step through code
- **Continue**: Press `Ctrl+Shift+C` after breakpoint

## Commands

| Command | Shortcut | Description |
|---------|----------|-------------|
| Start Debugger | - | Connect to mmemu instance |
| Run Script | `Ctrl+Shift+E` | Execute current script |
| Step Into | `Ctrl+Shift+I` | Step into function |
| Continue | `Ctrl+Shift+C` | Resume after breakpoint |
| Toggle Breakpoint | `Ctrl+Shift+B` | Set/clear breakpoint |
| Inspect Variable | - | Show variable value |
| Show Registers | - | Display CPU state |
| Show Memory | - | Open memory viewer |

## Workflow Examples

### Testing a Memory Pattern
```lua
-- examples/lua/test_pattern.lua
local patterns = require("memory_patterns")
local test_utils = require("test_utils")

local pattern = patterns.address_pattern(256, 0x100)
-- Set breakpoint here
patterns.fill(backend, 0x100, pattern)
```

1. Open in VS Code
2. Set breakpoint at `patterns.fill()` line
3. Press `Ctrl+Shift+E` to run
4. Debugger pauses at breakpoint
5. Hover over `pattern` to inspect
6. Press `Ctrl+Shift+C` to continue

### Debugging Device I/O
```lua
-- examples/lua/test_sid.lua
local device_io = require("device_io")

-- Set breakpoint here
device_io.SID_set_frequency(backend, 1, 440)
device_io.SID_gate(backend, 1, true)
```

1. Set breakpoint before SID initialization
2. Run script with `Ctrl+Shift+E`
3. Step through with `Ctrl+Shift+I`
4. Use "Show Registers" to verify audio state

## Architecture

### Components

- **extension.ts** — Main VS Code extension
  - Command registration
  - UI integration
  - Event handling

- **debugger.ts** — mmemu Communication
  - Socket-based CLI protocol
  - Command execution
  - Response parsing
  - Breakpoint management

- **lua-mmemu.json** — Syntax Highlighting
  - mmemu API keywords
  - Device I/O functions
  - stdlib/patterns/profiler
  - Comment and string highlighting

### Communication Protocol

The extension communicates with mmemu CLI via:

```
[VS Code Extension] <--TCP--> [mmemu CLI]
                               (localhost:9999 by default)
```

Commands are sent as text lines:
```
script run /path/to/script.lua
r
step
break $1000
```

Responses are parsed as text output.

## Troubleshooting

### "Failed to connect to mmemu"
- Ensure mmemu CLI is running
- Check host/port configuration
- Verify firewall allows localhost:9999

### Breakpoints not syncing
- Ensure mmemu supports breakpoint actions (Phase 4+)
- Check that Lua runtime is available (`lua5.4-dev`)
- Try manual `break` command in mmemu CLI

### Memory viewer shows no data
- Ensure machine is initialized
- Use valid hex addresses (e.g., `$100`, `$D000`)
- Check address range is readable

### Syntax highlighting not working
- Ensure file extension is `.lua`
- Verify extension is activated (check VS Code output)
- Restart VS Code if needed

## Development

### Build
```bash
npm run compile
```

### Watch Mode
```bash
npm run watch
```

### Debug Extension
1. Press `F5` in VS Code (opens Extension Development Host)
2. Set breakpoints in extension code
3. Reload window to apply changes

### Testing
```bash
npm test
```

## Roadmap

### Phase 6.5 Current (✅ Complete)
- ✅ Basic script execution
- ✅ Breakpoint management
- ✅ Variable inspection
- ✅ Register display
- ✅ Memory viewer
- ✅ Syntax highlighting
- ✅ Debugger adapter skeleton

### Phase 6.5+ Future
- 🔄 Debug console for Lua expressions
- 🔄 Watch expressions panel
- 🔄 Call stack display
- 🔄 Local variables panel
- 🔄 Hover tooltips with values
- 🔄 Integrated test runner
- 🔄 Performance profiler visualization

## API Reference

### mmemu Functions Available in Scripts

```lua
-- Memory operations
mmemu.read_byte(addr)
mmemu.write_byte(addr, value)

-- Register access
mmemu.get_register("A")  -- A, X, Y, SP, P
mmemu.set_register("A", value)
mmemu.get_pc()

-- Logging
mmemu.log(message)
mmemu.hex(value)

-- Snapshots
mmemu.save_snapshot("label")
mmemu.load_snapshot(id)
mmemu.list_snapshots()

-- Event hooks
mmemu.on_cycle(interval, function_name)
mmemu.on_interrupt(type, function_name)
```

### stdlib Module

```lua
local stdlib = require("stdlib")

stdlib.hex(0xFF)              -- "$FF"
stdlib.binary(0xAA)           -- "10101010"
stdlib.format_flags(0x81)     -- "Nv-bdizc"
stdlib.assert_hex_eq(a, b)
stdlib.bitfield(value, start, length)
stdlib.rotate_left(value, amount)
stdlib.popcount(value)
```

### device_io Module

```lua
local device_io = require("device_io")

device_io.SID_set_frequency(backend, channel, freq)
device_io.SID_set_envelope(backend, ch, a, d, s, r)
device_io.VIC_set_sprite_pos(backend, sprite, x, y)
device_io.CIA_set_timer_a(backend, cia, value, start)
device_io.DMA_copy(backend, src, dst, count)
```

## Contributing

Contributions welcome! Please submit PRs to the main mmsim repository.

## License

MIT — See LICENSE file in mmsim repository

## Support

- **Issues**: Report bugs at https://github.com/yourusername/mmsim/issues
- **Docs**: Full documentation at `examples/lua/PHASE_6_ADVANCED.md`
- **Chat**: Join mmemu community

---

**Made for mmemu - Multi-Machine Emulator**
