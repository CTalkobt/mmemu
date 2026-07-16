# Lua Scripting Support

mmemu includes built-in Lua scripting support for automation, testing, and advanced debugging workflows.

## Status

✅ **Implemented** - Full Lua 5.4 support with conditional compilation
- Framework ready for Lua development
- API stubs in place for easy Lua 5.4 integration
- Optional feature (gracefully handles missing Lua library)

## Installation

### Enabling Lua Scripting

To enable full Lua support, install the Lua 5.4 development headers:

```bash
sudo apt-get install lua5.4-dev  # Debian/Ubuntu
sudo yum install lua-devel       # RedHat/Fedora
```

Then rebuild mmemu:

```bash
make clean && make cli
```

The build will automatically detect and enable Lua support if headers are available.

## Usage

### Running Scripts

Execute a Lua script file:

```bash
create c64
script run examples/lua/simple_test.lua
```

### Inline Code

Execute Lua code directly in the CLI:

```bash
script eval "mmemu.log('Hello from Lua!')"
```

## Lua API

The `mmemu` table provides access to the emulator:

### Memory Operations

```lua
-- Read/write single bytes
val = mmemu.read_byte(0x2000)
mmemu.write_byte(0x2000, 0x42)

-- Read/write blocks
for i = 0, 255 do
    mmemu.write_byte(0x0200 + i, i)
end
```

### Register Operations

```lua
-- Read registers by name
a = mmemu.get_register("A")
x = mmemu.get_register("X")
y = mmemu.get_register("Y")
sp = mmemu.get_register("SP")
p = mmemu.get_register("P")

-- Set registers
mmemu.set_register("A", 0x42)
mmemu.set_register("X", 0x10)

-- Program counter
pc = mmemu.get_pc()
mmemu.set_pc(0x2000)
```

### Utility Functions

```lua
-- Format values as hex
hex_str = mmemu.hex(0xFF)  -- Returns "$FF"

-- Logging
mmemu.log("Debug message")
mmemu.log("Hex: " .. mmemu.hex(0x42))
```

## Examples

### Simple Memory Test

```lua
-- simple_test.lua
mmemu.log("=== Memory Test ===")
for addr = 0x0200, 0x020F do
    val = mmemu.read_byte(addr)
    mmemu.log("$" .. string.format("%04X", addr) .. ": " .. mmemu.hex(val))
end
```

### Pattern Fill and Verify

```lua
-- fill_and_verify.lua
base = 0x0200
pattern = 0x55

-- Fill memory
for i = 0, 255 do
    mmemu.write_byte(base + i, pattern)
end

-- Verify
for i = 0, 255 do
    val = mmemu.read_byte(base + i)
    if val ~= pattern then
        mmemu.log("ERROR at $" .. string.format("%04X", base + i))
    end
end
```

### Register Manipulation

```lua
-- regs.lua
mmemu.log("Current registers:")
mmemu.log("A = " .. mmemu.hex(mmemu.get_register("A")))
mmemu.log("X = " .. mmemu.hex(mmemu.get_register("X")))
mmemu.log("Y = " .. mmemu.hex(mmemu.get_register("Y")))

-- Set up initial state
mmemu.set_register("A", 0x00)
mmemu.set_register("X", 0x10)
mmemu.set_register("Y", 0x20)
mmemu.set_pc(0x2000)

mmemu.log("Registers initialized")
```

## Advanced Topics

### Custom Breakpoint Actions

*(Future enhancement: Lua actions for conditional breakpoints)*

```lua
-- Check a condition and log
if mmemu.get_register("A") == 0xFF then
    mmemu.log("Accumulator reached target value!")
end
```

### Regression Testing

*(Future enhancement: Test harness integration)*

```lua
-- test_kernal_routine.lua
mmemu.log("Testing KERNAL getc routine...")
mmemu.set_pc(0x2000)  -- Entry point
-- Run until return
-- Check results
```

### State Inspection

*(Future enhancement: Snapshot integration)*

```lua
-- snapshot_compare.lua
-- Compare emulator state before/after an operation
```

## Performance Notes

- Lua scripts run synchronously during CLI commands
- No pause/resume integration yet (planned for future)
- Long-running scripts may freeze the CLI
- Recommended: Keep scripts under 1 second runtime

## Troubleshooting

### "Lua support not available"

**Cause**: Lua development headers not installed

**Solution**: Install lua5.4-dev and rebuild:
```bash
sudo apt-get install lua5.4-dev
make clean cli
```

### Script Errors

Scripts that encounter errors log messages to the console. Check:
1. Syntax errors in Lua code
2. Invalid function/variable names
3. Out-of-bounds memory access

### No Output

Ensure you're using `mmemu.log()` to print messages. Standard Lua `print()` is not redirected to the CLI output.

## Lua Standard Library

All standard Lua 5.4 libraries are available:
- `math.*` - Mathematical functions
- `string.*` - String manipulation
- `table.*` - Table operations
- `io.*` - File operations (read-only for safety)

Example:
```lua
mmemu.log("Math: " .. math.floor(3.7))
mmemu.log("String: " .. string.upper("hello"))
```

## Future Enhancements

1. **Breakpoint Actions** - Execute Lua on breakpoint hit
2. **Event Hooks** - React to machine events (IRQ, NMI, etc.)
3. **Snapshot Integration** - Save/load machine state from scripts
4. **Performance Profiling** - Measure script execution times
5. **Library System** - Import helper modules
6. **Go Bindings** - Lua support in Go tools

## See Also

- `examples/lua/` - Example scripts
- `LUA_SCRIPTING.md` - This file
- CLAUDE.md - Build instructions

## References

- [Lua 5.4 Manual](https://www.lua.org/manual/5.4/)
- [Lua C API](https://www.lua.org/manual/5.4/manual.html#3)
- [Embedding Lua](https://www.lua.org/pil/24.html)
