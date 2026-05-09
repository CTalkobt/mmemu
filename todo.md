# Completed Items

## UI / Debugging Features
[x] Pop-up Modal Calculator
  [x] - Base conversion
  [x] - Basic Calculator Operations
    ( + - * / ^(power) square root, x^2, nth root, x^n, 
      sin, cos, sec, csc, tan, atn
      arc sin, arc cos.... 
      negate, ... 
    )
  [x] - 1's complement, 2's compliment
  [x] - Float / Integer mode. 
  [x] - Memory mode (8 results) 
  [x] - Ans (aka Rcl 0) button - Recall prior result. 
  [x] - Rcl n button to recall nth prior result. 
  [x] - Set precision display for int or floating point.

## Toolchain Migration
[x] Migrate 45GS02 test suite from KickAssembler (.asm) to ca45 (.s format)
  [x] - Remove legacy KickAssembler .asm files
  [x] - Convert test programs to .s format
  [x] - Update Makefile test targets
  [x] - Update validate.py for ca45 assembler
  [x] - Update documentation with ca45 project link

## MEGA65 Emulation
[x] Phase 20.2: F018B DMA Controller (basic implementation)
  [x] - Register file ($D700-$D70F) with shadow array
  [x] - Job list parsing and execution
  [x] - Copy, Fill, Swap operations
  [x] - 28-bit address support with bank register
  [x] - CPU halt during DMA execution
  [x] - Plugin creation and machine wiring
  [x] - Fractional stepping and modulus modes
  [d] - Mix operation implementation
  [d] - Interrupt on completion (bit 3)

## Phase 19: SparseMemoryBus(28) and MAP MMU ✓ COMPLETE
[x] - SparseMemoryBus with 4KB lazy-allocated pages and pre-mapped ROM regions
[x] - MapMmu as pure address translator (MAP instruction support)
[x] - Snapshot serialization (sparse format)
[x] - Unit tests for both components
    - `test_sparse_memory.cpp`: Validates lazy allocation, ROM overlays, snapshots
    - `test_map_mmu.cpp`: Validates memory mapping and translation

## MEGA65 Machine Integration (Phase 21) - IN PROGRESS
[x] - Wire MapMmu as CPU's bus pointer (CPU reads/writes via MapMmu → SparseMemoryBus)
[x] - Wire MapMmu pointer to 45GS02 CPU for MAP instruction support
[ ] - Implement MAP instruction (0x5C) in 45GS02 CPU to call MapMmu.setMapState()
[ ] - Separate C64 mode path (use C64BankController) from MEGA65 mode path (use MapMmu)
[ ] - Load MEGA65 ROM and wire overlays on SparseMemoryBus (KERNAL, BASIC, CHARROM)
[ ] - Implement I/O personality switching ($D02F) in machine factory
[ ] - Implement MAP instruction parameter parsing (read map spec from memory)
[ ] - MEGA65 integration tests (MAP functionality, address translation, ROM visibility)

## MCP Server Expansion (0.8.0) - COMPLETED
[x] - Assembler Support: `asm` tool for 6502/45GS02 code generation with error diagnostics
[x] - Search Navigation: `search_next`, `search_prior` for multi-match memory searches
[x] - MEGA65-Specific Features: MAP state control and I/O personality switching
[x] - Trace Buffer Integration: `get_trace_buffer`, `clear_trace`, `set_trace_filter`
[x] - Better Error Messages: Diagnostic feedback for address expressions and assembler errors
[x] - MCP Tool Count: 40 → 51 tools (27.5% expansion)

## Assembler Support & Selection
[x] MCP assembler tool (`.asm` tool in MCP server)
  [x] - Line-by-line assembly via IAssembler backend
  [x] - Support for both 6502 and 45GS02 ISAs
  [x] - Error diagnostics with syntax hints
  [x] - Optional memory loading of assembled bytes
[ ] Add ca45 assembler backend support for CLI and GUI (similar to KickAssembler)
  [ ] - Implement `Ca45AssemblerBackend` in libtoolchain
  [ ] - Support ca45 `.s` file format and `.prg` output
  [ ] - Parse ca45 symbol output (if available)
  [ ] - Register ca45 backend with toolchain registry
  [ ] - Add assembler selection mechanism (config, command-line, or file extension detection)
  [ ] - Update CLI `asm` command to support assembler selection
  [ ] - Update GUI assemble dialog with assembler picker
  [ ] - Document assembler backend architecture and how to add new assemblers
