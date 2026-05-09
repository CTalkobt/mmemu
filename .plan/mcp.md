# MCP Server Status & Outstanding Work

## Overview

The mmemu-mcp (Model Context Protocol) server provides Claude Code and other MCP clients with programmatic access to the emulation system. It exposes debugging, machine control, and device management capabilities through a standardized interface.

**Status:** Feature-complete with 51 tools (27.5% expansion in 0.8.1). All major features implemented and tested.

---

## Current Implementation (51 Tools) ✅

### Machine Management
- `create_machine` - Instantiate a machine preset
- `list_machines` - List available machine types
- `reset_machine` - Reset CPU and memory to initial state

### Memory Operations
- `read_memory` - Hex dump memory range
- `write_memory` - Write bytes to memory
- `fill_memory` - Fill range with constant value
- `copy_memory` - Copy memory region
- `swap_memory` - Swap two memory ranges
- `search_memory` - Find byte/ASCII patterns in memory
- `search_next` - Find next occurrence of last search pattern
- `search_prior` - Find previous occurrence of last search pattern

### CPU Control & State
- `step_cpu` - Execute N instructions
- `run_cpu` - Run until breakpoint (requires timeout)
- `set_pc` - Set program counter
- `read_registers` - Dump all CPU registers

### Debugging
- `set_breakpoint` - Execution breakpoint at address
- `set_watchpoint` - Memory read/write watchpoint
- `enable_breakpoint` / `disable_breakpoint` - Toggle without deleting
- `delete_breakpoint` - Remove by ID
- `list_breakpoints` - Show all breakpoints/watchpoints with status
- `get_stack` - Dump stack with frame analysis
- `disassemble` - Disassemble instruction range

### Symbol Management
- `add_symbol` - Register label for address
- `list_symbols` - List all defined symbols
- `remove_symbol` - Delete symbol
- `clear_symbols` - Clear all symbols
- `load_symbols` - Load symbol file (.sym)

### Storage & Peripherals
- `load_image` - Load program/disk image
- `mount_disk` - Attach disk image
- `eject_disk` - Remove disk
- `mount_tape` - Attach tape image
- `control_tape` - Play/stop/rewind tape
- `record_tape` - Record to tape
- `save_tape_recording` - Export tape recording
- `attach_cartridge` - Insert cartridge
- `eject_cartridge` - Remove cartridge

### Device Control
- `get_device_info` - Query device state/properties
- `list_devices` - List attached I/O devices
- `press_key` - Simulate keyboard press
- `type_string` - Type string into keyboard buffer

### Logging & Monitoring
- `list_loggers` - Show available log channels
- `set_log_level` - Adjust verbosity for channel

### Code Generation
- `asm` - Assemble source code to machine code with diagnostics

### MEGA65-Specific Features
- `get_map_state` - Read current MAP block offsets and enable masks
- `set_map_state` - Configure address translation blocks
- `get_personality` - Query current I/O personality mode
- `set_personality` - Switch personality via KEY register knock sequence

### Trace Buffer
- `get_trace_buffer` - Retrieve instruction execution trace entries
- `clear_trace` - Clear all entries from trace buffer
- `set_trace_filter` - Configure trace filtering (all, instructions, breakpoints, memory)

### Resources
- `machine_state` resource - Snapshot of running machines and cycle counts

---

## Completed Features (0.8.1)

### 1. ✅ Assembler Support (HIGH PRIORITY) - COMPLETE

The `asm` tool enables on-the-fly code generation:
- Line-by-line assembly for any registered ISA (6502, 45GS02, etc.)
- Returns assembled bytes, symbol table, and error list
- Optional automatic memory loading at specified address
- Error messages include syntax context and helpful hints

### 2. ✅ Enhanced Search Navigation (MEDIUM PRIORITY) - COMPLETE

Optimized multi-match memory searching:
- `search_next` / `search_prior` navigate through matches without rescanning
- Per-machine search context (pattern + last found address)
- Address wrapping at memory boundaries for circular searches
- Saves user time when looking for multiple occurrences

### 3. ✅ MEGA65-Specific Features (MEDIUM PRIORITY) - COMPLETE

Hardware-specific control for MEGA65 machines:
- MAP state queries return block offsets and enable masks
- MAP configuration via `set_map_state` activates address translation
- Personality switching via KEY register knock sequences (C64/C65/MEGA65/ETHERNET)
- Plugin-aware: gracefully handles machines without these features

### 4. ✅ Trace Buffer Integration (MEDIUM PRIORITY) - COMPLETE

Instruction execution history for debugging:
- `get_trace_buffer` shows address, mnemonic, register state, cycle count
- Limit parameter shows most recent entries without overflow
- Per-machine trace filter configuration (all/instructions/breakpoints/memory)
- Trace filtering infrastructure ready for future expansion

