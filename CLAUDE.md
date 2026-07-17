# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

**mmsim** (Multi-Machine Simulator) is a universal 8/16-bit emulation platform with a modular, plugin-based architecture. It supports Commodore computers (C64, VIC-20, PET) and MEGA65. The system is organized into core libraries, plugins, and three independent frontends (CLI, GUI, MCP).

---

## Build System & Common Commands

### Build Targets

- **`make all`** — Build everything: CLI, GUI, MCP binaries, and all plugins.
- **`make cli`** — Build the command-line interface (`bin/mmemu-cli`).
- **`make gui`** — Build the graphical debugger (`bin/mmemu-gui`), requires wxWidgets.
- **`make mcp`** — Build the MCP server (`bin/mmemu-mcp`).
- **`make plugins`** — Build all `.so` plugin modules in `lib/`.
- **`make libs`** — Build static archive libraries (for internal development use).
- **`make test`** — Build and run the complete test suite (~517 tests) plus extended test suites (MCP, GDB).
- **`make test-mega65`** — Run MEGA65 45GS02 cross-validation tests (requires `ca45` assembler).
- **`make test-mcp`** — Run MCP Python integration tests.
- **`make test-gdb`** — Run GDB RSP protocol integration tests.
- **`make man`** — Generate man pages from Markdown documentation (requires `pandoc`).
- **`make clean`** — Remove all build artifacts.
- **`make serve`** — Start the MCP server with configuration examples.

### Parallel Build Performance

**IMPORTANT:** Always use `make -j 12` for faster builds and test execution. The Makefile is highly parallelizable:

```bash
make -j 12 all                 # Build everything in parallel (~4-5x faster)
make -j 12 test                # Run full test suite in parallel
make -j 12 plugins             # Build all plugins concurrently
make -j 12 cli gui mcp         # Build all frontends in parallel
```

Parallel builds complete in **~30-60 seconds** vs. **2-3 minutes** serially. This is essential for iterative development and CI/CD. The `-j 12` flag uses up to 12 concurrent jobs; adjust based on your CPU core count (typically `nproc / 2` is optimal to avoid system overload).

### Running Individual Tests

The test suite is compiled into a single binary (`bin/mmemu-test`). You can filter tests:

```bash
make test                              # Run all tests
bin/mmemu-test "*vic20*"               # Run VIC-20 tests
bin/mmemu-test "*cpu6502*"             # Run 6502 CPU tests
bin/mmemu-test "*sparse*"              # Run SparseMemoryBus tests
./tests/45gs02/validate.py tests/45gs02/arithmetic.s  # Run single 45GS02 validation
```

Test source files are colocated with their modules: `src/libmem/test/test_flatmembus.cpp`, `src/plugins/6502/test/test_cpu6502.cpp`, etc.

---

## Architecture Overview

### Core Libraries (Static Archives in `lib/internal/`)

| Library | Purpose |
|---------|---------|
| **libmem** | Abstract address bus (`IBus`), `FlatMemoryBus`, `SparseMemoryBus` (28-bit for MEGA65) |
| **libcore** | CPU core interface (`ICore`), machine configuration, machine registry |
| **libdevices** | I/O handler infrastructure (`IOHandler`, `IORegistry`), device interfaces (`IPortDevice`, `ISignalLine`) |
| **libtoolchain** | Symbol table, disassembly, assembly, expression evaluator |
| **libdebug** | Breakpoints, watchpoints, trace buffer, debug context, stack tracing |
| **libplugins** | Plugin loader (`dlopen`/`dlsym`), logging infrastructure |

### Plugin System

All CPU cores, devices, and machine presets are **dynamically loaded** `.so` modules at startup. The plugin loader:
1. Scans `./lib/` for `.so` files
2. Calls the `mmemuPluginInit()` entry point
3. Registers cores, devices, disassemblers, assemblers, and machine factories

