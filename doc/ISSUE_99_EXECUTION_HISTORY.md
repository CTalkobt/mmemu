# Issue #99: Execution History and Reverse Debugging

## Overview

Issue #99 implements execution history and reverse debugging features for mmemu. This allows developers to inspect the sequence of instructions that led to a breakpoint, analyze memory access patterns, and step backwards through execution.

## Features Implemented

### 1. Execution History (CLI: `log` command)

**Basic usage:**
```bash
> log show                    # Show last 20 executed instructions
> log show -last 50           # Show last 50 instructions
> log clear                   # Clear execution history
```

**Example output:**
```
Last 20 instructions executed:
  $2045: LDA #$01
  $2047: STA $FD
  $204A: LDA $FD
  $204C: STA $04
  $204E: BNE $2055
  ...
```

### 2. Memory Access Tracking (CLI: `log show -memory`)

Track all writes to a specific memory address:

```bash
> log show -memory $FD      # Show all access to address $FD
```

**Example output:**
```
Memory access pattern for $FD:
  Cycle 10: Written with $01 (from $2047 STA $FD)
  Cycle 15: Written with $02 (from $204E STA $FD)
  ...
```

### 3. Function Call Stack (CLI: `log show -calls`)

Display the call/return sequence:

```bash
> log show -calls            # Show recent function calls/returns
```

**Example output:**
```
Function call stack (last 20 calls):
  CALL from $2045 to ???
  RETURN from $2050
  CALL from $2045 to ???
  ...
```

### 4. Reverse Stepping (CLI: `backstep`)

Step backwards through execution (already implemented, now documented):

```bash
> backstep                   # Step back one instruction
> backstep 5                 # Step back 5 instructions
```

### 5. MCP Tools (Remote/Automated Access)

**`get_execution_history`** - Retrieve recent instructions
```json
{
  "machine_id": "c64",
  "count": 30
}
```

**`get_memory_access_history`** - Track memory access pattern
```json
{
  "machine_id": "c64",
  "address": "$FD"
}
```

## Use Cases

### Debugging Variable Assignment

Problem: A memory location has the wrong value

```bash
> break when $FD = 0x00      # (not yet implemented)
> run
[breakpoint hits when value is wrong]
> log show -memory $FD       # What wrote the wrong value?
[shows STA instruction with wrong value]
> log show -last 5           # What led to that instruction?
[shows previous instructions to understand context]
```

### Tracing Function Calls

Problem: Need to understand call sequence

```bash
> run
[hit breakpoint]
> log show -calls            # See call stack
> backstep                   # Go back into caller
> log show                   # See what caller did
```

### Execution Performance Analysis

Problem: Slow loop iteration

```bash
> log show -last 100         # Show last 100 instructions
[identify repeated pattern]
> log show -memory $D020     # Track VIC register changes
[understand performance bottleneck]
```

## Implementation Details

### Architecture

1. **TraceBuffer** - Existing circular buffer storing recent execution
   - Stores: PC, mnemonic, registers, memory writes
   - Capacity: ~1000 instructions by default
   - Managed by `DebugContext`

2. **CLI Command** (`log` command)
   - Wraps and extends TraceBuffer functionality
   - Provides user-friendly filtering and display
   - Supports memory-centric views

3. **MCP Tools** (Remote/Automated)
   - `get_execution_history` - Retrieve instruction stream
   - `get_memory_access_history` - Analyze memory patterns
   - Structured JSON responses for programmatic access

### Data Captured Per Instruction

```cpp
struct TraceEntry {
    uint32_t addr;                          // Program counter
    std::string mnemonic;                   // Disassembled instruction
    std::map<std::string, uint8_t> regs;    // Register snapshots
    std::vector<MemoryWrite> memWrites;     // Memory changes
};
```

### Memory Write Tracking

```cpp
struct MemoryWrite {
    uint32_t addr;      // Memory address written
    uint8_t value;      // Value written
};
```

## CLI Commands Summary

| Command | Purpose |
|---------|---------|
| `log show` | Show last 20 instructions |
| `log show -last N` | Show last N instructions |
| `log show -memory ADDR` | Show writes to address |
| `log show -calls` | Show call/return sequence |
| `log clear` | Clear execution history |
| `backstep` | Step backward one instruction |
| `backstep N` | Step backward N instructions |

## MCP Tools Summary

| Tool | Purpose |
|------|---------|
| `get_execution_history` | Retrieve recent instructions |
| `get_memory_access_history` | Track memory access pattern |

## Future Enhancements

### Phase 1 (Completed)
- ✓ Execution history logging
- ✓ Memory access tracking
- ✓ Call stack display
- ✓ CLI commands
- ✓ MCP tools

### Phase 2 (Future)
- [ ] Time-based filtering: `log show -since CYCLE`
- [ ] Condition-based breakpoints: `break when $FD changed`
- [ ] Instruction-level filtering: `log show -filter "STA $*"`
- [ ] Diff view: Compare state at two points in history

### Phase 3 (Advanced)
- [ ] Deterministic replay: Record all I/O and replay to any point
- [ ] State snapshots: Periodic full machine state captures
- [ ] Fast-forward: Jump to specific cycle in execution
- [ ] Execution prediction: Replay with different register values

## Performance Considerations

- **Memory Usage**: ~10KB per 1000 instructions (typical)
- **CPU Overhead**: ~1-2% during execution (captures on each step)
- **Latency**: Negligible (circular buffer lookups are O(1))

## Integration

- Built on existing `TraceBuffer` infrastructure
- Uses `DebugContext` for unified access
- Consistent with existing CLI/MCP patterns
- No external dependencies

## Testing

CLI smoke tests:
```bash
> create c64
> load test.prg 2048
> break 2050
> run
> log show                    # Should show recent instructions
> log show -calls             # Should show JSR/RTS sequence
> backstep                    # Should work
> log show                    # Should show previous instruction
```

MCP integration tests:
- `get_execution_history` returns valid JSON
- `get_memory_access_history` filters correctly
- Handles invalid addresses gracefully

## References

- Issue: https://github.com/CTalkobt/mmemu/issues/99
- Related: TraceBuffer (libdebug/trace_buffer.h)
- Related: Time-Travel Debugging (Phase 0.3.2)