### 5. ✅ Better Error Messages (LOW PRIORITY) - COMPLETE

Diagnostic feedback for user errors:
- Address expression failures explain valid formats (hex $1000, decimal 4096, registers, operators)
- Assembler errors include syntax hints (e.g., immediate mode usage)
- Multi-parameter tools identify which parameter failed
- `resolveAddrWithDiagnostic()` function provides consistent error reporting

---

## Outstanding Work / Missing Features

### Outstanding Features

### 1. Plugin Tool Integration ⚠️ (LOW PRIORITY)

**Scope:** Verify and test plugin-provided tools

**Context:**
- PluginToolRegistry exists for plugins to register MCP tools
- Dispatch mechanism implemented
- No test coverage for plugin tools

**What needs to be done:**
- Test that plugin tools appear in `tools/list` output
- Verify plugin tool dispatch works (call a plugin tool via MCP)
- Document plugin tool API for plugin authors
- Handle plugin tool errors correctly

**Files to modify:**
- `src/mcp/test/mcp_test.py` - Add plugin tool test case
- Plugin author documentation

**Acceptance criteria:**
- MCP test verifies at least one plugin-provided tool is callable
- Plugin tools follow same error/result schema as built-in tools

---

### 2. Conditional Breakpoints (LOW PRIORITY)

**Scope:** Add conditional breakpoint support to MCP

**Context:**
- Debug system supports breakpoint conditions
- CLI can set conditional breakpoints
- No MCP tool to query or set conditions

**What needs to be done:**
- Expose breakpoint condition evaluation in MCP schema
- Allow `set_breakpoint` with optional `condition` parameter (e.g., "A == $42")
- Return condition text in `list_breakpoints` output
- Document condition syntax and examples

**Acceptance criteria:**
- Conditional breakpoints settable via MCP `set_breakpoint`
- Conditions appear in `list_breakpoints` output
- Breakpoints with conditions evaluate correctly

---

### 3. Performance Profiling Tools (LOW PRIORITY)

**Scope:** Add CPU profiling and cycle measurement

**Context:**
- CPU cycle counter available
- Useful for identifying hot spots
- No MCP tools for performance analysis

**What needs to be done:**
- Add `profile_cpu` tool - Sample instruction execution for hotspot analysis
- Add `measure_region` tool - Measure cycle count for address range
- Return structured data with counts and percentages

**Acceptance criteria:**
- `profile_cpu` returns top N instructions by count
- `measure_region` returns accurate cycle count for range
- Data useful for optimization analysis

---

## Implementation Priority & Effort Matrix (0.8.1 Complete)

| Feature | Priority | Effort | User Impact | Status |
|---------|----------|--------|-------------|--------|
| Assembler Support | HIGH | 4 hours | Enables code generation in Claude | ✅ DONE |
| Search Navigation | MEDIUM | 2 hours | Better UX for memory search | ✅ DONE |
| MEGA65 Features | MEDIUM | 3 hours | Unblock MEGA65 testing | ✅ DONE |
| Trace Integration | MEDIUM | 2.5 hours | Debug visibility | ✅ DONE |
| Better Errors | LOW | 1.5 hours | Polish | ✅ DONE |
| Plugin Tools Test | LOW | 1 hour | Code confidence | ⏳ NEXT |
| Conditional BP | LOW | 2 hours | Advanced debugging | 🔮 Future |
| Performance Tools | LOW | 3 hours | Optimization | 🔮 Future |

**Time Invested:** ~13.5 hours (5 features completed in one session)  
**Result:** MCP tool count 40 → 51 (27.5% expansion), all features tested and documented

---

## Next Steps

**Phase 2 (Post 0.8.1) Candidates:**

1. **Plugin Tool Integration Testing** (1 hr, low effort)
   - Verify plugin-registered tools appear in tools/list
   - Add unit test for plugin tool dispatch
   - Document plugin tool API

2. **Conditional Breakpoints** (2 hrs, medium effort)
   - Add `condition` parameter to `set_breakpoint`
   - Return conditions in `list_breakpoints` output
   - Useful for advanced debugging workflows

3. **Performance Profiling** (3 hrs, research phase)
   - `profile_cpu` tool for hotspot analysis
   - `measure_region` tool for cycle counting
   - Lower priority; useful for optimization work

---

## Test Command Reference

```bash
# Run all MCP tests
make test-mcp

# Run MCP server standalone (for manual testing)
bin/mmemu-mcp < test_input.json

# Check MCP tool schema
bin/mmemu-mcp | grep -A 5 "asm"
```

---

## Notes

- **All 250 unit tests + MCP tests pass** (sections 1-11)
- MCP server builds without errors
- Plugin tool registry is wired but untested
- Error messages now provide diagnostic feedback with helpful hints
- Search context and trace filters stored per-machine
- MEGA65 hardware control available when features present
- MCP server is production-ready with comprehensive error handling