Key plugins:
- **mmemu-plugin-6502.so** — MOS 6502, 6510 CPU cores; 6502 disassembler/assembler
- **mmemu-plugin-45gs02.so** — MEGA65 45GS02 CPU core (with hypervisor mode)
- **mmemu-plugin-vic20.so**, **mmemu-plugin-c64.so**, **mmemu-plugin-pet.so** — Machine presets
- **mmemu-plugin-vic2.so**, **mmemu-plugin-sid6581.so** — Commodore devices
- **mmemu-plugin-map-mmu.so**, **mmemu-plugin-f018b-dma.so** — MEGA65 peripherals
- **mmemu-plugin-mega65.so** — MEGA65 machine preset (includes hypervisor registers, HYPPO ROM loading)
- **mmemu-plugin-cbm-loader.so** — `.prg`, `.crt`, `.d64`, `.d71`, `.d80`, `.d81`, `.d82`, `.g64`, `.t64` file loaders
- **mmemu-plugin-datasette.so** — Tape (`.tap`) support

---

## Directory Structure

```
mmsim/
├── src/                           # All source code
│   ├── libmem/main/               # Memory bus abstractions
│   │   └── sparse_memory_bus.cpp  # SparseMemoryBus (28-bit MEGA65)
│   ├── libcore/main/              # Machine descriptor, CPU interface
│   │   └── json_machine_loader.cpp
│   ├── libdevices/main/           # IOHandler, IORegistry
│   ├── libtoolchain/main/         # Disassembler, assembler, symbols
│   ├── libdebug/main/             # Breakpoints, expressions, trace
│   ├── cli/main/                  # CLI target
│   ├── gui/main/                  # GUI target (wxWidgets)
│   │   └── dialogs/               # Modal dialogs (Assembly, Calculator, etc.)
│   ├── mcp/main/                  # MCP server target
│   ├── plugins/
│   │   ├── 6502/main/             # 6502/6510 CPU cores
│   │   ├── 45gs02/main/           # 45GS02 CPU core
│   │   ├── devices/               # Individual I/O devices
│   │   │   ├── vic2/              # VIC-II chip (C64)
│   │   │   ├── sid6581/           # SID chip (C64)
│   │   │   ├── f018b_dma/         # DMA controller (MEGA65)
│   │   │   ├── map_mmu/           # Memory map MMU (MEGA65)
│   │   │   ├── hypervisor/        # Hypervisor virtualisation registers (MEGA65)
│   │   │   └── ... (others)
│   │   └── machines/              # Machine presets
│   │       ├── vic20/
│   │       ├── c64/
│   │       ├── pet/
│   │       └── mega65/
│   ├── include/                   # Common headers (mmemu_plugin_api.h, etc.)
│   └── plugin_loader/main/        # Plugin discovery and loading
├── tests/                         # Integration tests, test utilities
│   └── 45gs02/                    # 45GS02 cross-validation suite
├── doc/                           # Device documentation (README-*.md)
├── roms/                          # ROM images (.bin files) organized by machine
├── lib/                           # Built plugins (.so files)
├── bin/                           # Built binaries
├── CHANGELOG.md                   # Release notes (keep up-to-date with recent work)
├── STYLEGUIDE.md                  # C++ naming, formatting conventions
└── Makefile                       # Build automation
```

### Test Organization

Unit tests are colocated with modules: `src/libmem/test/`, `src/plugins/6502/test/`, etc. Integration tests are in `tests/`. The test binary links all plugin object files (but not plugin entry points) to allow direct unit testing.

---

## Key Architectural Patterns

### Data-Driven Machine Composition

Machine presets (e.g., `c64.json`) describe CPUs, buses, devices, and ROM overlays declaratively. The `JsonMachineLoader` assembles them at runtime via the `MachineRegistry`.

**Example**: C64 includes a 6510 CPU, 6567/6569 VIC-II, 6581 SID, two 6526 CIAs, C64 PLA, and BASIC/KERNAL/Character ROMs.

### Expression Evaluator Integration

