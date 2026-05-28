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
[x] Phase 20.1: VIC-IV Video Controller
  [x] - Personality unlock ($D02F KEY)
  [x] - Extended registers ($D040-$D07F)
  [x] - 256-color palette ($D100-$D3FF)
  [x] - 32 KB internal color RAM
  [x] - C64 mode compatibility (inherited from VIC2)
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
[x] Phase 20.3: Math Acceleration Registers
  [x] - Hardware multiply/divide (single-cycle)
  [x] - Hardware RNG (LFSR)
[x] Phase 20.4: Dual SID (Stereo)
  [x] - Independent SID dispatching
  [x] - Stereo mixing with panning
[x] Phase 22: MEGA65 Keyboard and Joysticks
  [x] - C65 keyboard matrix (8x8)
  [x] - CIA1/CIA2 wiring
  [x] - Shared signal lines (IRQ/NMI)
  [x] - Combined port devices (Kbd + Joy2)

## Phase 19: SparseMemoryBus(28) and MAP MMU ✓ COMPLETE
[x] - SparseMemoryBus with 4KB lazy-allocated pages and pre-mapped ROM regions
[x] - MapMmu as pure address translator (MAP instruction support)
[x] - Snapshot serialization (sparse format)
[x] - Unit tests for both components

## MEGA65 Machine Integration (Phase 21) - IN PROGRESS
[x] - Wire MapMmu as CPU's bus pointer (CPU reads/writes via MapMmu → SparseMemoryBus)
[x] - Wire MapMmu pointer to 45GS02 CPU for MAP instruction support
[x] - Implement MAP instruction (0x5C) with correct 8×8KB block encoding
[x] - Separate C64 mode path (C64BankController) from MEGA65 mode path (MapMmu)
[x] - Implement I/O personality switching ($D02F KEY register) in machine factory
[x] - MAP instruction parameter parsing (register encoding from A/X/Y/Z)
- [x] - Load MEGA65 ROM and wire overlays on SparseMemoryBus (KERNAL, BASIC, CHARROM)
- [x] - MEGA65 integration tests (MAP functionality, address translation, ROM visibility)


## 45GS02 CPU Core
[x] - Quad immediate modes (LDQ/ADCQ/SBCQ/CMPQ/ANDQ/ORAQ/EORQ #imm32)
[x] - Decimal mode (BCD) accuracy for ADC/SBC
[x] - Full disassembly support (all opcodes, quad prefix, 32-bit indirect prefix)
[x] - 28-bit symbol table display (adaptive address width in CLI and GUI)
[x] - 32-bit indirect pointers ($EA prefix for [$zp],Z)
[x] - Stack-relative indirect addressing (d,S),Y
[x] - 8-bit / 16-bit stack mode switching (CLE/SEE)
[x] - B-register offset for stack operations
[x] - Interrupt masking / vector redirection via MMU

## MCP Server Expansion (0.8.1) - COMPLETED
[x] - Assembler Support: `asm` tool for 6502/45GS02 code generation with error diagnostics
[x] - Search Navigation: `search_next`, `search_prior` for multi-match memory searches
[x] - MEGA65-Specific Features: MAP state control and I/O personality switching
[x] - Trace Buffer Integration: `get_trace_buffer`, `clear_trace`, `set_trace_filter`
[x] - Better Error Messages: Diagnostic feedback for address expressions and assembler errors
[x] - MCP Tool Count: 40 → 51 tools (27.5% expansion)

## Assembler Support & Selection
[x] MCP assembler tool (`.asm` tool in MCP server)
[x] Pluggable Assembler Infrastructure (0.8.1)
[x] CA45 Assembler Backend
[x] MCP Server Assembler Tools (0.8.1)
[x] Add ca45 assembler backend support for CLI and GUI (phase 2)
  [x] - Update CLI `asm` command to support assembler selection and file-based mode
  [x] - Update GUI assemble dialog with assembler picker
  [x] - Add ca45 backend configuration to example config.json
  [x] - Document assembler backend architecture and how to add new assemblers

## CBM Disk Image Formats (0.3.1) ✓ COMPLETE
[x] Refactor D64Parser into geometry-driven CbmSectorDisk base class
[x] D71 (1571 double-sided) parser
[x] D80 (8050) parser
[x] D81 (1581) parser — used by MEGA65
[x] D82 (8250 double-sided) parser
[x] DiskImageLoader dispatch for all new extensions
[x] Fix D64 getDiskName (wrong sector), getFreeBlocks (BAM offset), remove debug cerr
[x] Unit tests for all formats (10 new tests)

## Deprioritized
- Cycle-accurate timing for all opcodes/modes
- I/O stalls and DMA contention simulation
