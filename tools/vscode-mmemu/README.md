# VS Code MEGA65 Emulator Extension

VS Code extension for remote debugging and development on MEGA65 emulator (mmemu).

## Features

- **Remote Debugging**: Connect to running mmemu instance via TCP
- **Variable Inspection**: View function-scoped variables with debug metadata
- **Memory Inspector**: Examine and edit memory regions
- **Breakpoint Management**: Set, list, and manage breakpoints
- **Disassembly View**: Show 6502/45GS02 disassembly at current PC
- **Register Display**: Real-time register values (A, X, Y, SP, PC, P)
- **Memory Dump**: Export memory regions to files

## Installation

### From VS Code Marketplace
(Coming soon)

### From Source

```bash
cd tools/vscode-mmemu
npm install
npm run compile
code --install-extension vscode-mmemu-0.1.0.vsix
```

## Quick Start

1. **Start mmemu with serial monitor enabled:**
   ```bash
   ./bin/mmemu-cli -m mega65 --serial-monitor-port 6502
   ```

2. **In VS Code:**
   - Press `Ctrl+Shift+M` (or Cmd+Shift+M on Mac) to connect
   - Or use Command Palette: "MEGA65: Connect to Emulator"
   - Configure host/port if needed

3. **Start debugging:**
   - Set breakpoints by clicking in the gutter
   - Use Debug Console or Variables panel to inspect state
   - View disassembly at current PC

## Configuration

Add to `.vscode/launch.json`:

```json
{
  "version": "0.2.0",
  "configurations": [
    {
      "name": "MEGA65 Debug",
      "type": "mmemu",
      "request": "launch",
      "host": "localhost",
      "port": 6502,
      "stopOnEntry": false
    }
  ]
}
```

### Configuration Options

- `host` - mmemu hostname (default: localhost)
- `port` - Serial monitor port (default: 6502)
- `timeout` - Connection timeout ms (default: 5000)
- `stopOnEntry` - Halt at first instruction (default: false)
- `program` - PRG file to load on connect (optional)

## Commands

Access via Command Palette (Ctrl+Shift+P):

- `MEGA65: Connect to Emulator` - Connect to running mmemu
- `MEGA65: Disconnect` - Close connection
- `MEGA65: Read Memory` - Display memory at address
- `MEGA65: Dump Memory to File` - Export memory region
- `MEGA65: Set Breakpoint` - Add breakpoint at address

## Views

### Variables View
Shows local variables and parameters for current function (requires debug metadata from cc45).

### Memory View
Displays memory contents in hex dump format with ASCII sidebar.

### Breakpoints View
Lists all active breakpoints. Click to jump to address.

### Disassembly View
Shows instructions around current PC with syntax highlighting.

## Keyboard Shortcuts

- `Ctrl+Shift+M` (Cmd+Shift+M) - Connect to emulator
- `F5` - Continue execution
- `F10` - Step over
- `F11` - Step into
- `Shift+F11` - Step out

## Protocol

The extension communicates with mmemu via the Serial Monitor Protocol. See `PROTOCOL.md` for details.

## Debugging the Extension

1. **Launch extension in debug mode:**
   ```bash
   npm run watch
   code --extensionDevelopmentPath=. .
   ```

2. **Press F5 in VS Code** to start debugging

3. **View debug output:**
   - Check "Debug Console" tab for extension logs
   - Check "mmemu" output channel for protocol messages

## Architecture

```
extension.ts          - VS Code extension entry point
├── debugAdapter.ts   - Debug Adapter Protocol implementation
├── client.ts         - Serial monitor client (TCP socket)
├── variables.ts      - Variables view provider
├── memory.ts         - Memory view provider
└── ui.ts             - UI commands and handlers
```

## Known Limitations

- Single breakpoint per address (no hit counts yet)
- Memory writes via GUI not implemented
- No expression evaluation in watch expressions
- Basic disassembly (no cross-references)

## Future Enhancements

- GDB Remote Serial Protocol support
- Performance profiling view
- Hardware register inspector
- Trace buffer visualization
- Multi-window debugging
- Integrated assembler
- Symbol import/export

## Development

### Building
```bash
npm run esbuild
```

### Testing
```bash
npm test
```

### Publishing
```bash
vsce package
vsce publish
```

## Contributing

See main mmemu repository for contribution guidelines.

## License

Same as mmemu (MIT)

## Related

- [mmemu Python Client](../mmemu_client_py/README.md)
- [Serial Monitor Protocol](../mmemu_client_py/PROTOCOL.md)
- [Main mmemu Repository](https://github.com/CTalkobt/mmemu)