The `ExpressionEvaluator` (libdebug) parses complex address expressions with symbols, registers, and math. It's integrated everywhere: CLI, GUI dialogs, breakpoint conditions, and MCP tools.

### Breakpoint & Watchpoint System

Conditional breakpoints use the evaluator. Breakpoints support `EXEC`, `READ_WATCH`, and `WRITE_WATCH` modes. Conditions are evaluated after the CPU event to allow state inspection.

### Sparse Memory for MEGA65

The `SparseMemoryBus` manages the MEGA65's 28-bit address space with 4KB lazy-allocated pages. The `MapMmu` acts as a pure address translator, called by the CPU during reads/writes to map logical addresses to physical memory.

---

## Important Conventions

### Naming (from STYLEGUIDE.md)

- **Classes/Structs**: `PascalCase` (e.g., `MachineDescriptor`, `FlatMemoryBus`)
- **Functions/Methods**: `camelCase` (e.g., `readByte()`, `setObserver()`)
- **Variables/Parameters**: `camelCase` (e.g., `addrBits`, `m_isActive`)
- **Constants/Macros**: `UPPER_SNAKE_CASE` (e.g., `REGFLAG_INTERNAL`)
- **Files**: `snake_case` (e.g., `machine_desc.h`, `cpu_6502.cpp`)

### C++ Standards

- **Standard**: C++17 (`-std=c++17`)
- **Header Guards**: `#pragma once`
- **Integer Types**: Fixed-width types (`uint8_t`, `uint16_t`, `uint32_t`, `int32_t`)
- **Comments**: `//` for single-line; `/** ... */` for public interfaces
- **Visibility**: Classes and public methods should be documented unless they are obvious.

### Plugin ABI Stability

Plugin entry points must use C calling convention: `extern "C" SimPluginManifest* mmemuPluginInit(const SimPluginHostAPI* host)`. Plugins receive stable function pointers for registry operations; they do not link against `spdlog`/`fmt` directly.

---

## Debugging & Development Tips

### Running the Emulator

```bash
./bin/mmemu-cli -m c64              # Start CLI with C64
./bin/mmemu-cli -m vic20            # Start CLI with VIC-20
./bin/mmemu-cli -m c64 --gdb-port 1234  # Start CLI with GDB server
./bin/mmemu-gui -m c64              # Start GUI with C64

# CLI commands: step, regs, m (memory), disasm, break, watch, run, sym, etc.
```

### Adding a Test

1. Create `src/<module>/test/test_myfeature.cpp`
2. Include `tests/src/test_harness.h`
3. Define `TEST_CASE("name") { ... }`
4. Add the file to `TEST_SRCS` in `Makefile`
5. Run `make test` to build and run

### Modifying a Plugin

Plugins do NOT need to export dynamic registration — the host's `SimPluginHostAPI` provides stable callbacks. If you add a new device:
1. Implement the `IOHandler` interface
2. Create a `plugin_init.cpp` that calls `host->registerDevice()`
3. Add the plugin to `PLUGINS` in the Makefile

### Symbol Resolution

The evaluator resolves symbols at expression-parse time. Symbols are auto-loaded from ROM files (e.g., `roms/c64/kernal.sym`). You can inspect/manage symbols via the CLI `sym` command or GUI **Symbols** pane.

---

## Dependencies & Prerequisites

