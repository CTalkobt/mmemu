# Implementation Roadmap: Multi-Machine Emulator

This roadmap covers the greenfield build of mmemu. All source files are currently
stubs. Phases build incrementally; each one leaves the project in a compilable,
testable state. VIC-20 is the first complete machine target; C64 follows as the
second, reusing most of the same device and bus infrastructure.

Architecture specs are in `.plan/arch.md`. Section references below point there.

---

## Phase 0: Project Foundation

*Goal: Establish the directory layout, build system, and test harness so every
subsequent phase has a clean home.*

- [x] **Directory skeleton**
   - [x] Create `src/libmem/`, `src/libcore/`, `src/libdevices/`, `src/libtoolchain/`, `src/libdebug/` as library roots (currently only stub headers exist).
   - [x] Create `src/libcore/machines/` for per-machine descriptor factories.
   - [x] Create `src/libdevices/devices/` for individual chip implementations.
   - [x] Create `roms/` for BASIC, KERNAL, and character ROM images (`.gitignore`'d; loaded at runtime).
   - [x] Create `tests/` for unit tests.
- [x] **Build system** (`Makefile`)
   - [x] Compile each `src/lib*/` tree into a static archive (`libmem.a`, `libcore.a`, `libdevices.a`, `libtoolchain.a`, `libdebug.a`).
   - [x] Link `mmemu-cli` and `mmemu-mcp` against all five archives.
   - [x] Link `mmemu-gui` against all five archives plus wxWidgets.
   - [x] Add `make test` target that builds and runs `tests/`.
   - [x] Add `make libs` target that builds archives only (no binaries).
   - [x] Propagate `INCLUDES = -Isrc -Isrc/include` to all translation units.
- [x] **Unit test harness** (`tests/`)
   - [x] Choose and document a single-header test framework (e.g., `doctest.h` or a hand-rolled `test_main.cpp`).
   - [x] Add a trivial passing smoke test so `make test` succeeds from the start.
- [x] **ROM loader utility** (`src/libcore/rom_loader.h/cpp`)
   - [x] `bool romLoad(const char *path, uint8_t *buf, size_t expectedSize)` — loads a binary file, returns false with an error message if the size doesn't match.
   - [x] Used by all machine factories to load BASIC/KERNAL/char ROMs at startup.

---

## Phase 0.5: Architectural Refactor (Critique Response)

*Goal: Address structural deficiencies identified in the Phase 0/1 review.*

- [x] **Interface Segregation**
   - [x] Refactor `ICore` to separate execution, register access, and disassembly.
   - [x] Refactor `IBus` to be leaner.
- [x] **Encapsulation**
   - [x] Move `ExecutionObserver* observer` to private/protected with proper accessors in `ICore` and `IBus`.
- [x] **Side-effect-free Peeking**
   - [x] Ensure `IBus::peek8` is strictly side-effect-free; do not delegate to `read8` by default if `read8` can trigger I/O side effects.
- [x] **Plugin ABI Stability**
   - [x] Refactor `SimPluginHostAPI` to use a stable C-compatible interface (opaque pointers/function pointers) instead of C++ classes.
- [x] **Resource Management**
   - [x] Fix memory leaks in `MachineRegistry::createMachine` and `MachineDescriptor` lifecycle.
- [x] **Undefined Behavior**
   - [x] Fix potential UB in bus masking (e.g., `1u << 32`).

---

## Phase 1: libmem — Address Bus Abstraction

*Goal: A working `IBus` interface and a `FlatMemoryBus` concrete implementation.
See arch.md §3.1.*

- [x] **`IBus` interface** (`src/libmem/ibus.h`)
   - [x] `BusConfig` struct: `addrBits`, `dataBits`, `role` (`BusRole` enum), `littleEndian`, `addrMask`.
   - [x] `IBus` abstract class: `read8`, `write8`, `read16/32`, `write16/32` (default: assemble from `read8/write8`).
   - [x] `peek8(addr)` — side-effect-free read for debugger/disassembler (default: delegates to `read8`).
   - [x] `reset()` virtual hook.
   - [x] Snapshot: `stateSize()`, `saveState()`, `loadState()`.
   - [x] Write log: `writeCount()`, `getWrites()`, `clearWriteLog()` — used by `libdebug` write history pane.
- [x] **`FlatMemoryBus`** (`src/libmem/memory_bus.h/cpp`)
   - [x] Heap-allocated byte array, configurable address width (default 16-bit / 64 KB).
   - [x] ROM overlay support: `addOverlay(base, size, data, writable)` — overlays redirect reads (and optionally writes) to a separate buffer, enabling switchable ROM banks without copying.
   - [x] Write log implemented in `FlatMemoryBus` (ring buffer of `{addr, before, after}` triples, appended on every `write8`).
   - [x] `stateSize/save/load` snapshots the full RAM array.
- [x] **Unit tests** (`tests/test_flatmembus.cpp`)
   - [x] Read/write round-trip for all address widths.
   - [x] ROM overlay: write is silently dropped; read returns ROM data.
   - [x] Write log captures correct before/after values.
   - [x] Snapshot/restore produces identical bus state.

---

## Phase 2: libcore — ICore Interface and MOS 6502 CPU

*Goal: A fully functional MOS 6502 core implementing `ICore`, with a register
descriptor table and disassembler stub. See arch.md §3.2.*

- [x] **`ICore` interface** (`src/libcore/icore.h`)
   - [x] `RegDescriptor` struct: `name`, `width` (`RegWidth` enum), `flags` (`REGFLAG_*`), `alias`.
   - [x] `ICore` abstract class — all methods as specified in arch.md §3.2.
   - [x] `ISA_CAP_*` bitmask constants.
   - [x] `ExecutionObserver* observer` member (linked to `libdebug`; initially `nullptr`).
- [x] **`MOS6502` CPU** (`src/libcore/cpu6502.h/cpp`)
   - [x] Internal state: `A`, `X`, `Y`, `SP`, `PC`, `P` (NV-BDIZC), `cycles`.
   - [x] Full NMOS 6502 opcode table (all official opcodes; common illegal opcodes mostly implemented).
   - [x] Register descriptor table: 8 entries — A, X, Y, SP, PC, P, cycles (REGFLAG_INTERNAL), plus a 9th placeholder for future B/Z extension.
   - [x] `step()` — fetch-decode-execute one instruction, return cycle count; fires `observer->onStep()` if set.
   - [x] `isCallAt()` — checks for `$20` (JSR abs); returns 3 (instruction size) or 0.
   - [x] `isReturnAt()` — checks for `$60` (RTS) or `$40` (RTI).
   - [x] `isProgramEnd()` — true if CPU is in a halt/infinite-loop state (detect `JMP $addr` where addr == PC).
   - [x] `writeCallHarness()` — writes `JSR lo hi; RTS` into scratch memory.
   - [x] `disassembleOne()` — inline disassembler for a single instruction (mnemonic + operand string); used by `libtoolchain`.
   - [x] `disassembleEntry()` — fills a `DisasmEntry` with all fields including `isCall`, `isReturn`, `isBranch`, `targetAddr`.
   - [x] Snapshot: `saveState/loadState` uses `sizeof(CPU6502State)` POD blob.
- [x] **`MachineDescriptor` and `CpuSlot`/`BusSlot`** (`src/libcore/machine_desc.h`)
   - [x] Structs as specified in arch.md §3.3: `CpuSlot`, `BusSlot`, `MachineDescriptor`.
   - [x] `onInit`, `onReset`, `schedulerStep` lifecycle hooks.
   - [x] `MemOverlay` list on descriptor.
- [x] **`MachineRegistry`** (`src/libcore/machines/machine_registry.h/cpp`)
   - [x] Global registry: `registerMachine(id, factoryFn)`, `createMachine(id) → MachineDescriptor*`.
   - [x] `enumerate(vector<string> &ids)` for GUI machine selector.
- [x] **Unit tests** (`tests/test_cpu6502.cpp`)
   - [x] Known-output tests for representative opcodes: LDA imm, STA zpg, ADC, SBC, LSR, ROR, BNE (taken/not taken), JSR/RTS, BRK/RTI.
   - [x] Cycle count accuracy for at least one instruction per addressing mode.
   - [x] Snapshot/restore: run 100 steps, save, run 100 more, restore, verify registers match post-save state.

---

## Phase 3: libtoolchain — Disassembler and Assembler Interfaces

*Goal: A standalone disassembler for 6502 code and an assembler wrapper for
KickAssembler. See arch.md §6.*

- [x] **`IDisassembler`** (`src/libtoolchain/idisasm.h`)
   - [x] `DisasmEntry` struct as in arch.md §6.1.
   - [x] `IDisassembler` abstract class: `isaName()`, `disasmOne()`, `disasmEntry()`.
   - [x] **Base Implementation**: Provide functional delegation to `ICore::disassembleOne()` if a specialized disassembler is not provided.
- [x] **`Disassembler6502`** (`src/libtoolchain/disassembler_6502.cpp`)
   - [x] Wraps `MOS6502::disassembleEntry()`.
   - [x] Resolves operand addresses against a `SymbolTable*` if provided (replaces `$C000` with `main` etc.).
- [x] **`IAssembler`** (`src/libtoolchain/iassembler.h`)
   - [x] `AssemblerResult` struct: `success`, `errorMessage`, `outputPath`, `symPath`, `listPath`.
   - [x] `IAssembler` abstract class: `name()`, `isaSupported()`, `assemble()`.
   - [x] **Backend Metadata**: Implement a mechanism to store tool-specific metadata (binary path, directory location, additional command-line options).
- [x] **`KickAssemblerBackend`** (`src/libtoolchain/kickassembler.h/cpp`)
   - [x] Invokes `java -jar tools/KickAss65CE02.jar` as a child process.
   - [x] **Metadata Configuration**: Support configurable directory location for the JAR and additional user-provided command-line options.
   - [x] Parses stdout for errors; populates `AssemblerResult`.
   - [x] Validates that `tools/KickAss65CE02.jar` exists; returns a clear error if not.
- [x] **`SymbolTable`** (`src/libtoolchain/symbol_table.h/cpp`)
   - [x] Load from KickAssembler `.sym` file (address → label map).
   - [x] `nearest(addr)` — returns the closest label at or before `addr`, plus the byte offset.
- [x] **`SourceMap`** (`src/libtoolchain/source_map.h/cpp`)
   - [x] Load from KickAssembler `.lst` file.
   - [x] `addrToSource(addr) → {file, line}`.
   - [x] `sourceToAddr(file, line) → addr`.
- [x] **Unit tests** (`tests/test_disasm6502.cpp`)
   - [x] Disassemble a hand-crafted byte sequence; compare output strings.
   - [x] `DisasmEntry` fields: verify `isCall`, `isBranch`, `targetAddr` for JSR, BNE, JMP.

---

## Phase 4: libdebug — Debug Infrastructure

*Goal: Breakpoints, trace history, write log, execution observer, and state
snapshots. Drives all debug panes described in arch.md §13.*

- [x] **`ExecutionObserver`** (`src/libdebug/execution_observer.h`)
   - [x] `onStep(ICore*, IBus*, DisasmEntry*)` — called by `ICore::step()` after each instruction.
   - [x] `onMemoryWrite(IBus*, uint32_t addr, uint8_t before, uint8_t after)` — called by `IBus::write8`.
   - [x] Default no-op implementation; `DebugContext` is the concrete subclass.
- [x] **`ExpressionEvaluator`** (`src/libdebug/expression_evaluator.h/cpp`)
   - [x] `evaluate(condition, ICore*, IBus*)` — stub; empty condition returns true (full expression parsing deferred).
- [x] **`BreakpointList`** (`src/libdebug/breakpoint_list.h/cpp`)
   - [x] `Breakpoint` struct: `addr`, `type` (EXEC / READ_WATCH / WRITE_WATCH), `condition` string, `hitCount`, `enabled`.
   - [x] `checkExec(addr, cpu, bus)` — returns the matching breakpoint or nullptr; evaluates condition via `ExpressionEvaluator`.
   - [x] `checkWrite(addr, cpu, bus)` — scans write-watch entries.
   - [x] Add, remove, enable/disable by index.
- [x] **`TraceBuffer`** (`src/libdebug/trace_buffer.h/cpp`)
   - [x] Fixed-size ring buffer of `TraceEntry` (addr, mnemonic string, register snapshot, cycle count).
   - [x] `push(entry)` — overwrites oldest on overflow.
   - [x] Configurable buffer size (default 1 000).
- [x] **`DebugContext`** (`src/libdebug/debug_context.h/cpp`)
   - [x] Implements `ExecutionObserver`.
   - [x] Holds a `BreakpointList`, `TraceBuffer`, and snapshot list.
   - [x] `onStep()` — appends to `TraceBuffer`; evaluates exec breakpoints.
   - [x] `saveSnapshot(label)` / `restoreSnapshot(idx)` — stores/restores `ICore` + `IBus` state blobs.
   - [x] `diffSnapshots(i, j)` — returns lists of changed registers and changed memory addresses.
- [x] **Unit tests** (`tests/test_debug.cpp`)
   - [x] Breakpoint fires at correct address; `hitCount` increments.
   - [x] Write-watch fires on matching write; not on reads.
   - [x] Trace buffer wraps correctly at capacity.
   - [x] Snapshot round-trip: save → mutate → restore → verify state identical to save point.

---

## Phase 5: CLI Target Implementation

*Goal: A functional command-line interface for manual machine control.*

- [x] **Command Loop** (`src/cli/main.cpp`)
   - [x] Interactive REPL with input parsing.
   - [x] `step`, `run`, `pause`, `stop` commands mapped to `ICore`.
   - [x] `regs` command: displays all registers from `ICore::regDescriptor()`.
   - [x] `m` (memory) and `f` (fill) using `IBus::read8/write8`.
   - [x] `disasm` command using `IDisassembler` interface.
   - [x] `load` command to load binary files into memory.
   - [x] **Direct Instruction Execution**: Support `.<instr>` prefix for instant assembly and execution.
- [x] **Machine Integration**
   - [x] Support selecting machines from `MachineRegistry`.
   - [x] Handle multiple CPU slots if present in the descriptor.
- [x] **Debug Commands**
   - [x] `break <addr>` / `watch <read|write> <addr>` — add exec/watchpoints via `DebugContext`.
   - [x] `delete <id>`, `enable <id>`, `disable <id>` — manage breakpoints.
   - [x] `info breaks` — list active breakpoints.
   - [x] `run [addr]` loops until `DebugContext::isPaused()` or `isProgramEnd()`.
   - [x] Integration test: `tests/test_breakpoints.cpp`.

---

## Phase 6: Plugin Architecture & Refactor

*Goal: Decouple 6502-specific code into a loadable plugin and implement the Plugin Loader.*

- [x] **Finalize Plugin ABI** (`src/include/mmemu_plugin_api.h`)
   - [x] Define C-compatible structs for `SimPluginManifest`, `CorePluginInfo`, etc.
   - [x] Add factory function pointers for `ICore`, `IDisassembler`, and `IAssembler`.
- [x] **Plugin Loader Implementation** (`src/plugin_loader/`)
   - [x] Implement `PluginLoader::load(path)` using `dlopen`/`dlsym`.
   - [x] Implement automatic plugin discovery in standard paths.
   - [x] Registry integration: automatically register plugin-provided cores/toolchains.
- [x] **Create 6502 Plugin** (`src/plugins/6502/`)
   - [x] Move `MOS6502`, `Disassembler6502`, and `Assembler6502` to the plugin directory.
   - [x] Implement `mmemuPluginInit` entry point for the 6502 plugin.
   - [x] Update `Makefile` to build `mmemu-plugin-6502.so`.
- [x] **Decouple Host**
   - [x] Remove hardcoded 6502 references from `libcore` and `libtoolchain`.
   - [x] Update CLI to use `PluginLoader` and request "6502" tools from the registry.

---

## Phase 7: MCP Target Implementation

*Goal: Enable AI agents to interact with the simulator via the Model Context Protocol.*

- [x] **MCP Server** (`src/mcp/main.cpp`)
   - [x] Implementation of the MCP specification over JSON-RPC 2.0.
   - [x] Communication over stdin/stdout.
- [x] **Tools Registration**
   - [x] `step_cpu(machine_id, count)`: Execute N instructions.
   - [x] `read_memory(machine_id, addr, size)`: Hex dump or raw data.
   - [x] `write_memory(machine_id, addr, bytes)`: Inject data.
   - [x] `read_registers(machine_id)`: Get full CPU state.
- [x] **Resource Registration**
   - [x] `machine_state`: Snapshot of the current session state.
- [x] **Validation Harness**
   - [x] Use MCP tools to automate the execution and validation of 6502 test programs.

---

## Phase 8: GUI Target Implementation

*Goal: A rich graphical interface for machine simulation and debugging.*
See Cross-Cutting GUI Standards for pane requirements. 

- [x] **Application Skeleton** (`src/gui/main.cpp`)
   - [x] wxWidgets main frame with menu, toolbar, and status bar.
   - [x] Tabbed or multi-pane layout for different debug views.
- [x] **Register Pane**
   - [x] Dynamically creates rows from `RegDescriptor` table.
   - [x] Highlights changed registers since the last step.
   - [x] Supports multi-CPU slot selection.
- [x] **Memory Pane**
   - [x] Hex/ASCII dump with scroll support.
   - [x] Bus selector dropdown for Harvard architectures.
   - [x] Highlights recently written bytes using `IBus::getWrites()`.
- [x] **Disassembly Pane**
   - [x] Scrollable view centered on the current PC.
   - [x] Integration with `IDisassembler` and `SymbolTable`.
- [x] **Machine Selector**
   - [x] Dialog to choose machine presets from `MachineRegistry`.
   - [x] Display machine ID, display name, and license class.

---

## Phase 8.5: GUI Advanced Debugger Features

*Goal: Port advanced CLI features to the GUI to provide parity for power users.*

- [x] **CLI Console Pane**
   - [x] Integrated text console for direct command input (matching Phase 5 CLI features).
   - [x] Support for all CLI commands within the GUI environment.
- [x] **Memory Manipulation Tools**
   - [x] "Fill Memory" dialog: Specify start address, length, and byte value.
   - [x] "Copy Memory" dialog: Specify source range and destination address.
- [x] **Inline Assembler**
   - [x] "Assemble" dialog: Input single assembly instruction and target address.
   - [x] Integration with `IAssembler::assembleLine` from `libtoolchain`.
- [x] **Navigation & Search**
   - [x] "Go to Address" shortcut/dialog in Memory and Disassembly panes. Allow
          setting of PC in CLI if not already available. 
   - [x] Search for hex patterns or ASCII strings in memory. Add capability to CLI and 
          MCP if not already available.

---

## Phase 9: libdevices — IOHandler Base Layer

*Goal: The abstract `IOHandler`/`IORegistry` infrastructure shared by all
devices. No concrete chips yet — those come in Phases 10 and 11.*

- [x] **`IOHandler`** (`src/libdevices/io_handler.h`)
   - [x] Abstract base: `name()`, `baseAddr()`, `addrMask()`.
   - [x] `ioRead(IBus*, uint32_t addr, uint8_t* val) → bool` — return true if handled.
   - [x] `ioWrite(IBus*, uint32_t addr, uint8_t val) → bool`.
   - [x] `reset()`, `tick(uint64_t cycles)` — called each step for timers/counters.
- [x] **`IORegistry`** (`src/libdevices/io_registry.h/cpp`)
   - [x] `registerHandler(IOHandler*)` — stores in priority order by base address.
   - [x] `dispatchRead(addr, val*)` / `dispatchWrite(addr, val)` — walks handler list, returns on first match.
   - [x] `tickAll(uint64_t cycles)` — calls `tick()` on every handler.
   - [x] `resetAll()`.
   - [x] `enumerate(vector<IOHandler*>&)` — for device inspector pane.
- [x] **`IPortDevice`** (`src/libdevices/iport_device.h`)
   - [x] `readPort() → uint8_t`, `writePort(uint8_t val)`, `setDdr(uint8_t ddr)`.
   - [x] Used to inject keyboard matrix, joystick, cassette, etc. into VIA ports without coupling VIA logic to specific peripherals.
- [x] **`ISignalLine`** (`src/libdevices/isignal_line.h`)
   - [x] `get() → bool`, `set(bool)`, `pulse()`.
    - [X] Used for single-bit control lines: ATN, SRQ, cassette motor, IRQ, NMI.
- [X] **Unit tests** (`tests/test_io_registry.cpp`)
    - [X] Two handlers at non-overlapping ranges; verify each is dispatched correctly.
    - [X] Read returns false when no handler covers the address.
   - [x] `tickAll` calls `tick()` on every registered handler.

---

## Phase 10: VIC-20 Machine [COMPLETED]

*Goal: A fully bootable VIC-20 simulation. This is the first machine where all
five libraries are exercised together.*

### Phase 10.1 MOS 6522 VIA (`src/plugins/devices/via6522/`) [COMPLETED]
### Phase 10.2 MOS 6560/6561 VIC (`src/plugins/devices/vic6560/`) [COMPLETED]
### Phase 10.3 VIC-20 Memory Map [COMPLETED]
### Phase 10.4 VIC-20 Peripheral Wiring [COMPLETED]
### Phase 10.5 Plugin Extension Host API [COMPLETED]
### Phase 10.6 VIC-20 Integration Test [COMPLETED]
### Phase 10.7 VICE ROM Importer Plugin [COMPLETED]

---

## Phase 11: C64 Machine [COMPLETED]

*Goal: A bootable C64 simulation. Reuses `FlatMemoryBus`, `MOS6502` (via
`MOS6510` subclass), VIA→CIA upgrade, and adds VIC-II and SID.*

### Phase 11.1 MOS 6510 CPU [COMPLETED]
### Phase 11.2 C64 PLA Banking [COMPLETED]
### Phase 11.3 MOS 6526 CIA [COMPLETED]
### Phase 11.4 MOS 6567/6569 VIC-II [COMPLETED]
### Phase 11.5 MOS 6581 SID [COMPLETED]
### Phase 11.6 C64 Memory Map and Wiring [COMPLETED]

---

## Phase 12: Commodore PET/CBM Machine [COMPLETED]

### Phase 12.1: MOS 6520 PIA [COMPLETED]
### Phase 12.2: MOS 6545/6845 CRTC [COMPLETED]
### Phase 12.3: PET Video Subsystem [COMPLETED]
### Phase 12.4: IEEE-488 Bus Implementation [COMPLETED]
### Phase 12.5: PET Machine Factory and Memory Map [COMPLETED]
### Phase 12.6: PET Integration Tests [COMPLETED]

---

## Phase 13: Runtime Image and Cartridge Loading [PARTIALLY COMPLETED]

---

## Phase 14: Tape (Datasette) Support [COMPLETED]

---

## Phase 15: IEC Serial Bus and Disk Drive Support [COMPLETED]

---

## Phase 18: 45GS02 CPU [COMPLETED]

*Goal: The full MEGA65 CPU, extending 65CE02 with 32-bit Quad operations, the
MAP instruction for 28-bit memory access, variable speed, math acceleration
registers, and Hypervisor mode. See `ref/CBM/Mega65/mega65-book.txt` Appendix K.*

### Phase 18.1 45GS02 Core (`src/plugins/45gs02/main/cpu45gs02.h/cpp`) [COMPLETED]

- [x] Implement `MOS45GS02` inheriting from `ICore`.
- [x] **Quad (Q) pseudo-register**: Q = {Z[7:0], Y[7:0], X[7:0], A[7:0]} as a
      virtual 32-bit register. Not stored separately — assembled on demand from
      the four 8-bit registers.
- [x] **MAP state**: internal struct `MapState` holding the mapping offsets
      and enable bits.
- [x] **MAP instruction** (`$5C`): enables MAP translation.
- [x] **EOM instruction** (`$7C`): disables MAP translation.
- [x] **Quad load/store**: `LDQ zp`, `LDQ abs`, `LDQ (zp),z`, `STQ zp`,
      `STQ abs`, `STQ (zp),z` — moves 4 bytes between Q and memory.
- [x] **Quad arithmetic**: `ADCQ`, `SBCQ`, `ANDQ`, `ORQ`, `EORQ`, `CMPQ` —
      operate on the full Q register.
- [x] **Quad shifts**: `ASLQ`, `LSRQ`, `ROLQ`, `RORQ`, `ASRQ` — 32-bit shift/rotate.
- [x] **32-bit flat indirect**: `NOP` prefix ($EA) before `LDA (zp),z` etc.
      activates 28-bit physical addressing.
- [x] **New Addressing Modes**: `(d,S),Y` stack-relative indirect indexed.
- [x] **Long branches**: All relative branches have 16-bit long variants.
- [x] **16-bit memory operations**: `ASW`, `ROW`, `DEW`, `INW`.
- [x] **NEG**: Two's complement negation of Accumulator and Quad.
- [x] **Hypervisor mode**: Traps writing to `$D6CF` immediately halt simulation.
- [x] **`disassembleOne()` / `disassembleEntry()`**: full coverage for 45GS02.
- [x] **Register descriptor table**: A, X, Y, Z, B, SP, PC, P, Q.
- [x] Snapshot: `saveState/loadState` implemented.

### Phase 18.2 45GS02 Plugin (`src/plugins/45gs02/`) [COMPLETED]

- [x] Create `src/plugins/45gs02/` plugin; `mmemuPluginInit` exposes
      `CPU45GS02` core and `Disassembler45GS02`.
- [x] `Makefile` target: `lib/mmemu-plugin-45gs02.so`.

### Phase 18.3 Unit Tests (`tests/test_cpu45gs02.cpp`) [COMPLETED]

- [x] **45GS02 validation suite**: Automated cross-validation against `xemu-xmega65`.
- [x] **Advanced tests**: `INW`, `DEW`, `ASW`, `ROW`, `NEG`, `MAP`, `EOM`, `B` register.

---

## Phase 19: SparseMemoryBus(28) and MAP MMU

*Goal: A 28-bit sparse bus suitable for the MEGA65's 256 MB address space, plus
the MAP MMU IOHandler that translates 16-bit CPU virtual addresses to 28-bit
physical addresses. See arch.md §3.1 and the `SparseMemoryBus(28)` note.*

### Phase 19.1 `SparseMemoryBus` (`src/libmem/sparse_memory_bus.h/cpp`)

- [ ] Template (or configurable) `SparseMemoryBus` class implementing `IBus`.
- [ ] Internally uses a `std::unordered_map<uint32_t, uint8_t*>` of 4 KB pages,
      allocated lazily on first write (reads to unallocated pages return $FF).
- [ ] Constructor takes `addrBits` (28 for MEGA65; also used for 65C816 at 24).
- [ ] `addRegion(base28, size, data, writable)` — maps a pre-allocated buffer
      (e.g., ROM image) into the sparse space; reads/writes redirect to `data`.
      Writable=false silently drops writes (ROM protection).
- [ ] `read8(addr28)` / `write8(addr28, val)`: page lookup → buffer offset.
      Write log appended on every `write8`.
- [ ] `peek8(addr28)`: side-effect-free read, same as `read8` but does not
      trigger write-watch callbacks.
- [ ] `stateSize/saveState/loadState`: serialises only allocated pages (sparse
      snapshot); restores by re-allocating and populating those pages.
- [ ] `reset()`: deallocates all dynamically allocated pages; pre-mapped regions
      (ROM buffers) remain mapped but intact.

### Phase 19.2 MAP MMU IOHandler (`src/plugins/devices/map_mmu/`)

*The MAP MMU sits between the CPU's 16-bit address space and the 28-bit
`SparseMemoryBus`. It intercepts every CPU read/write and translates.*

- [ ] `MapMmu` implements `IOHandler` (responds to the full $0000–$FFFF range)
      but is wired as a *pass-through* bus adapter rather than a device:
    - Internally holds a reference to the downstream `SparseMemoryBus*`.
    - The CPU's `IBus*` slot points to the `MapMmu` wrapper, not directly to
      `SparseMemoryBus`; `MapMmu::ioRead/ioWrite` performs the translation.
- [ ] **MAP state**: eight 20-bit offsets (four for $0000–$7FFF, four for
      $8000–$FFFF, each covering an 8 KB block) plus a corresponding enable
      bitmask. Offsets are set by `CPU45GS02` calling `setMapState()`.
- [ ] **Translation**: for each CPU 16-bit address:
    - Determine which 8 KB block it falls in (bits 15:13 → index 0–7).
    - If that block's enable bit is set: `phys = (offset[i] << 8) | (vaddr & 0x1FFF)`.
    - If not enabled: `phys = (c64_bank << 16) | vaddr` (C64-style banking,
      upper bank byte from the 6510-compatible port at $00/$01).
- [ ] **ROM overlay fast path**: C64-compatibility banking (LORAM/HIRAM/CHAREN)
      is handled by a `C64BankController` (similar to Phase 11.2 C64 PLA) that
      intercepts writes to $00/$01 and updates `SparseMemoryBus` overlays for
      KERNAL ($E000–$FFFF), BASIC ($A000–$BFFF), and I/O/CHARROM ($D000–$DFFF).
- [ ] **I/O personality**: writes to $D02F (KEY register) cycle through
      C64 → C65 → MEGA65 personality; the active personality controls which
      device handlers are visible in the $D000–$DFFF window.
- [ ] `setMapState(const MapState&)` — called by `CPU45GS02::step()` when it
      executes MAP; stores new offsets and enable bits atomically.
- [ ] `reset()`: clears all MAP enables; C64 banking lines default to $37
      (LORAM=HIRAM=CHAREN=1 — KERNAL+BASIC visible, I/O active).

### Phase 19.3 Unit Tests (`tests/test_sparse_memory_bus.cpp`)

- [ ] Read from unallocated page returns $FF without crash.
- [ ] Write to page allocates it; subsequent read returns the written value.
- [ ] `addRegion()` with writable=false: write is silently dropped; read returns
      region data.
- [ ] Two non-overlapping regions at $20000 and $40000; verify each returns
      its own data independently.
- [ ] Snapshot: allocate two pages, write distinct values, save, overwrite,
      restore; verify original values recovered; unallocated page still $FF.
- [ ] MAP MMU translation: enable MAP for $A000–$BFFF block pointing to $40000;
      CPU read at $A000 reads from physical $40000; CPU read at $8000 (not
      mapped) reads from physical $8000.
- [ ] C64 bank switch: set HIRAM=0; verify $E000 no longer reads KERNAL ROM
      overlay (reads underlying RAM).

---

## Phase 20: MEGA65 Peripheral Chips

*Goal: VIC-IV, dual SID, F018B DMA controller, and math acceleration registers
as concrete `IOHandler` implementations.*

### Phase 20.1 VIC-IV (`src/plugins/devices/vic4/`)

*Extends VIC-II (Phase 11.4). The VIC-IV is backward-compatible at $D000–$D02E;
new registers are unlocked by the I/O personality at $D030 and above.*

- [ ] Inherits or wraps `VIC2`; re-uses sprite engine and VIC-II register file.
- [ ] **$D02F (KEY)**: personality unlock register. Write $47 ('G') then $53
      ('S') to enable MEGA65 extended registers; write any other value to lock.
      Managed by `MapMmu` but exposed to `VIC4` via a callback so the VIC-IV
      can enable its extended register window.
- [ ] **$D030 (VIC-III banking)**: ROM control bits (CRAM2K, ROM-B), V400 enable.
- [ ] **$D031 (VIC-III control)**: H640 (80-column mode), V400, FAST CPU bit
      (forwards to `CPU45GS02` speed scalar), PAL/NTSC select.
- [ ] **$D040–$D07F (VIC-IV extended registers)**:
    - $D040–$D043: character X/Y pixel position (16-bit each).
    - $D044: `CHRCOUNT` (character columns in current row).
    - $D045: extended control (H1280 super-wide, 16-bit char mode enable).
    - $D046: horizontal character count (used for 40/80-column switching).
    - $D047: vertical character count.
    - $D048–$D049: left/right border pixel positions.
    - $D04A–$D04B: top/bottom border raster positions (11-bit).
    - $D04C–$D04F: screen RAM base address (32-bit, 28-bit physical).
    - $D050–$D053: character set base address (32-bit).
    - $D054: sprite enable bits (extended, 8 sprites from $D015 + FCM flag).
    - $D055–$D057: additional sprite control (16-colour enable per sprite).
    - $D058–$D05B: colour RAM base address (32-bit; default $FF80000).
    - $D05C: sprite pointer table address (relative to screen RAM base).
    - $D05D: raster compare high bit (bit 10 of 11-bit raster counter).
    - $D05E: horizontal smooth scroll (extended, 4-bit).
    - $D05F: vertical smooth scroll (extended, 4-bit).
- [ ] **$D100–$D3FF (colour palette)**: 256 entries × 3 bytes (R, G, B in
      separate $D100/$D200/$D300 arrays). Default power-on palette matches the
      16-colour MEGA65 system palette (compatible with C64 colours 0–15).
- [ ] **Video modes** (controlled by $D031/$D054/$D045):
    - Standard 40-column text (VIC-II compatible).
    - 80-column text (H640 bit).
    - Multicolour text and bitmap (VIC-II compatible).
    - Full Colour Mode (FCM): each character cell uses 64 bytes of character
      data for a 8×8 full-colour glyph; screen RAM holds 16-bit character
      pointers. `renderFrame()` fetches from 28-bit character base address via
      `IBus::peek8()` on the `SparseMemoryBus`.
- [ ] **Raster counter**: 11-bit (0–2047), set via $D012 (bits 7:0) and $D05D
      (bit 10); compare fires IRQ via `ISignalLine`.
- [ ] **32 KB colour RAM** at 28-bit base $FF80000 (configurable via $D058–$D05B).
      Allocated as a pre-mapped `SparseMemoryBus` region; also mirrored at
      $D800–$DBFF (first 1 KB) for C64 compatibility.
- [ ] `renderFrame()`: implements the MEGA65 raster pipeline for the active
      video mode; fills an RGBA pixel buffer using register state and bus data.
      For FCM and 80-column modes, accesses the 28-bit address space.
- [ ] `tick(cycles)`: drives raster counter; fires raster IRQ.
- [ ] `ChipRegDescriptor` table: covers all $D000–$D07F registers and palette.

### Phase 20.2 F018B DMA Controller (`src/plugins/devices/dma_f018b/`)

*The MEGA65's DMA controller is an enhanced version of the C65 F018 chip. It
supports 28-bit source/destination addresses, chained job lists, and four
operations: copy, fill, swap, and mix.*

- [ ] **Register file** (at $D700–$D70F in MEGA65 personality):
    - $D700: DMA list address low byte (written to trigger job execution).
    - $D701: DMA list address high byte.
    - $D702: DMA list address bank byte (bits 19:16 of 28-bit list pointer).
    - $D703: DMA execute register — writing triggers DMA using the current
             list address; a write to $D700 also triggers (C65 compatibility).
    - $D704: DMA upper address byte (bits 27:20 of 28-bit list pointer).
    - $D705: Enhanced DMA options: modulus mode, slope, fractional stepping.
    - $D706–$D70F: reserved / enhanced per-job override bytes.
- [ ] **Job list format** (F018B enhanced):
    - Byte 0 (command): bits 1:0 = operation (0=copy, 1=fill, 2=swap, 3=mix);
      bit 2 = chain (1 = another job follows); bit 3 = interrupt on completion;
      bit 6 = enhanced format (extra header bytes follow).
    - Enhanced prefix (if bit 6 set): additional bytes for source/dest bank,
      stepping, and modulus override.
    - Bytes 1–2: count (16-bit, number of bytes).
    - Bytes 3–5: source address (24-bit; bank from $D704/$D702).
    - Bytes 6–8: destination address (24-bit).
    - Byte 9: chain/end byte (0 = end of list, 4 = next job follows).
- [ ] **Execution**: `ioWrite($D703, any)` fetches the job list from the 28-bit
      pointer via `SparseMemoryBus::read8()`; processes all chained jobs in a
      single `tick()` call (DMA freezes the CPU — set `m_dmaActive` flag which
      `CPU45GS02::step()` checks and spins on until cleared).
- [ ] **Operations**:
    - Copy: `src[i]` → `dst[i]` for `count` bytes; source/dest may overlap
      (copy direction chosen by address relationship).
    - Fill: `dst[i] = fill_byte` (fill_byte is the low byte of the source
      address field in fill mode).
    - Swap: atomically exchanges `src[i]` and `dst[i]` (read-modify-write).
- [ ] `reset()`: clears list address and active flag.
- [ ] `ChipRegDescriptor` table covering $D700–$D70F.

### Phase 20.3 Math Acceleration Registers (`src/plugins/devices/math_accel/`)

*Hardware multiply and divide completing in a single CPU cycle. Exposed as an
`IOHandler` at $D770–$D77F.*

- [ ] **Multiply**: write 16-bit MULTINA ($D770–$D771) and MULTINB ($D772–$D773);
      result appears immediately in MULTOUT ($D774–$D777, 32-bit read-only).
- [ ] **Divide**: write 32-bit DIVINA ($D778–$D77B) and 16-bit DIVINB ($D77C–$D77D);
      DIVOUT quotient ($D77E–$D77F) and remainder ($D77C–$D77D read-back) are
      available on the very next read (model as synchronous — no latency).
- [ ] Division by zero: set quotient and remainder to $FFFF (hardware behaviour).
- [ ] `ioRead` / `ioWrite` implement the register semantics; no `tick()` logic
      required (combinatorial hardware model).

### Phase 20.4 Dual SID (`src/plugins/devices/sid_pair/`)

- [ ] Thin wrapper that instantiates two `SID6581` objects (Phase 11.5) at
      $D400–$D41F (SID1) and $D420–$D43F (SID2).
- [ ] Stereo mix: SID1 output mixed right-biased, SID2 output mixed left-biased
      (MEGA65 default panning). Mix weights configurable via constructor.
- [ ] `IOHandler` spans $D400–$D43F; dispatch to SID1 or SID2 by address bit 5.
- [ ] `reset()`: resets both SIDs.
- [ ] `tick(cycles)`: ticks both SIDs.

### Phase 20.5 Unit Tests (`tests/test_mega65_chips.cpp`)

- [ ] **VIC-IV**: unlock extended registers via KEY; write $D04C–$D04F with a
      non-standard 28-bit screen address; verify `renderFrame()` fetches
      character codes from that address (use a mock `SparseMemoryBus` pre-seeded
      with known data).
- [ ] **VIC-IV palette**: write 3 bytes to $D100/$D200/$D300 for entry 1; read
      back each byte; verify values match.
- [ ] **VIC-IV raster**: tick past raster compare; verify IRQ signal fired.
- [ ] **DMA copy**: set up a known source region at $10000; trigger a copy DMA
      to $20000 for 256 bytes; verify destination matches source.
- [ ] **DMA fill**: trigger fill DMA at $30000 for 128 bytes with fill value
      $42; verify all 128 bytes are $42.
- [ ] **DMA chain**: two-job chain (fill then copy); verify both jobs executed
      in sequence in one trigger call.
- [ ] **Math multiply**: write MULTINA=$0100, MULTINB=$0100; read MULTOUT;
      verify result = $00010000 (256 × 256).
- [ ] **Math divide**: write DIVINA=$00010000, DIVINB=$0100; verify quotient
      = $0100, remainder = $0000.
- [ ] **Math divide by zero**: DIVINB=0; verify quotient = $FFFF.
- [ ] **Dual SID dispatch**: write to $D404 (SID1 voice 1 control) and $D424
      (SID2 voice 1 control) independently; verify both `SID6581` objects
      receive separate writes without crosstalk.

---

## Phase 21: MEGA65 Memory Map and Machine Factory [COMPLETED]

*Goal: A bootable MEGA65 simulation with the full 28-bit address space wired,
all devices registered, and I/O personality switching functional.*

### Phase 21.1 MEGA65 28-bit Memory Map

```
28-bit Range      Bank(s)  Description
$0000000–$00FFFFF  0–15    Chip RAM (16 × 64 KB = 1 MB)
  $000000–$0FFFF    0      Bank 0: zero page, stack, KERNAL vars, user RAM
  $010000–$01FFFF   1      Bank 1: DOS buffers, BASIC arrays
  $020000–$03FFFF   2–3    ROM area (MEGA65.ROM); write-protected at boot
  $040000–$05FFFF   4–5    Expansion chip RAM (128 KB for graphics/data)
  $060000–$0FFFFF   6–15   Additional chip RAM
$4000000–$7FFFFFF   —      Slow bus / cartridge port (unmapped by default)
$8000000–$87FFFFF   —      Attic RAM (8 MB; modelled as sparse pages)
$FF80000–$FF87FFF   —      Colour RAM (32 KB dedicated)
$FFD0000–$FFDFFFF   —      Direct I/O (28-bit access to VIC-IV, SID, DMA)
```

**16-bit CPU view (Bank 0 default, I/O personality MEGA65):**
```
$0000–$00FF   Base page (default; relocatable via B register)
$0100–$01FF   Stack (default; 16-bit SP in native mode)
$0200–$D7FF   User RAM (Bank 0 chip RAM)
$D000–$D02E   VIC-IV registers (VIC-II compatible)
$D02F         KEY register (personality unlock)
$D030–$D031   VIC-III banking + control
$D040–$D07F   VIC-IV extended registers (when unlocked)
$D100–$D3FF   Colour palette (R/G/B arrays, when unlocked)
$D400–$D41F   SID1
$D420–$D43F   SID2
$D600–$D6FF   UART, SD card controller, RTC (stub IOHandler)
$D700–$D70F   DMA controller (F018B)
$D770–$D77F   Math acceleration registers
$D800–$DBFF   Colour RAM mirror (first 1 KB of $FF80000)
$DC00–$DCFF   CIA1
$DD00–$DDFF   CIA2
$DE00–$DFFF   Cartridge I/O (stub)
$E000–$FFFF   KERNAL ROM (from MEGA65.ROM; overlay in Banks 2–3)
```

### Phase 21.2 Machine Factory (`src/plugins/machines/rawMega65/`) [COMPLETED]

- [x] `rawMega65` machine preset for bare-metal testing.
- [x] JSON-based machine configuration (`machines/rawMega65.json`).
- [x] Initial wiring of 45GS02 to system bus.
- [x] Full 28-bit bus integration with `SparseMemoryBus`.
- [x] All I/O devices registered in `IORegistry`.

---

## Phase 22: MEGA65 Keyboard

*Goal: Wire the MEGA65 keyboard (C65-derived matrix) to CIA1.*

### Phase 22.1 C65 Keyboard Matrix (`src/plugins/devices/keyboard/main/keyboard_matrix_c65.h/cpp`)

- [ ] 8×8 matrix (64 positions), wired to CIA1 port A (column output) and
      CIA1 port B (row input) — same electrical topology as the C64 but with
      a different key layout.
- [ ] `keyDown(row, col)` / `keyUp(row, col)` API.
- [ ] Implements `IPortDevice` on both CIA1 ports; VIA reads return active-low
      row bits for the currently scanned column.
- [ ] **Restore key**: wired directly to CIA1 NMI line (same as C64); a
      dedicated `ISignalLine` for NMI is pulsed on Restore press.
- [ ] **CAPS LOCK LED**: `ISignalLine` output from the keyboard matrix to
      the host GUI for visual feedback. Host sets the LED state.
- [ ] Key mapping constants: expose a `MEGA65_KEY_*` enum for all physical keys
      including the extra MEGA65-specific keys (Mega key, NO SCROLL, ALT).

### Phase 22.2 Joystick Ports

- [ ] Two joystick ports wired to CIA1 port B (joystick 2) and CIA2 port A
      (joystick 1), active-low — identical to the C64 wiring. Reuse the
      `Joystick` class from Phase 10.4.

---

## Phase 23: MEGA65 Integration Tests

*Goal: Smoke tests that exercise the complete machine stack end-to-end.*

- [ ] **MAP test**: assemble a tiny program that executes `MAP` to redirect
      $A000–$BFFF to bank 4 ($40000); writes a sentinel byte at $A000;
      verifies via `SparseMemoryBus::peek8($40000)` that the byte appears at
      the physical address.
- [ ] **DMA fill test**: trigger a DMA fill of 256 bytes at $2000 with $55;
      verify via bus peek that all 256 bytes are $55; confirm CPU was stalled
      (DMA active flag was set then cleared).
- [ ] **VIC-IV raster test**: tick enough cycles to advance at least two raster
      lines; read raster counter via `IORegistry::dispatchRead($D012)`;
      verify it advanced.
- [ ] **Dual SID write test**: write distinct frequency values to SID1 ($D400)
      and SID2 ($D420) voice 1; read them back; confirm values are independent.
- [ ] **Math accelerator test**: write MULTINA=200 ($00C8), MULTINB=200 ($00C8)
      via `IORegistry::dispatchWrite`; read MULTOUT; verify = 40000 ($9C40).
- [ ] **Personality switch test**: start in C64 personality; write G/S to $D02F
      to advance to MEGA65 personality; write to $D04C (VIC-IV extended reg);
      verify the write reaches VIC-IV. Lock personality (write 0 to $D02F);
      verify $D04C write no longer dispatches to VIC-IV.
- [ ] **C64 bank switch test**: set CPU port $01 = $35 (HIRAM=0, KERNAL
      hidden); read $FFFC; verify it returns RAM content, not KERNAL vector.
- [ ] **Extension host verification**: verify `machineId == "mega65"` for pane
      filtering (Phase 10.5 `PluginPaneManager`).

---

## Phase 24: MEGA65 ROM Importer Plugin (`src/plugins/mega65Importer/`) [COMPLETED]

*Goal: Help users obtain the MEGA65.ROM file from a local xemu installation or
MEGA65 SD card image.*

### Phase 24.1 Plugin Identity and Wiring [COMPLETED]

- [x] Plugin entry point `mmemuPluginInit` in
      `src/plugins/mega65Importer/main/plugin_main.cpp`.
- [x] Registers CLI command, GUI pane, and MCP tool.
- [x] `Makefile` target: `lib/mmemu-plugin-mega65-importer.so`.

### Phase 24.2 Discovery Engine [COMPLETED]

- [x] `discoverSources("mega65")` searches `xemu` paths.
- [x] Supports manual selection via GUI.
- [x] `romFilesFor("mega65")` returns `MEGA65.ROM` spec.

### Phase 24.3 Import Operation [COMPLETED]

- [x] Implements `importRoms()` with destination validation.
- [x] Never overwrites unless requested.

### Phase 24.4 CLI, GUI, and MCP Integration [COMPLETED]

- [x] **CLI**: `importmega65` command.
- [x] **GUI pane**: `Mega65RomImportPane` with "Manual Selection...".
- [x] **MCP tool**: `"import_mega65_roms"`.

### Phase 24.5 Unit Tests [COMPLETED]

- [x] `tests/test_mega65_importer.cpp` suite.

---

## Phase 25: Commodore Plus/4 and C16 (TED-based)

*Goal: Implement the TED-based Commodore machines (C16, C116, Plus/4).*

### Phase 25.1: MOS 7360 / 8360 TED (`src/plugins/devices/ted7360/`)

- [ ] Implement `TED7360 : public IOHandler`.
- [ ] **Register File**: 64 registers ($FF00–$FF3F) controlling video, sound, 
      timers, and memory banking.
- [ ] **Internal Timers**:
    - Three 16-bit interval timers (Timer 1, 2, 3).
    - Timer 3 also drives the cursor blink and system clock.
- [ ] **Banking Logic**:
    - Integrated ROM/RAM banking control (BASIC, KERNAL, Function ROMs).
    - `ISignalLine` outputs for LORAM/HIRAM equivalents.
- [ ] **Sound**:
    - Two square wave channels (10-bit frequency).
    - Channel 2 can be switched to digital noise.
    - Master volume control.
- [ ] **I/O**:
    - Keyboard matrix scanning (shared with video registers).
    - Joystick port reading.

### Phase 25.2: TED Video and Palette (`src/plugins/devices/ted_video/`)

- [ ] Implement `TedVideo` inheriting from `IVideoOutput`.
- [ ] **121-Color Palette**:
    - 15 hues (plus black) and 8 luminance levels.
    - Implement the luminance scaling logic in the RGBA renderer.
- [ ] **Video Modes**:
    - Standard 40-column text.
    - Multicolor text.
    - Multicolor bitmap.
    - Extended background color.
- [ ] **Raster Pipeline**:
    - Raster counter and compare (triggering IRQ).
    - Horizontal and vertical smooth scrolling.
- [ ] `renderFrame()`: produces RGBA buffer using TED registers and memory.

### Phase 25.3: MOS 6551 ACIA (`src/plugins/devices/acia6551/`)

- [ ] Implement `ACIA6551 : public IOHandler` at $FD00.
- [ ] **Registers**: Control, Command, Status, and Data.
- [ ] **Baud Rate Generator**: Support for standard rates (50 to 19200 baud).
- [ ] **Interrupts**: IRQ on transmit buffer empty or receive buffer full.

### Phase 25.4: Plus/4 Machine Factory and Memory Map

- [ ] `MachineDescriptor` for `"plus4"` and `"c16"`.
- [ ] **Memory Map**:
    - C16: 16 KB RAM ($0000–$3FFF).
    - Plus/4: 64 KB RAM ($0000–$FDFF).
    - ROMs: BASIC ($8000), KERNAL ($E000).
- [ ] **Keyboard**: Implement the 8x8 matrix scanned via TED registers.
- [ ] `onReset`: Load ROMs, reset TED and ACIA.

### Phase 25.5: Plus/4 Integration Tests

- [ ] **TED Color Test**: Cycle through all 121 colors and verify RGBA output.
- [ ] **TED Timer Test**: Program Timer 1 for IRQ and verify `sigIrq` pulses.
- [ ] **TED Banking Test**: Switch between RAM and ROM at $8000 via TED 
      registers; verify data change.
- [ ] **ACIA Loopback Test**: Connect TX to RX and verify data round-trip.

---

## Phase 26: Atari 8-bit Family (400/800/XL/XE) [PARTIALLY COMPLETED]

*Goal: Cycle-accurate emulation of the ANTIC, GTIA, and POKEY trio.*

- [x] **ROM Collection**: System ROMs (OS-A, OS-B, XL, BASIC, CHAR) collected in `roms/atari/`.
- [x] **ANTIC DMA Engine** (`src/plugins/devices/antic/`) [PARTIALLY COMPLETED]
- [x] **Display List Interpreter**:
    - [x] Fetch-decode-execute display list instructions.
    - [x] Support for basic modes (mapped in code).
    - [x] Handle `DLI` (Display List Interrupt) bit in instructions.
    - [ ] Handle `HSCROL` and `VSCROL` smooth scrolling.
- [x] **DMA Timing**:
    - [x] "Steal" cycles from the CPU for playfield, sprite, and display list fetches.
    - [x] Implement the `HALT` signal line back to `ICore`.
- [x] **NMI Generation**: Vertical Blank Interrupt (VBI) and Display List 
      Interrupt (DLI).

### Phase 26.2: GTIA Color and PMG (`src/plugins/devices/gtia/`) [COMPLETED]

- [x] Implement `GTIA : public IOHandler`.
- [x] **Color Palette**:
    - [x] 256 colors (16 hues x 16 luminances).
    - [x] Map GTIA color registers ($D012–$D01A) to RGBA.
- [x] **Player-Missile Graphics (PMG)**:
    - [x] 4 Players and 4 Missiles (acting as a 5th player).
    - [x] Priority logic structure (ready for renderer).
    - [x] Collision detection registers ($D000–$D00F).
- [x] **Console Switches**: Read Start, Select, Option buttons via $D01F.

### Phase 26.3: POKEY Audio and IO (`src/plugins/devices/pokey/`) [COMPLETED]

- [x] Implement `POKEY : public IOHandler`.
- [x] **Audio Subsystem**:
    - [x] 4 independent square-wave channels.
    - [x] Polynomial-counter noise generators (4, 5, 9, 17-bit).
    - [x] Frequency division and 16-bit channel linking.
- [x] **Timers**: 3 interval timers with IRQ support.
- [x] **Keyboard**:
    - [x] Serial scan matrix for the Atari keyboard (skeleton).
    - [x] Handle BREAK and Control keys (skeleton).
- [x] **Serial Port (SIO)**: Support for SIO bus interrupts and basic data transfer (skeleton).

### Phase 26.4: Atari Machine Factory and Memory Map

- [ ] `MachineDescriptor` for `"atari800"`, `"atari800xl"`, and `"atari65xe"`.
- [ ] **PIA 6520**:
    - Port A: Joystick ports (4 for Atari 800, 2 for XL/XE).
    - Port B: XL/XE memory banking (OS ROM, BASIC ROM, RAM expansion).
- [ ] **Memory Map**:
    - ROMs: OS ($C000–$FFFF), BASIC ($A000–$BFFF).
    - I/O: ANTIC ($D400), GTIA ($D000), POKEY ($D200), PIA ($D300).
- [ ] **Video Renderer**: `IVideoOutput` combining ANTIC playfield and GTIA PMG 
      with priority and collisions.

### Phase 26.5: Atari Integration Tests

- [ ] **Display List Test**: Load a simple display list and verify ANTIC 
      addresses generated via `IBus` log.
- [ ] **PMG Priority Test**: Place a Player over a Playfield and verify RGBA 
      pixel priority matches GTIA settings.
- [ ] **POKEY Audio Test**: Program a channel and verify `pullSamples` returns 
      the expected waveform.
- [ ] **XL/XE Banking Test**: Toggle PIA Port B bit 0 and verify OS ROM 
      visibility at $C000.

---

## Phase 27: Atari 2600 (VCS)

*Goal: Ultra-lean scanline-based architecture requiring perfect timing.*

### Phase 27.1: TIA Video and Audio (`src/plugins/devices/tia/`)

- [ ] Implement `TIA : public IOHandler`.
- [ ] **Scanline Engine**:
    - No frame buffer; logic must render a single scanline pixel-by-pixel.
    - `WSYNC` logic: halt CPU until the end of the current scanline.
    - **Sprites**: Player 0, Player 1, Missile 0, Missile 1, Ball.
    - **Playfield**: 20-bit register-based playfield (mirrored or repeated).
    - Collision registers: Hardware-level pixel collision bit-latches.
- [ ] **Audio Subsystem**:
    - 2 independent channels (square/noise).
    - 5-bit frequency, 4-bit volume, 4-bit waveform.
- [ ] **Input**: Paddle and joystick button latching.

### Phase 27.2: RIOT 6532 (`src/plugins/devices/riot6532/`)

- [ ] Implement `RIOT6532 : public IOHandler`.
- [ ] **RAM**: 128 bytes of built-in RAM.
- [ ] **I/O Ports**: Dual 8-bit ports for joysticks and console switches.
- [ ] **Interval Timer**:
    - 8-bit counter with 1, 8, 64, or 1024 clock pulse intervals.
    - Interrupt support on underflow.

### Phase 27.3: Atari 2600 Machine Factory and Memory Map

- [ ] `MachineDescriptor` for `"vcs"`.
- [ ] **Memory Map**:
    - TIA: $00–$7F.
    - RIOT RAM: $80–$FF.
    - RIOT I/O/Timer: $280–$29F.
    - Cartridge: $F000–$FFFF (typical).
- [ ] **Banking**: Support for standard Atari bank-switching (F8, F6, etc.) 
      via `IBus` overlay.
- [ ] **Timing Model**: Cycle-exact `step()` loop; TIA must be updated every 
      3 CPU clock cycles (the 2600 has a 3:1 ratio).

### Phase 27.4: Atari 2600 Integration Tests

- [ ] **WSYNC Test**: Write to `WSYNC` and verify `cycles()` jump to next 
      scanline boundary.
- [ ] **Collision Test**: Set Player 0 and Player 1 at the same X-coord and 
      verify the P0-P1 collision bit is set.
- [ ] **RIOT Timer Test**: Set timer to 1024-count and verify it decrements 
      at the correct rate.
- [ ] **Playfield Render Test**: Program the playfield and verify the RGBA 
      buffer contains the expected pattern on a single scanline.

---

## Phase 28: Apple II Family

*Goal: Implementation of the classic Apple architecture and its unique Disk II.*

### Phase 28.1: Apple II Soft-Switches and Video (`src/plugins/devices/apple2_video/`)

- [ ] Implement `Apple2Video : public IVideoOutput, public IOHandler`.
- [ ] **Soft-Switches ($C000–$C0FF)**:
    - Many hardware functions are toggled via reads/writes to this range.
    - Video modes: Text, Lo-Res, Hi-Res.
    - Page 1/Page 2 display.
    - **Language Card**: ROM/RAM switch for $D000–$FFFF.
- [ ] **Keyboard**: Implement the latch at $C000 and strobe-clear at $C010.
- [ ] **Speaker**: Simple toggle at $C030; generate audible click via `IAudioOutput`.
- [ ] **Video Renderer**:
    - Apple II colors (NTSC artifacting model for Hi-Res).
    - Support for 40/80-column and Double-Hi-Res on IIe/IIc models.

### Phase 28.2: Disk II Controller (`src/plugins/devices/disk2/`)

- [ ] Implement `Disk2Controller : public IOHandler` at $C0E0–$C0EF.
- [ ] **State Machine**: Implement the 8-state stepper motor phases.
- [ ] **Drive Control**: SELECT, DRIVE, MOTOR ON, R/W.
- [ ] **Data Stream**:
    - Raw GCR-encoded bits from a virtual disk image (.dsk or .nib).
    - Shift-register emulation for read/write.
- [ ] **Disk Image Support**: Implement a simple `.dsk` to GCR converter.

### Phase 28.3: Apple IIe/IIc MMU (`src/plugins/devices/apple2e_mmu/`)

- [ ] Implement `Apple2eMMU : public IOHandler`.
- [ ] **Auxiliary RAM Banking**: Map a 64 KB auxiliary bank for 128 KB systems.
- [ ] Support for main/aux bank switches (80STORE, RAMRD, RAMWRT).
- [ ] **Alternative Character Set**: Support for the IIe/IIc character sets.

### Phase 28.4: Apple IIgs (16-bit) (`src/plugins/machines/apple2gs/`)

- [ ] `MachineDescriptor` for `"apple2gs"`.
- [ ] **CPU Slot**: `MOS65816` in native mode.
- [ ] **Ensoniq ES5503 DOC**:
    - 32 independent wavetable voices.
    - Dedicated audio RAM ($E10000–$E1003F).
- [ ] **Video Generation Chip (VGC)**:
    - Super Hi-Res 320x200 and 640x200.
    - Per-scanline palettes (16 colors per scanline).
- [ ] **Mega II**: Compatibility layer to provide the exact timing of a 
      IIe within the IIgs architecture.

### Phase 28.5: Apple II Integration Tests

- [ ] **Soft-Switch Test**: Toggle Hi-Res via $C057 and verify `IVideoOutput` 
      mode change.
- [ ] **Disk II Stepper Test**: Pulse the stepper phases and verify the 
      head position increments.
- [ ] **Language Card Test**: Switch $D000–$FFFF to RAM; write/read back; 
      switch back to ROM and verify data.
- [ ] **Speaker Click Test**: Rapidly toggle $C030 and verify `IAudioOutput` 
      samples.

---

## Phase 29: BBC Micro / Acorn Electron

*Goal: Fast, modular architecture with the unique Tube second-processor bus.*

### Phase 29.1: BBC Video Subsystem (`src/plugins/devices/bbc_video/`)

- [ ] Implement `BBCVideo : public IVideoOutput, public IOHandler`.
- [ ] **MC6845 CRTC**: Register-based timing generator at $FE00–$FE01.
- [ ] **Video ULA**:
    - Palette control (8 colors, including Mode 7 flashing).
    - Mode switching (Modes 0–6: 2/4/16-color tile/bitmap).
    - Clock speed switching for various modes.
- [ ] **SAA5050 Teletext**:
    - Implement the hardware character generator for Mode 7.
    - Support for Teletext attributes (double height, flash, alphanumeric/graphic).
- [ ] **Video RAM**: Dynamic mapping depending on mode (e.g., Mode 0–2 uses 20KB).

### Phase 29.2: System and User VIAs (`src/plugins/machines/bbc/`)

- [ ] Instantiate two `MOS6522` objects (Phase 10.1).
- [ ] **System VIA ($FE40)**:
    - Keyboard scanning (via Port A/B).
    - Sound latch control (SN76489).
    - Vertical sync interrupt.
- [ ] **User VIA ($FE60)**:
    - User port I/O.
    - Printer port.
    - ADC control.
- [ ] **SN76489 PSG**: 3-channel square wave + noise sound chip.

### Phase 29.3: The Tube Interface (`src/plugins/devices/bbc_tube/`)

- [ ] Implement `BBCTube : public IOHandler` at $FEE0–$FEE7.
- [ ] **Communication Protocol**: Implement the 8-bit bidirectional data 
      and status registers.
- [ ] **Second Processor Support**:
    - Connect a second `CpuSlot` (e.g., Z80, 6502, ARM) to the system via the Tube.
    - Handle bus synchronization and interrupts.

### Phase 29.4: BBC Machine Factory and Memory Map

- [ ] `MachineDescriptor` for `"bbcb"`, `"bbca"`, `"electron"`.
- [ ] **Paged ROM Mapping**:
    - 4-bit latch at $FE30 selects one of 16 ROM banks ($8000–$BFFF).
    - Implement `PagedMemoryBus` for this range.
- [ ] **Memory Map**:
    - RAM: $0000–$7FFF.
    - ROM/RAM Paged: $8000–$BFFF.
    - OS ROM: $C000–$FFFF.
    - I/O: $FC00–$FEFF.
- [ ] `onReset`: Load OS and Paged ROMs; reset VIAs and CRTC.

### Phase 29.5: BBC Integration Tests

- [ ] **Mode 7 Test**: Write Teletext codes to $7C00 and verify `renderFrame` 
      produces the correct SAA5050 glyphs.
- [ ] **VIA Latch Test**: Write to System VIA and verify SN76489 registers 
      are updated.
- [ ] **Paged ROM Test**: Toggle $FE30 and verify different ROM data 
      appears at $8000.
- [ ] **Tube Transfer Test**: Perform a simple status check/data write 
      to the Tube registers.

---

## Phase 30: NES / Famicom

*Goal: Tile-based rendering and the classic 5-channel sound.*

### Phase 30.1: Ricoh 2C02 PPU (`src/plugins/devices/ppu2c02/`)

- [ ] Implement `PPU2C02 : public IOHandler`.
- [ ] **Register Interface**: $2000–$2007 (Ctrl, Mask, Status, OAMAddr, 
      OAMData, Scroll, PPUAddr, PPUData).
- [ ] **Tile Pipeline**:
    - Background: Nametables, Attribute tables, Pattern tables.
    - Sprites: OAM (64 sprites, 8 per scanline limit).
    - Palette: 32 entries (BG/Sprite).
- [ ] **Scrolling**:
    - Implement "Loopy's" X/Y scroll latch and address logic.
    - Handle vertical and horizontal mirroring.
- [ ] **Interrupts**: VBlank NMI.

### Phase 30.2: Ricoh 2A03 APU (`src/plugins/devices/apu2a03/`)

- [ ] Implement `APU2A03 : public IOHandler`.
- [ ] **Sound Channels**:
    - 2 Pulse waves (4 duty cycles).
    - Triangle wave.
    - Noise generator.
    - **DMC (Delta Modulation Channel)**: Sample-based sound with DMA from RAM.
- [ ] **Frame Counter**: Internal 240Hz/192Hz clock for APU interrupts.

### Phase 30.3: NES Mappers (`src/libcore/nes_mappers.h/cpp`)

- [ ] Implement common mappers:
    - **NROM (0)**: Basic 16KB/32KB PRG, 8KB CHR.
    - **MMC1 (1)**: Serial bank switching for PRG and CHR.
    - **MMC3 (4)**: Scanline counter (via PPU A12 line) and fine banking.
- [ ] `NESMapper` base class with virtual `read/write` hooks for `IBus`.

### Phase 30.4: NES Machine Factory and Memory Map

- [ ] `MachineDescriptor` for `"nes"`, `"famicom"`.
- [ ] **Memory Map**:
    - RAM: $0000–$07FF.
    - PPU Registers: $2000–$2007.
    - APU/IO: $4000–$4017.
    - PRG ROM: $8000–$FFFF (mapped by Mapper).
- [ ] **Input**: Shift-register read for 8-button controllers.
- [ ] `onReset`: Reset CPU, PPU, APU, Mapper.

### Phase 30.5: NES Integration Tests

- [ ] **VBlank Test**: Run until VBlank and verify NMI fires.
- [ ] **PPU Register Test**: Write to `PPUADDR` and verify subsequent 
      `PPUDATA` read returns the correct memory byte.
- [ ] **Mapper MMC1 Test**: Perform the serial-write sequence to swap 
      PRG banks; verify data at $8000.
- [ ] **APU Tone Test**: Program a pulse channel and verify square wave 
      period in `pullSamples`.

---

## Phase 31: SNES / Super Famicom

*Goal: 16-bit 65816 core with complex tile modes and HDMA.*

### Phase 31.1: Ricoh 5A22 CPU (65816) (`src/libcore/cpu65816.h/cpp`)

- [ ] Implement `MOS65816 : public ICore`.
- [ ] **Native/Emulation Modes**:
    - Emulation: 6502-compatible ($0100–$01FF stack, 8-bit registers).
    - Native: 16-bit Accumulator (A/B), 16-bit X, Y, and SP.
- [ ] **24-bit Addressing**: Support for Bank ($K) and Direct ($D) registers.
- [ ] **Math Unit**: Support for hardware multiply/divide via $4200–$421F.
- [ ] **DMA/HDMA**:
    - 8 DMA channels for block transfers.
    - Horizontal-blank DMA (HDMA) for per-scanline register updates.

### Phase 31.2: SNES PPU1 and PPU2 (`src/plugins/devices/snes_ppus/`)

- [ ] Implement `SNES_PPUs : public IVideoOutput, public IOHandler`.
- [ ] **Tile Modes (0-7)**:
    - Mode 7: Affine transformations (matrix scaling, rotation, shearing).
    - Mosaic and Windowing effects.
- [ ] **Layering**: Up to 4 background layers with priority and transparency.
- [ ] **Color Math**: Master Brightness and color addition/subtraction.
- [ ] **Sprites (OBJ)**: 128 sprites, sizes from 8x8 up to 64x64.

### Phase 31.3: SNES APU (SPC700 + DSP) (`src/plugins/devices/snes_apu/`)

- [ ] Implement `SPC700` class inheriting from `ICore`.
- [ ] **Sony DSP**:
    - 8-channel ADPCM sample decoding.
    - Gaussian interpolation and Echo effects.
- [ ] **APU RAM**: 64 KB dedicated sound RAM.
- [ ] **Communication Ports ($2140–$2143)**: Four 8-bit ports for CPU-APU comms.

### Phase 31.4: SNES Machine Factory and Memory Map

- [ ] `MachineDescriptor` for `"snes"`.
- [ ] **Memory Map**:
    - WRAM: $7E0000–$7FFFFF (128 KB).
    - Registers: $2100–$43FF.
    - ROM (LoROM/HiROM): Banks $00–$3F and $80–$BF.
- [ ] **Input**: Support for SNES joypads (16-bit serial read).
- [ ] `onReset`: Reset 65816, PPU, APU.

### Phase 31.5: SNES Integration Tests

- [ ] **Native Mode Transition**: Execute `CLC; XCE` and verify 16-bit register 
      writes.
- [ ] **DMA Transfer Test**: Configure a DMA channel to copy from ROM to 
      WRAM; verify data.
- [ ] **PPU Mode 7 Test**: Enable Mode 7 and verify `renderFrame` applies 
      matrix transforms to background tiles.
- [ ] **APU Port Test**: Write to $2140 and verify `SPC700` reads the byte 
      from its local register view.

---

## Phase 32: Commander X16

*Goal: Modern 65C02-based machine with VERA graphics and YM2151 sound.*

### Phase 32.1: VERA (Video Embedded Retro Adapter) (`src/plugins/devices/vera/`)

- [ ] Implement `VERA : public IVideoOutput, public IOHandler`.
- [ ] **Register Interface**: $9F20–$9F2F.
- [ ] **Dual-Port VRAM**: 128 KB of internal video RAM accessible via 
      auto-incrementing data ports.
- [ ] **Video Layers**:
    - Two independent layers supporting tile, bitmap, and character modes.
    - 128 hardware sprites (up to 64x64, 4/8-bpp).
- [ ] **Palette**: 256 entries (12-bit RGB).
- [ ] **Audio Subsystem**:
    - PCM playback: 16-bit mono/stereo samples from VRAM.
    - PSG: 16 channels of programmable waveforms (pulse, sawtooth, noise).
- [ ] **SPI Controller**: Interaction with SD card via VERA registers.

### Phase 32.2: YM2151 OPM Sound (`src/plugins/devices/ym2151/`)

- [ ] Implement `YM2151 : public IOHandler` at $9F40.
- [ ] **FM Synthesis**:
    - 8 independent voices.
    - 4 operators per voice with various algorithms.
    - Precise envelope and LFO modulation.
- [ ] **Audio Output**: 2 channels (Stereo).

### Phase 32.3: X16 Machine Factory and Memory Map

- [ ] `MachineDescriptor` for `"x16"`.
- [ ] **CPU Slot**: `W65C02` (8-bit CMOS version).
- [ ] **Banking**:
    - Banked RAM: $A000–$BFFF (512 KB to 2 MB).
    - Banked ROM: $C000–$FFFF (512 KB).
    - Controlled by registers at $0000/$0001 (VIA1-based mapping).
- [ ] **Input**: PS/2 keyboard and mouse via VIA1/VIA2.
- [ ] `onReset`: Load KERNAL and BASIC ROMs; reset VERA and YM2151.

### Phase 32.4: X16 Integration Tests

- [ ] **VERA VRAM Test**: Write to VRAM via `DATA0` and verify address 
      auto-increment logic.
- [ ] **Banked RAM Test**: Switch between RAM banks 0 and 1; verify data 
      isolation at $A000.
- [ ] **YM2151 Write Test**: Write to an FM register and verify `pullSamples` 
      contains FM-modulated output.
- [ ] **VERA Sprite Test**: Enable a sprite and verify `renderFrame` RGBA 
      buffer includes the sprite data.

---

## Cross-Cutting GUI Standards

Applied to all new panes in Phase 8.

- [x] **Fixed-width fonts** for all hex and register values.
- [x] **Consistent colour palette** for change highlights (e.g., light red background for changed values).
- [x] **Side-effect-free reads** (via `peek8`) for all UI rendering.
- [x] **Asynchronous refresh**: UI updates at 30-60 Hz, not on every instruction step (too slow).
- [x] **DPI Awareness**: All panes and icons must scale correctly on High-DPI displays (macOS Retina, 4K Windows).
- [x] **Device Inspectable**: Every `IOHandler` provides a `ChipRegDescriptor` table for the Device Inspector pane.

---

## Phase 33: Device Info Breakdown & Visual Inspection [COMPLETED]

*Goal: Provide deep introspection into every emulated device, including internal 
register state, I/O line levels, and visual assets like sprites or tile maps.*

### Phase 33.1: Device Info API and Infrastructure [COMPLETED]

- [x] Define `DeviceInfo`, `DeviceRegister`, and `DeviceBitmap` structures in `libdevices`.
- [x] Add `virtual void getDeviceInfo(DeviceInfo& out) const` to `IOHandler`.
- [x] Implement base reporting (name, address) in `IOHandler` default.

### Phase 33.2: Per-Chip Breakdown Implementation [COMPLETED]

- [x] **VIC-II**: Full register dump, raster/cycle status, and individual sprite 
      extraction (8 bitmaps).
- [x] **SID 6581**: All 29 registers, filter settings, and per-voice envelope phases.
- [x] **CIA 6526 / VIA 6522**: Port latches, DDRs, timer counters, and IRQ status.
- [x] **Virtual IEC**: Bus state machine phase and ATN/CLK/DATA line levels.
- [x] **Datasette**: Tape position, motor/sense status, and I/O pulse lines.

### Phase 33.3: Multi-Frontend Inspection [COMPLETED]

- [x] **CLI**: `<device>.info` command with fuzzy name matching.
- [x] **MCP**: `list_devices` and `get_device_info` tools returning JSON breakdowns.
- [x] **GUI**: `DeviceInfoPane` featuring:
    - Device selector dropdown.
    - Hierarchical tree view for registers and state.
    - **Visual Inspector**: Scrolled window rendering zoomed-in bitmaps (e.g. sprites) 
      on a grid layout.
- [x] **Layout Optimization**: Move Register window beneath Memory; CLI at 
      bottom-right; expand Tool Notebook for maximum visibility.

### Phase 33.4: Device editing

- [ ] ***GUI**: On the DeviceInfoPane hierarchical view, allow 
      a) adding, b) editing and c) removign a device. 
