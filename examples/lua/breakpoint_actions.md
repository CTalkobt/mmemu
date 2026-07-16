# Lua Breakpoint Action Scripts

Lua scripting integrates with mmemu's breakpoint system to enable custom actions when breakpoints are hit.

## Concept

When a breakpoint triggers, execute Lua code in the context of the emulator's current state:
- Read/write registers
- Inspect memory
- Log conditional information  
- Modify execution flow
- Count hits and skip conditionally

## Example: Conditional Logging

```lua
-- Log when accumulator reaches a specific value
if mmemu.get_register("A") == 0x42 then
    mmemu.log("Accumulator reached target value 0x42!")
end
```

## Example: Loop Counter

```lua
-- Count how many times we hit this breakpoint
-- (Requires persistent state in future enhancement)
if not breakpoint_counter then
    breakpoint_counter = 0
end
breakpoint_counter = breakpoint_counter + 1
mmemu.log("Breakpoint hit #" .. breakpoint_counter)
```

## Example: Register Dump on Condition

```lua
-- Dump registers only on certain condition
if mmemu.get_pc() == 0x2050 then
    mmemu.log("PC=$" .. mmemu.hex(mmemu.get_pc()))
    mmemu.log("A=$" .. mmemu.hex(mmemu.get_register("A")))
    mmemu.log("X=$" .. mmemu.hex(mmemu.get_register("X")))
    mmemu.log("Y=$" .. mmemu.hex(mmemu.get_register("Y")))
end
```

## Example: Memory Inspection

```lua
-- Log memory contents when breakpoint hits
base = 0x2000
for i = 0, 15 do
    val = mmemu.read_byte(base + i)
    mmemu.log("$2000+" .. string.format("%02X", i) .. ": " .. mmemu.hex(val))
end
```

## Example: State Modification

```lua
-- Inject test values into registers
mmemu.set_register("A", 0xFF)
mmemu.set_register("X", 0x00)
mmemu.set_register("Y", 0x10)
mmemu.log("Registers injected for test")
```

## Integration (Future Enhancement)

### CLI Syntax (Planned)

```bash
# Set breakpoint with Lua action
break $2000 action "mmemu.log('Hit at ' .. mmemu.hex(mmemu.get_pc()))"

# More complex action
break $2000 action '''
  if mmemu.get_register("A") == 0 then
    mmemu.log("A is zero!")
  end
'''

# Conditional breakpoint with action
break when mmemu.get_register("A") == 0x42 action "mmemu.log('Found A=42')"
```

### MCP Integration (Planned)

```json
{
  "tool": "set_breakpoint",
  "params": {
    "address": "0x2000",
    "action": "mmemu.log('Breakpoint hit')"
  }
}
```

### GUI Integration (Planned)

- Breakpoint editor dialog with Lua code field
- Syntax highlighting for Lua
- Quick templates for common patterns
- Action execution log display

## Use Cases

1. **Regression Testing**
   ```lua
   -- Verify expected state at test point
   if mmemu.get_register("A") ~= expected_a then
       mmemu.log("TEST FAILED: A=" .. mmemu.hex(mmemu.get_register("A")))
   end
   ```

2. **Conditional Skip**
   ```lua
   -- Skip breakpoint if condition met
   if mmemu.get_register("X") < 10 then
       return false  -- Continue execution
   end
   ```

3. **State Injection**
   ```lua
   -- Set up next test case state
   mmemu.write_byte(0x2000, 0x01)
   mmemu.write_byte(0x2001, 0x02)
   mmemu.log("Test state injected")
   ```

4. **Profiling**
   ```lua
   -- Track execution paths
   pc = mmemu.get_pc()
   mmemu.log("Execution at $" .. string.format("%04X", pc))
   ```

## Performance Considerations

- Actions execute synchronously during breakpoint handling
- Keep Lua scripts short (<100ms execution time)
- Avoid nested breakpoints in actions
- Test performance impact on real hardware tests

## Limitations (Current)

- Persistent state between hits requires future enhancement
- No direct device control (VIC2, SID, CIA)
- No interrupt injection yet
- No snapshot save/load from actions yet

## Future Enhancements

1. **Persistent Breakpoint Context**
   - Store variables across multiple hits
   - Global breakpoint session state

2. **Advanced Control**
   - Inject interrupts (IRQ, NMI)
   - Modify return addresses on stack
   - Change execution flow

3. **Integration with Other Features**
   - Snapshot save from breakpoint
   - Trace to file from breakpoint
   - Automated test reporting

4. **IDE Support**
   - Breakpoint action templates
   - Remote action execution via MCP
   - VS Code extension integration

## See Also

- `LUA_SCRIPTING.md` - Main Lua documentation
- `examples/lua/simple_test.lua` - Basic script examples
- Breakpoint documentation in CLAUDE.md