- **C++17 Compiler**: GCC 9+ or Clang 10+
- **Required Libraries**: `spdlog`, `fmt`, `nlohmann/json`, `wxWidgets 3.0+`
- **Audio**: `libasound2` (ALSA)
- **Tools**: `ca45` assembler for 45GS02 tests (from [github.com/CTalkobt/m65compiler](https://github.com/CTalkobt/m65compiler))
- **Optional**: `pandoc` for generating man pages

---

## Assembler Infrastructure

The simulator now features a pluggable assembler system with per-machine selection:

- **SimConfig** loads tool configuration from `config.json` (searched in `./`, `~/.local/share/mmsim/`, `/usr/local/share/mmsim/`)
- **ToolchainRegistry** supports both ISA-based defaults and named assembler factories
- **MachineDescriptor** includes optional `"assembler"` field (e.g., `"ca45"`, `"kickAssembler"`)
- **resolveAssembler()** follows 3-level precedence: runtime override → machine-preferred → ISA-default
- **CA45Assembler** plugin provides MEGA65 45GS02 assembly support
- **MCP Server** provides 58 tools including assembler selection, machine snapshots, binary diff, routine analysis, and test vector generation

### Adding a New Assembler

1. Implement `IAssembler` interface with `assembleLine()` (line-mode) or set return value to -1 to trigger file-based fallback
2. Create plugin entry point calling `host->registerAssemblerByName("name", factory)`
3. Add configuration to `config.json` under `tools.assemblers.<name>`
4. Optional: Set `"assembler": "name"` in machine JSON for default selection

---

## Current Development Focus

**Phase 21: MEGA65 Machine Integration** ✅ Complete
- Phase 19 ✓ Complete: `SparseMemoryBus` and `MapMmu` implemented with unit tests
- ✓ MAP instruction fixed (correct 8×8KB block encoding, per-block offsets)
- ✓ C64BankController for ROM banking in C64 compatibility mode
- ✓ KEY register ($D02F) wired for I/O personality switching
- ✓ 45GS02: quad immediate modes, decimal mode, full disassembly, 28-bit symbol display
- ✓ Hypervisor mode: enter/exit, HYPPO ROM loading, virtualisation control registers
- ✓ Hypervisor registers IOHandler ($D640-$D67F) with SYSCALL traps
- ✓ F018B DMA overlap-safe copy and `getDeviceInfo()`
- ✓ Audio DMA: 4 channels, loop mode, sine wave, volume control, IAudioOutput integration (19 tests)
- ✓ Line Drawing Enhancements: slope initialization, X/Y-major modes, texture scaling (9 tests)
- ✓ Inline DMA Lists: Enhanced DMA job options ($D705/$D706 triggers), option parsing (7 tests)
- ✓ MAP'd DMA testing: ETRIGMAPD ($D706) with 4 unit tests + 1 integration test (5 tests)
- ✓ MEGA65 integration tests: Full system MAP'd DMA validation

See `todo.md` for the full roadmap.

## Issue #81: DMA Enhancements — Implementation Status

### ✅ Audio DMA (Complete)
- 4 independent audio channels ($D720-$D75F)
- 24-bit frequency counter with overflow detection
- Loop mode with automatic sample restart
- Pure sine wave generation mode
- Per-channel volume control (0-255)
- IAudioOutput integration with stereo mixing
- 19 comprehensive unit tests
- MEGA65 machine integration
- See: `src/plugins/devices/audio_dma/`

### ✅ Line Drawing Enhancements (Complete)
- Slope accumulator initialization ($8D/$8E, $9D/$9E) for sub-pixel precision
- X-major and Y-major line mode selection
- Positive and negative slope support
- Card boundary crossing detection
- Texture scaling via fractional skip rates ($82-$85)
- 40.5 Mpixels/sec drawing speed
- 9 unit tests covering all features
- Full VHDL compliance verified
- See: `src/plugins/devices/f018b_dma/LINE_DRAWING.md`

### ✅ Inline DMA Lists (Complete)
- **$D705 (ETRIG)**: Enhanced DMA trigger with flat 28-bit address
- **$D706 (ETRIGMAPD)**: Enhanced DMA trigger with MAP'd address (stub; MAP translation future)
- **Option parsing**: Full support for options $00-$9F with arguments
- **Supported options**: MB selection ($80/$81), skip rates ($82-$85), transparency ($06/$07/$86), format ($0A/$0B), line drawing ($87-$8F/$97-$9F)
- **Inheritance**: Per-job options with MB settings persisting across chained jobs
- **7 comprehensive unit tests** covering ETRIG, option parsing, multiple option sequences
- **All 633 tests passing** (617 existing + 16 new inline DMA tests)
- **Full documentation**: See `src/plugins/devices/f018b_dma/INLINE_DMA_LISTS.md`

---

## Hardware Validation & Cross-Validation Testing

The emulator can be cross-validated against real MEGA65 hardware to verify emulation accuracy. The **Hardware Test Runner Bridge** provides a unified interface for running identical test programs on both targets.

### Components

- **HardwareTestBridge** — Unified interface for emulator (TCP to SerialMonitorServer) and hardware (serial port) communication
- **CrossValidationRunner** — Orchestrates test execution on one or both targets with automatic result comparison
- **Serial Monitor Protocol** — Text-based commands: M (read), S (write), R (registers), D (disasm), G (setPC), T (step)

### Quick Start

```cpp
// Cross-validate: run same test on both emulator and hardware
auto runner = CrossValidationRunner::withBoth(
    "127.0.0.1", 6502,              // Emulator
    "/dev/ttyUSB0", 2000000         // Hardware serial port
);

std::vector<CrossValidationRunner::TestCase> tests = {
    {.name = "arithmetic", .programPath = "tests/arithmetic.bin", 
     .programAddr = 0x0800, .resultAddr = 0x2000, .resultSize = 256}
};

auto results = runner->runTests(tests);
for (const auto& [name, result] : results) {
    if (result.overallPass()) {
        printf("✓ %s: PASS (emulator and hardware match)\n", name.c_str());
    } else if (!result.resultsMatch) {
        printf("✗ %s: Results differ\n", name.c_str());
    }
}
```

### Hardware Setup

- MEGA65 with serial monitor support
- USB-to-UART adapter (FTDI/CP2102)
- Connect to JTAG/serial connector pins 2-3 (RX/TX)

### See Also

- **HARDWARE_VALIDATION.md** — Complete API reference, examples, and debugging guide
- **tests/src/test_cross_validation.cpp** — Integration tests (7 tests)
- **src/cli/main/hardware_test_bridge.h** — Bridge class definition
- **src/cli/main/cross_validation_runner.h** — Runner class definition

---

## Quick Reference: File & Function Locations

| Concept | Location |
|---------|----------|
| CPU cores | `src/plugins/6502/main/cpu6502.cpp`, `src/plugins/45gs02/main/cpu45gs02.cpp` |
| Memory bus | `src/libmem/main/memory_bus.cpp`, `sparse_memory_bus.cpp` |
| I/O devices | `src/plugins/devices/*/main/*.cpp` |
| Expression evaluator | `src/libdebug/main/expression_evaluator.cpp` |
| Debugger UI | `src/gui/main/*.cpp` |
| Machine factory | `src/libcore/main/json_machine_loader.cpp` |
| Plugin loading | `src/plugin_loader/main/plugin_loader.cpp` |
| Assembler registry | `src/libtoolchain/main/toolchain_registry.cpp` |
| Simulator config | `src/libcore/main/sim_config.cpp` (loads `config.json`) |
| CA45 assembler | `src/plugins/45gs02/main/ca45_assembler.cpp` |
| Hypervisor regs | `src/plugins/devices/hypervisor/main/hypervisor_regs.cpp` |
| MEGA65 machine | `src/plugins/machines/mega65/main/machine_mega65.cpp` |
| MCP server | `src/mcp/main/main.cpp` (62 tools: assembler, snapshots, diff_file, analyze_routine, generate_tests, record_audio, load_sid, reverse_step) |
| Disk image parsers | `src/plugins/cbm-loader/main/cbm_sector_disk.cpp` (D64/D71/D80/D81/D82 base) |
| GDB server | `src/cli/main/gdb_server.cpp` (RSP protocol, `--gdb-port` CLI flag) |
| Hardware test bridge | `src/cli/main/hardware_test_bridge.cpp` (Serial/TCP backends for MEGA65) |
| Cross-validation runner | `src/cli/main/cross_validation_runner.cpp` (Emulator vs hardware comparison) |
