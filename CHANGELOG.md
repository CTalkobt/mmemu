# CHANGELOG.md - mmsim (Multi-Machine Simulator)

All notable changes to this project will be documented in this file.

## [0.3.2] - 2026-05-28

### Added
- **CBM Disk Image Format Support** (`src/plugins/cbm-loader/`):
    - D71 (1571 double-sided, 70 tracks, 349,696 bytes)
    - D80 (8050, 77 tracks, 533,248 bytes)
    - D81 (1581, 80 tracks, 819,200 bytes — used by MEGA65)
    - D82 (8250 double-sided, 154 tracks, 1,066,496 bytes)
    - All formats support error-info variants (appended per-sector error bytes)
    - `CbmSectorDisk` base class with geometry-driven shared logic for directory parsing, file chain reading, disk name/ID extraction, and BAM-based free block counting
    - `DiskImageLoader` now dispatches `.d71`, `.d80`, `.d81`, `.d82` extensions
    - 10 new unit tests: format open/read, geometry validation, size rejection, extension dispatch
- **MCP `diff_file` Tool** (`src/mcp/main/main.cpp`):
    - Compare two binary files byte-by-byte
    - Summary: file sizes, changed byte count/percentage, region count
    - 6502 vector table comparison (NMI, RESET, IRQ) with change detection
    - Per-region hex dump with configurable context bytes
    - Optional symbol annotations via `machine_id` (nearest label + offset)
- **MCP Snapshot Tools** (`src/mcp/main/main.cpp`):
    - `snapshot_save`: Capture named snapshot of CPU registers and memory (configurable range)
    - `snapshot_diff`: Compare two snapshots — shows register changes and memory diff regions with symbol annotations
    - `snapshot_list`: List all saved snapshots for a machine instance
    - `snapshot_delete`: Remove a snapshot (or all with `"*"`)
- **MCP `analyze_routine` Tool** (`src/mcp/main/main.cpp`):
    - Control-flow walking disassembler from an entry point
    - Follows branches (both paths), unconditional jumps; stops at RTS/RTI/BRK
    - Detects loops (backward branches), subroutine calls (JSR with dedup + count)
    - Identifies I/O accesses ($D000-$DFFF) as read/write with symbol annotations
    - Reports indirect jumps (JMP ($xxxx)) as unresolvable exit points
    - `recursive` mode: follows into JSR targets to analyze subroutine internals
    - Reports max call depth (deepest JSR nesting level)
    - Configurable max_instructions limit (default 200, max 5000)
- **MCP `generate_tests` Tool** (`src/mcp/main/main.cpp`):
    - Automated test vector generation for 6502 routines
    - Runs a routine with all combinations of input register values
    - Captures output registers after routine returns (RTS/RTI/BRK detection via call-depth tracking)
    - Saves/restores full machine state between test runs
    - Configurable: input/output registers, test values, max steps per run
    - Handles nested JSR calls (tracks call depth, only stops at outermost return)
    - Outputs formatted table with input→output mappings, step count, completion status
    - Tool count: 51 → 58

### Fixed
- **D64 getDiskName()**: Read disk name from wrong sector (18/1 directory instead of 18/0 BAM). Fixed to use geometry-defined header sector.
- **D64 getFreeBlocks()**: BAM entry offset was off by 4 bytes (read from sector start instead of skipping the 4-byte header). Fixed via configurable `bamEntryOffset` in geometry.
- **D64 readFile()**: Removed leftover `std::cerr` debug statements.

## [0.3.0] - 2026-05-19

### Added
- **VIC-III (CSG 4567) Implementation** (`src/plugins/devices/vic3/`):
    - 256-color palette ($D100-$D3FF: R, G, B channels)
    - $D030 banking register (CRAM2K, EXTSYNC, PAL, ROM8/A/C/E, CROM9)
    - $D031 mode control (H640, FAST, ATTR, BPM, V400, H1280, MONO, INT)
    - 80-column text mode (H640 bit) with half-width character cells
    - Bitplane mode (BPM) with up to 8 planes, complement flags, X/Y offsets
    - Extended attributes (ATTR) with 8-bit colour register access
    - Bitplane enable ($D032), addresses ($D033-$D03A), DAT ports ($D040-$D047)
    - Personality lock: C64 mode delegates to VIC-II, C65 mode uses VIC-III
    - Plugin registered as device type "4567"
- **VIC-IV Full Colour Mode (FCM)**:
    - 8×8 character cells with 64 bytes of pixel data (one byte per pixel = palette index)
    - FCLRLO ($D054.1): FCM for character numbers ≤ $FF
    - FCLRHI ($D054.2): FCM for character numbers > $FF
    - CHR16 ($D054.0): 16-bit character numbers from screen RAM
    - Pixel value $FF selects foreground colour from colour RAM
    - $D054 bit constants (CHR16, FCLRLO, FCLRHI, SPRH640, VFAST, PALEMU, ALPHEN)
- **VIC-IV Nibble-Colour Mode (NCM)**:
    - 4 bits per pixel, 16-pixel-wide characters (2 pixels per byte)
    - Lower 4 bits from character data nibble, upper 4 from colour RAM
    - Nibble $F selects full colour RAM value as palette index; $0 = background
    - Per-character selection via colour RAM bit 3 when CHR16 enabled
    - Shares 64-byte character data format with FCM
- **VIC-IV Variable Display Geometry**:
    - $D05E (CHRCOUNT): configurable characters per row (0 = default 40/80)
    - $D07B (DISPROWS): configurable text rows (0 = default 25)
    - $D058-$D059 (LINESTEP): bytes between screen RAM rows (16-bit, 0 = auto)
    - Enables non-contiguous screen RAM layouts
- **VIC-IV Sprite Extensions**:
    - Extended height ($D055 SPRHGTEN per-sprite, $D056 SPRHGHT 0-255px)
    - 16-colour sprites ($D06B SPR16EN): 4 bits/pixel, color = sprite# × 16 + nibble, nibble 0 = transparent
    - 64-pixel wide mode ($D057 SPRX64EN): 8 bytes per row
    - Sprite pointer relocation ($D06C-$D06E SPRPTRADR, 24-bit base)
    - 16-bit sprite pointers ($D06E.7 SPRPTR16)
    - Sprite Y MSBs ($D077 SPRYMSBS): 9-bit Y positioning
- **VIC-IV Display Geometry Registers**:
    - $D048-$D049 (TBDRPOS): top border position (12-bit)
    - $D04A-$D04B (BBDRPOS): bottom border position (12-bit)
    - $D04C-$D04D (TEXTXPOS): text X position (12-bit)
    - $D04E-$D04F (TEXTYPOS): text Y position (12-bit)
    - $D05C-$D05D (SDBDRWD): side border width
- **VIC-IV Palette Banks** ($D070):
    - SPRPALSEL, BTPALSEL, ABTPALSEL, MAPEDPAL (2 bits each)
- **VIC-IV System Flags**:
    - $D054.5 (VFAST): 40 MHz CPU mode
    - $D06F.7 (PALNTSC): PAL/NTSC timing selection
- **VIC-IV 16-Colour Bitplanes** ($D071 BP16ENS):
    - Pairs of bitplanes form 4-bit colour nibbles instead of individual bits
- **MEGA65 Machine Factory**: VIC4 now receives char ROM and colour RAM wiring, fixing C64-mode text rendering
- **MEGA65 Boot Support**: Machine now boots to C64 KERNAL reset vector ($E4B8) and executes MEGA65 KEY unlock sequence, VIC-III banking, and INTERFACE ROM transition
- **Hypervisor Mode** (`src/plugins/45gs02/main/cpu45gs02.cpp`):
    - `enterHypervisor(trapAddr)`: saves full CPU state (A/X/Y/Z/B/SP/P/PC) and MAP state into virtualisation control registers, switches to hypervisor mode with stack at $BE00 and base page at $BF00
    - `exitHypervisor()`: restores saved CPU and MAP state, returns to user mode
    - On reset with HYPPO ROM present, CPU enters hypervisor at $8100 instead of reading the standard reset vector
    - Hypervisor mode maps $8000-$BFFF to HYPPO ROM (reads from ROM, writes ignored)
- **Hypervisor Virtualisation Registers** (`src/plugins/devices/hypervisor/main/hypervisor_regs.cpp`):
    - New `HypervisorRegs` IOHandler at $D640-$D67F (64 bytes)
    - In user mode: writes trigger SYSCALL traps (entry point = $8000 + offset*4)
    - In hypervisor mode: read/write access to saved CPU state (A, X, Z, B, SP, P, PC, MAP state, port $00/$01, VIC mode)
    - $D67F write (ENTEREXIT) exits hypervisor mode
- **HYPPO ROM Loading** (`src/plugins/machines/mega65/main/machine_mega65.cpp`):
    - Searches for 16KB `HICKUP.M65` in `roms/mega65/`, current directory, and `~/.local/share/xemu-lgb/mega65/`
    - Falls back to standard C64 reset vector if not found
- **F018B DMA `getDeviceInfo()`**: Register descriptions (ADDRLSB, ADDRMSB, ADDRBANK, EXECUTE, ADDRMB, ETRIGGER) and runtime state (list address, active flag, enhanced mode, job count)

### Fixed
- **MEGA65 ROM offset mapping**: C64 compatibility regions (BASIC, KERNAL, Char ROM) were loaded from wrong offsets in the 128KB ROM file. Corrected per MEGA65 Book Appendix J (C65 Compatibility ROM Layout): BASIC at file $A000, KERNAL at $E000, Char ROM at $D000.
- **MEGA65 onReset callback**: Machine factory did not set `onReset`, so the CPU was never reset after bus wiring. PC stayed at $0000 instead of reading the reset vector. Added reset callback matching JSON loader pattern.
- **MCP server onReset**: `createMachineInstance()` did not call `onReset` after setup, leaving newly created machines with PC=$0000. Now calls `onReset` after wiring, matching CLI behavior.
- **F018B DMA overlapping copy**: `doCopy()` now detects overlapping source/destination regions and copies backward when destination falls within the source range, preventing data corruption during self-overlapping DMA transfers.

### Changed
- **VIC-IV now extends VIC-III** (`VIC2 → VIC3 → VIC4`): Palette, personality lock, and VIC-III register handling moved from VIC4 into VIC3. VIC4 retains only VIC-IV specific registers ($D048-$D07F), 32KB internal colour RAM, and MEGA65-specific rendering. Net reduction of 133 lines.

### Fixed
- **VIC-IV register address mapping**: Screen RAM pointer was incorrectly mapped to $D04C-$D04F (TEXTXPOS). Fixed to $D060-$D063 (SCRNPTR, 28-bit). Character set pointer fixed from $D050-$D053 to $D068-$D06A (CHARPTR, 24-bit). Colour RAM pointer added at $D064-$D065 (COLPTR).

## [0.2.1] - 2026-05-18

### Added
- **Code Coverage**: `make coverage` target using gcov instrumentation with optional lcov HTML reports.
- **175 New Unit Tests** (309 → 484 total):
    - `test_stack_trace`: Push/pop, recent ordering, clear, type name strings.
    - `test_breakpoint_list`: Exec/read/write watchpoints, enable/disable, conditions, hit counts.
    - `test_source_map`: KickAssembler list parsing, address lookup, missing files, malformed input.
    - `test_tap_parser`: TAP load/save, normal and long pulse decoding, recording roundtrip.
    - `test_mega65_math`: Multiply, divide, divide-by-zero, RNG advancement, reset, out-of-range.
    - `test_datasette`: Mount, play/stop/rewind, recording, signal line wiring, end-of-tape.
    - `test_crtc6545`: Register select/write, read-only registers, counter advancement, reset.
    - `test_sim_config`: Missing config defaults.
    - `test_expression_evaluator`: Condition evaluation, char escapes, error paths (91% → 98%).
    - `test_toolchain`: Named assemblers, getAssemblerNames, resolveAssembler 3-level precedence (53% → 98%).
    - `test_cli`: Registers, memory dump/fill/copy/swap, step, disasm, breakpoints, watchpoints, symbols, search, stack, save, config, inline assemble-execute, no-machine error paths (22% → 61%).
    - `test_cpu45gs02`: Register access (inc. Q), reset, LDA/LDX/LDY/LDZ, stores, transfers, INC/DEC, JMP, JSR/RTS, flags, ADC/SBC, PHA/PLA (10% → 16%).
    - `test_vic2_unit`: Registers, raster counter/IRQ, rendering (all 5 video modes), sprites (Y-expansion), banking, char ROM shadow, device info, logger (48% → 99.7%).
    - `test_vic3`: Registers, palette, locked/unlocked state, 80-col rendering, bitplane rendering, complement flags, DAT ports, ATTR mode, VIC-II backward compat (new, 98.9%).
    - `test_vic4`: Locked/unlocked rendering, tick, register accessors, FCM basic/CHR16/$FF substitution/FCLRLO selection (new + expanded).
    - `test_virtual_iec_unit`: Initial state, readPort, writePort ATN, reset, mount/eject/status, LED activity, device info (14% → 24%).

### Fixed
- **ImportResult ODR violation**: `vice_importer` and `mega65_importer` both defined `struct ImportResult` at global scope with different member layouts (swapped `string`/`vector` fields). This caused a double-free crash when building at `-O0`. Fixed by moving each struct into its respective namespace.

### Changed
- `make clean` now removes coverage artifacts (`.gcno`, `.gcda`, `coverage/`).

## [0.2.0] - 2026-05-14

### Added
- **MEGA65 Phase 21 Completion**:
    - **Machine Factory**: Implemented `Mega65MachineFactory` for the full "mega65" machine preset.
    - **ROM Loading**: Integrated automatic loading of `mega65.rom` (128KB) into physical Banks 2-3 with write protection.
    - **Device Registration**: Wired and registered all core MEGA65 devices: F018B DMA, Math Accelerator, Hyper Serial, and Exit Trap.
    - **Integration Testing**: Added `test_mega65_integration.cpp` verifying 28-bit bus wiring, MMU translation, and I/O personality switching.
    - **MEGA65 Phase 24 (ROM Importer)**:
        - New plugin: `lib/mmemu-plugin-mega65-importer.so`.
        - Automatically discovers MEGA65 ROMs in standard `xemu-xmega65` installation paths across Linux, macOS, and Windows.
        - Added "Manual Selection..." in the GUI to allow users to browse for their `MEGA65.ROM` file.
        - CLI command `importmega65` and MCP tool `import_mega65_roms` for scriptable setup.
        - Namespaced existing `viceImporter` to prevent symbol collisions during static linking in tests.
    - **Build System Refactor**:
 Moved plugin entry points (`mmemuPluginInit`) to separate `plugin_init.cpp` files for Exit Trap, DMA, and Math Unit to prevent linker collisions in test binaries.
- **45GS02 CPU Enhancements**:
    - **Quad immediate modes**: LDQ/ADCQ/SBCQ/CMPQ/ANDQ/ORAQ/EORQ #imm32 now execute (previously halted).
    - **Decimal mode (BCD)**: ADC/SBC now handle FLAG_D correctly via doAdc8/doSbc8 helpers.
    - **Full disassembly**: All opcodes including quad prefix (#imm32 shown as 8-digit hex).
- **28-bit Symbol Table Display**: CLI and GUI adapt address width based on bus (4 digits for 16-bit, 7 for 28-bit) across sym list, memory dump, disasm, and search results.
- **Build System**:
    - Added `make cppcheck` target for static analysis of the codebase.
    - Fixed several One Definition Rule (ODR) violations in test suites by using anonymous namespaces for mock classes.
- **Version string**: Build system generates `version.h` with git hash (displayed as `0.2.0-<hash>`).

### Changed
- Suppressed "Registering machine:" and plugin load messages to spdlog debug level.
- Refactored all inline 8-bit ADC/SBC to shared doAdc8/doSbc8 helpers (decimal-aware).

### Fixed
- MAP instruction enable bit encoding (was swapped between lower/upper halves).
- EOM incorrectly clearing MAP state.
- GUI plugin pane manager segfault on window destroy in test environment.

## [0.8.1-dev] - 2026-05-09

### Added
- **Pluggable Assembler Infrastructure**:
    - **SimConfig** class loads tool configuration from `config.json` (with search in standard paths)
    - **ToolchainRegistry** extended with name-based assembler registration
    - **resolveAssembler()** function with 3-level precedence: runtime override → machine-preferred → ISA-default
    - **MachineDescriptor** now includes `preferredAssembler` field (loaded from machine JSON)
- **CA45 Assembler Backend**:
    - Integrated CA45 assembler for 45GS02/MEGA65 as pluggable backend
    - Auto-configured via SimConfig tool settings
    - Registered in 45GS02 plugin
- **MCP Server Assembler Tools**:
    - `set_assembler` — Override assembler for a specific machine at runtime
    - `get_assembler` — Query current assembler for a machine
    - Enhanced `asm` tool with fallback from line-by-line to file-based assembly modes
- **MCP Multi-Instance Machine Management**:
    - `list_instances` — List all running machine instances with their type and display name
    - `destroy_machine` — Close and release a machine instance by instance ID
- **Previous MCP Enhancements** (from earlier 0.8.1 work):
    - Code generation tools with error diagnostics
    - Search navigation tools for memory pattern navigation
    - MEGA65-specific address translation and I/O personality control
    - Trace buffer integration

### Changed
- **Multi-Instance Machine Support**: `create_machine` refactored for independent instances
  - `machine_id` parameter renamed to `machine_type` (the registry type: "c64", "rawMega65", etc.)
  - `machine_id` now an optional instance name; auto-generated as `<type>_<n>` if omitted
  - Enables multiple independent instances of the same machine type (e.g., "c64_1", "c64_2")
- **Assembler Handling**: Moved from ISA-only default to configurable per-machine selection
- **MCP Tool Count**: 40 → 53 → 55 tools (multi-instance support adds 2 new tools)

### Technical Details
- **SimConfig**: Searches `./config.json` then standard data paths for tool configuration
- **ToolchainRegistry**: Now supports both ISA-based and name-based assembler factories
- **MachineState (MCP)**: Added `IAssembler* assem` field; global `g_assemblerOverrides` map for per-machine runtime selection

## [0.8.0-dev] - 2026-05-07

### Added
- **Phase 19: Sparse Memory and Memory Management Unit (MEGA65)**:
    - **SparseMemoryBus**: 28-bit address space with 4KB lazy-allocated pages for efficient MEGA65 memory management.
    - Pre-mapped ROM regions for KERNAL, BASIC, and Character ROM with configurable overlays.
    - Snapshot serialization support for sparse memory format.
    - **MapMmu**: Pure address translator implementing MEGA65 memory mapping (MAP instruction support).
    - Full support for memory region translation with snapshot persistence.
    - Unit tests validating both `SparseMemoryBus` and `MapMmu` functionality.
- **Phase 21: MEGA65 Machine Factory (In Progress)**:
    - Wire `MapMmu` as CPU's bus pointer for transparent address translation.
    - Integration with 45GS02 CPU for MAP instruction support.
    - Foundation for ROM loading and I/O personality switching.

### Changed
- **Memory Architecture**: Moved from flat 16-bit memory model to sparse 28-bit addressing for MEGA65 support.

## [0.7.0-dev] - 2026-04-25

### Added
- **Installation Infrastructure**:
    - New `install-local.sh` and `install-system.sh` scripts in `tools/` for easy deployment to `~/.local` or `/usr/local`.
    - **Man Page Generation**: Added a `make man` target using `pandoc` to convert Markdown documentation into system man pages (installed to Section 1 for binaries and Section 7 for plugins/concepts).
- **Resource Discovery System (`PathUtil`)**:
    - Centralized path resolution for plugins, machine configurations, and ROM images.
    - Automatic search in standard locations: `./`, `~/.local/share/mmsim/`, and `/usr/local/share/mmsim/`.
    - Plugins are now automatically discovered in `~/.local/lib/mmsim/plugins` and `/usr/local/lib/mmsim/plugins`.
- **45GS02 CPU Implementation** (Phase 18):
    - Initial implementation of the MEGA65 45GS02 CPU core in `src/plugins/45gs02/`.
    - Support for 65CE02 extensions: `TAZ`, `TZA`, `TAB`, `TBA`, `PHZ`, `PLZ`, `INW`, `DEW`, `ASW`, `ROW`, `PHW`, `BSR`, `RTN`.
    - Support for MEGA65 specifics: `NEG`, `MAP`, `EOM`.
    - Implemented **Quad (Q) pseudo-register** (A+X+Y+Z) and associated 32-bit operations:
        - `LDQ`, `STQ` (Zero Page, Absolute, and 32-bit Indirect addressing).
        - `ADCQ`, `SBCQ`, `CMPQ`, `ANDQ`, `ORQ`, `EORQ`.
        - `ASLQ`, `LSRQ`, `ROLQ`, `RORQ`.
    - Enhanced disassembler with support for all Quad instructions.
    - Integrated `translate()` logic for memory mapping (4-slot MMU).
    - Support for the `B` (Base Page) register in zero-page addressing modes.
    - Hypervisor exit trap support (triggered at `$D6CF`).
- **45GS02 Validation Suite**:
    - Automated cross-validation of the `mmsim` 45GS02 core against the reference `xemu-xmega65` emulator.
    - Test cases for arithmetic, transfers, and advanced CPU features.
    - **Note**: Running these tests may trigger a GUI dialog from `xemu-xmega65` even in headless mode on some systems.
- **IEC Bus Enhancements**:
    - Added comprehensive tests for IEC serial bus communication and `.d64` disk image handling.
- **`rawMega65` Machine Preset**:
    - A minimal machine target for testing the 45GS02 CPU without full MEGA65 chipset complexity.

### Changed
- **Pathing**: Updated `xmega65` path configuration in validation scripts to be less user-specific, improving portability.

### Fixed
- **CLI Cleanup**: Minor code cleanups in `cli_interpreter.cpp` to use modern C++17 `if` with initializer and removed redundant code.

## [0.6.0-dev] - 2026-04-11

### Added
- **Engineering Calculator** (GUI):
    - Pop-up modal calculator accessible via **Tools > Calculator** (**Ctrl-Shift-C**).
    - **28-Button Dynamic Layout**: Key labels and functions adapt automatically based on the selected numerical base.
    - Supports **Floating Point** and **Integer** modes.
    - Supports **Hex**, **Decimal**, **Binary**, and **Octal** bases with automatic formatting.
    - Features **8 persistent memory slots** (`Rcl0`-`Rcl7`) and an `Ans` button for result recall.
    - **Direct Keyboard Entry**: The display field is editable, allowing manual typing of complex expressions.
    - Comprehensive function support: `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `sqrt`, `log`, `exp`, `abs`, `^`, `<<`, `>>`, and bitwise complements.
- **Enhanced Expression Evaluator**:
    - Refactored `ExpressionEvaluator` to use `double` precision for all internal math.
    - Added support for bitwise shift operators (`<<`, `>>`).
    - Added support for power operator (`^`).
    - Integrated library of math functions: `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `sqrt`, `log` (log10), `exp`, `abs`, and `between(n, val, delta)`.
    - Added `defaultBase` support to allow parsing literals without prefixes (e.g., `ffd2` in hex mode).

### Fixed
- **Expression Parsing**: Resolved issue where hex literals like `ffd2` were rejected in Hex mode because they were interpreted as invalid decimals. The evaluator now respects the context-provided default base.
- **UI Layout**: Fixed status line overflow in the calculator dialog by expanding the minimum width and optimizing sizer alignment.

## [0.5.0-dev] - 2026-04-09

### Added
- **Virtual IEC Bus Device** (`src/plugins/devices/virtual_iec/`):
    - `VirtualIECBus` class implementing Level 2 (protocol-level) IEC serial bus emulation.
    - State machine: `IDLE → ATTENTION → ADDRESSING → ACKNOWLEDGE` with correct ATN/CLK/DATA open-collector semantics.
    - Implements `IPortDevice` for direct attachment to CIA #2 Port A (C64) or VIA #2 (VIC-20).
    - `IOHandler` interface satisfied: `tick()`, `reset()`, `mountDisk()`, `ejectDisk()`.
    - Plugin entry point (`plugin_init.cpp`) and `.so` build target (`mmemu-plugin-virtual-iec.so`).
    - JSON machine-loader wiring via new `iecWiring` section in machine specs.
    - Unit test `virtual_iec_handshake` verifying ATN detection and DATA acknowledge within a single tick.
- **`IOHandler` base class**: Added default `mountDisk(int, const std::string&)` and `ejectDisk(int)` virtual methods so plugins exposing disk management don't need to inherit a separate interface.
- **Build system**: `virtual_iec.o` added to `ALL_PLUGIN_OBJS`; `test_virtual_iec.cpp` added to `TEST_SRCS`; test count grows to 162.

### Fixed
- **`VirtualIECBus::tick()` timer reset bug**: Transitioning IDLE→ATTENTION previously reset `m_stateTimer` *after* the tick's cycles had been accumulated, making it impossible for the ATTENTION threshold to be satisfied in the same tick as ATN detection. Fixed by preserving the accumulated cycle count across the transition and processing up to two state changes per tick.
- **Stale-artifact vtable crash**: Adding virtual methods to `IOHandler` without a full rebuild caused a vtable layout mismatch between the statically-linked test binary and the dynamically-loaded `.so` plugins, crashing `c64_tape_load` with `basic_string: construction from null`. Resolved by ensuring all artifacts are rebuilt consistently.

## [0.4.0-dev] - 2026-04-08

### Added
- **Advanced Expression Evaluator**:
    - Support for high byte (`<`) and low byte (`>`) unary operators.
    - Support for logical negation (`!`).
    - Support for multiplication (`*`), division (`/`), and modulus (`%`).
    - Support for comparison operators (`==`, `!=`, `<`, `>`, `<=`, `>=`).
    - Support for parentheses `( )` to control evaluation order and precedence.
    - Support for character literals (e.g., `'a'`, `'\n'`) returning ASCII values.
    - Disambiguation between modulus `%` and binary literal prefix.
- **Enhanced Debugger GUI**:
    - **DisasmPane Context Menu**: Added a comprehensive right-click menu for breakpoint management, navigation (Go to Address/PC, Follow Operand, Set PC, Go up stack frame), and clipboard operations (Copy Address/Disassembly).
    - **Conditional Breakpoints**: Integrated condition support into the Breakpoint Pane, allowing users to set and edit expressions that must be non-zero to trigger a break.
    - **Breakpoint Management**: Added right-click menu to Breakpoint Pane for toggling, editing conditions, and clearing hit counts.
    - **Memory Sync**: "Show address in memory pane" feature to instantly jump the primary memory view to a specific address from the disassembler.
- **Improved Symbol Support**:
    - Enhanced address-entry dialogs and `Ctrl-G` to resolve machine symbols and complex expressions (e.g., `CHROUT`, `start + 5`).

### Changed
- **UI Refactoring**: Replaced magic numbers in GUI event handlers with structured enums for better maintainability.
- **Expression Parsing**: Enforced strict rules for literal prefixes (`$`, `%`) to prevent ambiguity with whitespace.

## [0.3.0-dev] - 2026-04-07

### Added
- **Enhanced Symbol Table Management**:
    - CLI: Added `sym` command with `add`, `del`, `list`, `search`, `load`, and `clear` subcommands.
    - GUI: Integrated a new **Symbols** pane with real-time search, manual management, and double-click navigation.
    - MCP: Added `list_symbols`, `add_symbol`, `remove_symbol`, `load_symbols`, and `clear_symbols` tools.
    - Support for multiple `.sym` file formats (`label = value` or `label value`).
- **Core Expression Evaluator**:
    - Implemented a robust recursive-descent parser for addresses and values.
    - Supported formats: Hex (`$prefix` or `0x`), Binary (`%prefix`), and Decimal.
    - Integrated support for using symbols and CPU registers (e.g., `PC + 5`, `start_addr`) in expressions.
    - Integrated the evaluator into all address-taking CLI commands, GUI dialogs, and MCP tools.
- **Kernal Symbols & Monitoring**:
    - Added standard KERNAL symbol tables for C64, VIC-20, and PET machines.
    - Implemented auto-loading of these symbols upon machine creation via the new `symbolFiles` machine configuration field.
    - Added **Kernal Routine Monitoring**: Setting `log level kernal debug` now logs register states upon entry to Kernal routines and displays result states upon matching `RTS` exit.
- **Testing Expansion**:
    - Added 5 new test suites (totaling 153 passing tests) covering Expression Evaluation, Symbol Management, Kernal Autoloading, and C64 I/O logic.

### Fixed
- **Build System**: Fixed several Makefile syntax errors and redundant variable definitions.
- **Datasette/Tape Pane**: Fixed "No Datasette found" error by ensuring `Datasette` and other plugins correctly implement `setName()` and `name()` to match machine configurations.
- **C64 Logic**: Repaired severely broken bit-manipulation logic in the 6510 I/O port (`cpu6510.cpp`).
- **Stability**: Added null checks and move semantics to MCP machine management to prevent crashes and double-free errors.

## [0.2.0-dev] - 2026-04-07 (2)

### Added
- **CLI/MCP parity with GUI memory operations**:
    - CLI: Added `swap <addr1> <addr2> <len>` command to swap two equal-length memory regions.
    - CLI: Added `findnext` / `findprior` commands to navigate forward and backward through occurrences of the last `search`/`searcha` pattern (wraps around the full address space).
    - CLI: `search` and `searcha` now report **all** matches, save state for `findnext`/`findprior`, and use the bus address mask instead of the hardcoded `0x10000` range.
    - MCP: Added `swap_memory(machine_id, addr1, addr2, size)` tool, matching the GUI Swap Memory operation.

---

## [0.2.0-dev] - 2026-04-07

### Added
- **MemoryPane Enhancements**:
    - Added a context menu to `MemoryPane` for quick access to Fill, Copy, Swap, Go to Address, and Search Memory.
    - Implemented **Swap Memory** functionality (`ID_SWAP_MEM`) with a new `SwapMemoryDialog`.
    - Integrated Fill, Copy, and Swap operations into `MmemuFrame` and `MemoryPane`.
    - Added error handling (try-catch) to `CopyMemoryDialog` and `SwapMemoryDialog` for robust hex input validation.
    - Implemented **in-place editing** of memory cells by clicking on a hex value. Pressing **Return** commits the change, while **Escape** or losing focus cancels it.

## [0.2.0-dev] - 2026-04-05

### Added
- **C64 Video Color Update Test** (`src/plugins/devices/vic2/test/test_c64_video.cpp`): Added an integration test to verify that the C64 VIC-II chip correctly updates screen and border colors after simulating 5 million cycles, ensuring memory management and color rendering are accurate.
- **C64 Boot Screen Content Test** (`src/plugins/devices/vic2/test/test_c64_boot_screen.cpp`): Added a test to verify that the C64 boots correctly and displays non-space characters on the screen after initialization, indicating successful KERNAL ROM execution and screen memory updates.
- **`ExpressionEvaluator`** (`src/libdebug/main/expression_evaluator.h/cpp`): Stub condition evaluator used by `BreakpointList::checkExec/checkWrite/checkRead`; empty condition always matches.
- **CLI breakpoint/watchpoint commands** (`break`, `watch`, `delete`, `enable`, `disable`, `info breaks`) wired to `DebugContext::breakpoints()`.
- **`tests/test_breakpoints.cpp`**: Integration test exercising `create vic20` → assemble JMP loop → `break 0400` → `run 0400` → assert paused at correct PC.

### Changed
- **Test File Location Refactoring**: Moved various tests from the top-level `tests/` directory to their corresponding module-specific `src/plugins/test/<module>/` directories for better organization. Temporarily excluded problematic Atari-specific machine integration tests from the main test executable due to missing machine definitions.

### Fixed
- **Build System**:
    - Repaired corrupted `src/include/util/stb_image_write.h` which had invalid multi-line string literals and macros.
    - Fixed missing initializers for `SimPluginManifest` in multiple plugin initialization files to eliminate compiler warnings.
- `cli_interpreter.cpp`: Removed duplicate `info` and `break` else-if blocks that were unreachable dead code after the `eject` handler.
- `tests/test_plugin_extension.cpp`: Removed duplicate `int refresh;` field in local `TestCtx` struct (caused compile error).
- `tests/test_breakpoints.cpp`: Assembly line used label syntax (`loop: JMP loop`) unsupported by the mini-assembler; replaced with `JMP $0400`.

---

## [0.2.0-dev] - 2026-03-31

### Added
- **Atari 8-bit Video implementation (Phase 26)**:
    - **ANTIC DMA Engine**: Implemented display list interpretation, cycle-accurate timing (114 cycles/line, 262 lines/frame), LMS instruction, WSYNC, and NMI generation (VBI/DLI).
    - **GTIA Color and PMG**: Implemented color palette (256 colors), player-missile graphics positioning, collision detection, and console switches.
    - **POKEY Audio and IO**: Implemented 4-channel audio synthesis, polynomial-counter noise generators, frequency division, joined channels, 3 interval timers with IRQs, and skeletons for keyboard scanning and SIO.
    - Added dedicated documentation for each chip in `doc/README-ANTIC.md`, `doc/README-GTIA.md`, and `doc/README-POKEY.md`.

## [0.2.0-dev] - 2026-03-30

### Changed
- **Plugin ABI Stability (Phase 0.5)**:
    - Refactored `SimPluginHostAPI` to use stable C function pointers (`createCore`, `createDevice`, etc.) instead of passing C++ class pointers.
    - Updated `PluginLoader` to provide host-side wrappers for registry operations.
    - Updated all plugins to use the new host API, removing brittle registry singleton synchronization.
- **Resource Management (Phase 0.5)**:
    - Implemented RAII-based cleanup in `MachineDescriptor` via a new destructor that handles cpus, buses, I/O registry, and ROM buffers.
    - Updated `IORegistry` to support owned handlers, ensuring automatic deletion of machine-specific devices.
    - Added a `deleters` mechanism to `MachineDescriptor` for cleaning up generic machine-resident objects.
    - Fixed memory leaks in CLI, GUI, and integration tests by properly deleting machine instances on switch or shutdown.
- **Undefined Behavior (Phase 0.5)**:
    - Fixed potential undefined behavior in `FlatMemoryBus` address mask calculation for 32-bit address spaces.
- **Interface Segregation (Phase 0.5)**:
    - Refactored `ICore` into `ICpuRegs` (register access) and `ICpuDisasm` (disassembly) interfaces.
    - Refactored `IBus` to be leaner, segregating `ISnapshotable` (state management) and `IBusWriteLog` (write logging) interfaces.
    - Updated all CPU cores (`MOS6502`, `MOS6510`) and memory buses (`FlatMemoryBus`, `PortBus`) to implement the new segregated interfaces.
- **Encapsulation & Security**:
    - Moved `ExecutionObserver* observer` to protected members in `ICore` and `IBus`.
    - Added `setObserver()` and `getObserver()` accessors for safer observer management.
    - Made `IBus::peek8()` pure virtual to guarantee side-effect-free memory reads in all implementations.

### Fixed
- **Build System**: 
    - Fixed `make test` target in `Makefile` which was previously broken (Permission denied).
    - Configured the test binary to correctly link against plugin object files and registry implementations.
    - Fixed naming mismatch for the `vice-importer` plugin in the build system.
- **Hardware Emulation**:
    - Fixed incorrect wired-AND logic in `PIA6520` read-back of Peripheral Registers A and B. It now correctly returns the combination of latch values for output bits and pin values for input bits.
- **Testing**:
    - Fixed missing device registrations in VIC-20 integration tests.
    - Added missing include paths to the test build.

## [0.2.0-dev] - 2026-03-29

### Added
- **Deep Code Review (Critique)**:
    - Performed a comprehensive architectural and implementation review of the Phase 0/1 codebase.
    - Documented critical findings in `critique.md`, including ABI stability risks, memory leaks, encapsulation breaks, and performance bottlenecks.
    - Identified "fat" interfaces and bug-prone `peek8` implementations in `ICore` and `IBus`.
    - Recommended corrective actions for resource ownership, plugin API stability, and interface segregation.

## [0.2.0-dev] - 2026-03-28

### Added
- **Core Loader Infrastructure (Phase 13.1)**:
    - Defined `IImageLoader` and `ICartridgeHandler` interfaces in `src/libcore/main/image_loader.h`.
    - Implemented `ImageLoaderRegistry` singleton for managing loaders and cartridge factories.
    - Implemented `BinLoader` for raw binary images (.bin) at user-specified addresses.
    - Extended `SimPluginHostAPI` and `SimPluginManifest` to support plugin-provided loaders and cartridges.
    - Updated `PluginLoader` to handle automatic registration of plugin image loaders.
- **CBM Loader Plugin (Phase 13.2)**:
    - Implemented `PrgLoader` for Commodore `.prg` files with automatic load address detection.
    - Implemented `CrtParser` for the OpenC64Cart `.crt` format, supporting standard 64-byte headers and CHIP packets.
    - Implemented `CbmCartridgeHandler` for mapping standard 8 KB and 16 KB C64 cartridges into the bus.
    - Registered loaders into the host via `lib/mmemu-plugin-cbm-loader.so`.
- **Snapshot Integration for Cartridges**:
    - Integrated active cartridge tracking into the system snapshot system.
    - Extended `SystemSnapshot` to include the `cartridgePath`.
    - `DebugContext` now automatically reattaches the correct cartridge image upon snapshot restoration.
- **Bus Overlay API Enhancements**:
    - Added `addRomOverlay` and `removeRomOverlay` to the `IBus` interface.
    - Implemented these methods in `FlatMemoryBus` to support dynamic cartridge attachment and ejection.
- **Commodore PET Video (Phases 12.2, 12.3)**:
    - Implemented MOS 6545 / 6845 CRTC emulation with full timing counters and address generation.
    - Implemented PET Video Subsystem supporting PET 2001, 4000, and 8000 series.
    - Added support for character ROM glyph rendering, inverse video, and phosphor simulation.
- **Commodore PET Series (Phases 12.4, 12.5)**:
    - Implemented `IEEE488Bus` interface and `SimpleIEEE488Bus` implementation for PET peripherals.
    - Added `MachineDescriptor` factories for PET 2001, 4032, and 8032 models.
    - Implemented PET keyboard matrix (Graphics and Business layouts) wired to PIA/VIA.
    - Configured model-specific memory maps with RAM and ROM overlays.
- **Tape (Datasette) Support (Phase 14)**:
    - Added a roadmap for .tap pulse-encoded archive support across VIC-20, C64, and PET.
    - Planned core timing-exact decoder and machine-specific signal wiring.
- **IEC Serial Bus and Disk Drive Support (Phase 15)**:
    - Added a roadmap for IEC serial bus emulation and disk image support (.d64, .g64).
    - Planned High-Level Emulation (HLE) via KERNAL trapping and Level 2 virtual bus simulation.
- **Runtime Image and Cartridge Loading (Phase 13)**:
    - Added a roadmap for dynamic loading of `.prg`, `.bin`, and `.crt` images.
    - Planned CLI, GUI, and MCP interfaces for runtime memory injection and cartridge attachment.
- **Commodore 128 Implementation Plan (Phase 16)**:
    - Inserted a comprehensive roadmap for the C128, including the 8502 and Z80 CPUs, the 8722 MMU, and the 8563 VDC.
- **Commodore PET/CBM Implementation Plan (Phase 12)**:
    - Inserted a roadmap for the PET series, including the 6520 PIA, 6545 CRTC, and PET-specific video and memory configurations.
- **MOS 6510 CPU (Phase 11.1)**:
    - Implemented `MOS6510 : public MOS6502` in `src/plugins/6502/main/cpu6510.h/cpp`.
    - Built-in 6-bit I/O port at $0000 (DDR) and $0001 (DATA) using a `PortBus` proxy, since `MOS6502::read/write` are private non-virtual.
    - Power-on defaults: DDR=$00, DATA=$3F — all lines pulled high (KERNAL+BASIC+I/O visible immediately after reset).
    - Three `ISignalLine*` outputs (`signalLoram()`, `signalHiram()`, `signalCharen()`) for the C64 PLA.
    - Registered as core `"6510"` and machine preset `"raw6510"` in `mmemu-plugin-6502.so`.
- **C64 PLA Banking Controller (Phase 11.2)**:
    - Implemented `C64PLA : public IOHandler` in `src/plugins/devices/c64_pla/`.
    - Monitors LORAM/HIRAM/CHAREN signal lines to route BASIC ROM ($A000–$BFFF), KERNAL ROM ($E000–$FFFF), Character ROM or I/O ($D000–$DFFF) on every read.
    - Registered with `baseAddr()=0xA000` so IORegistry sort order places it before $D000 device handlers.
    - Returns `false` for I/O-visible state, allowing VIC-II/SID/CIA handlers to respond.
    - Plugin: `lib/mmemu-plugin-c64-pla.so`; registration name `"c64pla"`.
- **MOS 6526 CIA (Phase 11.3)**:
    - Implemented `CIA6526 : public IOHandler` in `src/plugins/devices/cia6526/`.
    - 16-register interface (`addrMask=0x000F`); configurable base address for CIA #1 ($DC00) and CIA #2 ($DD00).
    - Timer A and Timer B: 16-bit countdown with latches, continuous/one-shot modes; Timer B can count Timer A underflows.
    - TOD BCD clock with freeze/latch behavior on TODHR read and alarm interrupt.
    - ICR set/clear/read-to-clear semantics; IRQ propagated via `ISignalLine`.
    - Peripheral injection via `IPortDevice` for keyboard matrices and joysticks.
    - Plugin: `lib/mmemu-plugin-cia6526.so`; registration name `"6526"`.
- **MOS 6567/6569 VIC-II (Phase 11.4)**:
    - Implemented `VIC2 : public IOHandler, public IVideoOutput` in `src/plugins/devices/vic2/`.
    - Five video modes: Standard/Multicolor Text, Standard/Multicolor Bitmap, Extended Background Color.
    - 8-sprite engine with X/Y expansion, priority, mono/multicolor, hardware sprite-sprite and sprite-BG collision.
    - 384×272 total frame (RGBA8888) with 320×200 active display area and 32/36px borders.
    - Raster IRQ via `ISignalLine`; write-1-to-clear $D019 semantics.
    - VIC-II banking via `setBankBase()`; character ROM always visible at bank offsets $1000 and $9000.
    - DMA reads through dedicated `IBus*`; color RAM read via `m_dmaBus->peek8(0xD800+cellIdx)`.
    - Plugin: `lib/mmemu-plugin-vic2.so`; registration name `"6567"`.
- **MOS 6581 SID (Phase 11.5)**:
    - Implemented `SID6581 : public IOHandler, public IAudioOutput` in `src/plugins/devices/sid6581/`.
    - Three voices: 24-bit phase accumulator, Triangle/Sawtooth/Pulse/Noise waveforms (ANDed when combined).
    - 23-bit LFSR noise (taps 22,17) clocked on phase bit-19 rising edge.
    - ADSR envelope generator with 16 attack and 16 decay/release rate levels.
    - Hard sync (reset by previous voice overflow) and ring modulation (triangle XOR'd with previous voice MSB).
    - Chamberlin state-variable filter: LP/BP/HP individually selectable per $D418 mode bits.
    - `IAudioOutput` pull model: `tick()` fills 8192-sample ring buffer; `pullSamples()` drains it.
    - Plugin: `lib/mmemu-plugin-sid6581.so`; registration name `"6581"`.
- **Documentation (Phase 11.1–11.5)**:
    - Created `doc/README-6510.md` — MOS 6510 I/O port and PortBus proxy.
    - Created `doc/README-C64PLA.md` — C64 PLA banking matrix and signal wiring.
    - Created `doc/README-6526.md` — CIA 6526 register map, timer, TOD, and ICR details.
    - Created `doc/README-VIC2.md` — VIC-II video modes, sprite engine, banking, and register map.
    - Created `doc/README-SID.md` — SID voice architecture, ADSR, filter, and audio output.
    - Updated `doc/README-6502.md` — added MOS 6510 variant section.
    - Updated `doc/README-PLUGINS.md` — added all Phase 11 plugins; updated distribution strategy.
    - Updated `README.md` — expanded quick links and roadmap.

## [0.1.0-dev] - 2026-03-23

### Added
- **Machine Plugin Refactor & Build Cleanup:**
    - Moved the VIC-20 machine descriptor into a dedicated dynamic plugin (`lib/mmemu-plugin-vic20.so`).
    - Standardized the `lib/` directory to contain only dynamic plugins intended for distribution.
    - Relocated internal static archives (`.a` files) to `lib/internal/` to keep them hidden from end-users.
    - Updated the `Makefile` to handle complex cross-plugin dependencies while maintaining test suite stability.
- **VIC-I Video Chip & Standard Video Interface (Phase 10.2):**
    - Defined `IVideoOutput` standard interface for all video-generating plugins.
    - Implemented MOS 6560 (VIC-I) rendering logic, including character fetch, multi-color support, and palette.
    - Integrated VIC-I as a modular device plugin (`lib/mmemu-plugin-vic6560.so`).
    - Wired VIC-I and Color RAM into the VIC-20 machine descriptor.
- **Project Structure Refactoring:**
    - Reorganized all sub-modules into `main` and `test` subdirectories (e.g., `src/libdebug/main` and `src/libdebug/test`).
    - Moved module-specific unit tests from `tests/` to their respective module `test` directories.
    - Updated `Makefile` and all C++ include directives to reflect the new structure.
    - Reserved the top-level `tests/` directory for cross-cutting integration scenarios.
- **Device Infrastructure (Phase 9):**
    - Implemented `IOHandler` abstract base class for memory-mapped devices.
    - Implemented `IORegistry` for centralized device management and I/O dispatching.
    - Added `IPortDevice` interface for parallel I/O peripherals (VIA/CIA support).
    - Added `ISignalLine` interface for hardware signals (IRQ/NMI/Reset).
    - Integrated `IORegistry` into the build system and added unit tests in `tests/test_devices.cpp`.
- **Plugin Extension Host API (Phase 10.5):**
    - Extended `SimPluginHostAPI` with dynamic registration for GUI panes, CLI commands, and MCP tools.
    - Implemented `PluginPaneManager` (GUI) with `wxAuiNotebook` integration for plugin-provided tabs.
    - Implemented `PluginCommandRegistry` (CLI) for dynamically adding commands to the REPL.
    - Implemented `PluginToolRegistry` (MCP) for exposing plugin methods as JSON-RPC tools.
    - Updated `PluginLoader` to automatically wire host registration stubs during plugin initialization.
    - Verified cross-module callback logic with comprehensive unit tests.
- **VIC-20 Machine Integration (Phase 10.3 & 10.4):**
    - Implemented full VIC-20 memory map with support for Character, BASIC, and KERNAL ROM overlays.
    - Integrated `romLoad` utility for validated ROM loading into machine memory.
    - Implemented a standard 5-bit active-low joystick device as an `IPortDevice`.
    - Wired the Joystick to VIA #1 Port B in the VIC-20 machine descriptor.
    - Added `onJoystick` hook to `MachineDescriptor` for external input injection.
    - Updated `MachineDescriptor` to manage ROM memory lifetimes.
- **GUI Advanced Debugger Features (Phase 8.5):**
    - Implemented `ConsolePane` providing an integrated CLI environment within the GUI.
    - Added `FillMemoryDialog` and `CopyMemoryDialog` for easier memory manipulation.
    - Added `AssembleDialog` for on-the-fly instruction assembly and RAM injection.
    - Added `GotoAddressDialog` with optional PC update and "Go to" toolbar button.
    - Added `SearchMemoryDialog` for Hex/ASCII pattern searching in memory.
    - Added CLI `setpc`, `search`, and `searcha` commands for advanced debugging.
    - Added MCP `set_pc` and `search_memory` tools for AI-driven debugging.
    - Refactored GUI layout to include a vertical split for the console and notebook support.
- **GUI Target Implementation (Phase 8):**
    - Implemented a modular graphical user interface using wxWidgets (`src/gui/main.cpp`).
    - Added `MachineSelectorDialog` for dynamic machine preset selection.
    - Implemented `RegisterPane` with automatic change highlighting and support for multiple CPU slots.
    - Implemented `MemoryPane` providing a high-performance hex/ASCII dump with scroll support and side-effect-free reads.
    - Implemented `DisasmPane` with scrollable disassembly view centered on the current program counter.
    - Integrated all core libraries (mem, core, toolchain, debug, plugins) into the GUI frontend.
    - Established GUI standards: fixed-width fonts, asynchronous refresh (30 Hz), and DPI-aware layout using splitters.
- **MCP Target Implementation (Phase 7):**
    - Implemented MCP (Model Context Protocol) server over JSON-RPC 2.0 (`src/mcp/main.cpp`).
    - Created a lightweight, dependency-free JSON library (`src/mcp/minijson.h`).
    - Registered essential tools for AI interaction: `step_cpu`, `read_memory`, `write_memory`, and `read_registers`.
    - Exposed `machine_state` as an MCP resource.
    - Added a Python-based validation harness (`tests/mcp_test.py`) for automated MCP interface testing.
    - Updated `PluginLoader` to use `std::cerr` for logging to keep `stdout` clean for JSON-RPC.
    - Fixed a race condition in the `Makefile` when running `make -j clean all`.
- **libmem — Address Bus Abstraction (Phase 1):**
    - Implemented `IBus` abstract interface for memory and I/O access.
    - Implemented `FlatMemoryBus` with support for heap-allocated storage, configurable address masking, and ROM overlays.
    - Added high-performance write-logging to `FlatMemoryBus` for debugger synchronization.
    - Added state snapshot/restore support to memory buses.
    - Created `tests/test_flatmembus.cpp` with comprehensive coverage for basic R/W, masking, and overlays.
- **libcore — ICore and MOS 6502 (Phase 2):**
    - Implemented `ICore` abstract CPU interface, featuring register descriptors and convenience accessors.
    - Implemented a fully functional `MOS6502` CPU core supporting all official NMOS opcodes and common undocumented instructions.
    - Implemented all 6502 addressing modes including the infamous indirect JMP bug for cycle-accurate behavior.
    - Added `MachineDescriptor`, `CpuSlot`, and `BusSlot` structures for data-driven machine composition.
    - Implemented `MachineRegistry` for dynamic registration and creation of machine presets.
    - Created `tests/test_cpu6502.cpp` to verify instruction accuracy, status flags, and state snapshots.
- **libtoolchain — Disassembler and Assembler (Phase 3):**
    - Implemented `IDisassembler` interface and `DisasmEntry` for detailed instruction metadata.
    - Implemented `Disassembler6502` with support for symbol resolution and control-flow detection.
    - Implemented `IAssembler` interface with support for configurable backend metadata.
    - Implemented `KickAssemblerBackend` for integration with external Java-based tools.
    - Implemented `Assembler6502` native mini-assembler for single-line instruction support.
    - Implemented `SymbolTable` and `SourceMap` for mapping between machine code and source labels/locations.
    - Added `BaseDisassembler` for default delegation to CPU core disassembly.
    - Created `tests/test_disasm6502.cpp` to verify disassembly and symbol resolution.
- **libdebug — Debug Infrastructure (Phase 4):**
    - Implemented `ExecutionObserver` interface for monitoring CPU execution and memory events.
    - Implemented `BreakpointList` supporting EXEC, READ_WATCH, and WRITE_WATCH breakpoints.
    - Implemented `TraceBuffer` for cycle-accurate execution history.
    - Implemented `DebugContext` for managing debug sessions, breakpoints, and system-wide snapshots.
    - Added support for diffing memory states between snapshots.
    - Created `tests/test_debug.cpp` to verify breakpoint logic, tracing, and snapshot integrity.
- **Plugin Architecture & Refactor (Phase 6):**
    - Finalized the `mmemu_plugin_api.h` C-ABI contract for runtime plugins.
    - Implemented `PluginLoader` using `dlopen`/`dlsym` for dynamic loading of `.so` modules.
    - Implemented `CoreRegistry` and `ToolchainRegistry` for centralized management of CPU cores and machine-specific tools.
    - Decoupled the 6502 implementation from the host libraries, moving it into a dedicated `mmemu-plugin-6502.so`.
    - Updated the CLI to automatically discover and load plugins from the `lib/` directory.
- **CLI Target Implementation (Phase 5):**
    - Implemented interactive REPL with command history and parsing.
    - Added support for dynamic machine creation and selection.
    - Implemented core debugging commands: `step`, `regs`, `m` (memory dump), `f` (memory fill), `disasm`.
    - Added direct CPU instruction execution using the `.` prefix (e.g., `.lda #$42`).
- **Utility & Performance:**
    - Implemented a generic `CircularBuffer<T>` template for high-performance fixed-capacity logging.
    - Refactored `FlatMemoryBus` write-log and `TraceBuffer` execution log to use `CircularBuffer`, eliminating $O(N)$ removal overhead.
- **Project Infrastructure:**
    - Unified the test suite into a single `mmemu-tests` binary using `tests/test_main.cpp`.
    - Added `tests/test_harness.h` for simple, lightweight unit testing.
- **Project Foundation (Phase 0):**
    - Created directory skeleton for core libraries: `src/libmem/`, `src/libcore/`, `src/libdevices/`, `src/libtoolchain/`, and `src/libdebug/`.
    - Created `roms/` and `tests/` directories.
    - Added `.gitkeep` and `.gitignore` files to ensure proper tracking of empty directories and ignored ROM files.
- **Library Build System:**
    - Extensively updated `Makefile` to support modular builds.
    - Added targets for building static archives (`make libs`) and running tests (`make test`).
    - Configured binaries (`mmemu-cli`, `mmemu-mcp`, `mmemu-gui`) to link against internal libraries.
- **ROM Loader Utility:**
    - Implemented `romLoad` in `src/libcore/rom_loader.h/cpp` for validated binary loading.
- **Documentation:**
    - Created `GEMINI.md` for project overview and quick-start instructions.
    - Created `STYLEGUIDE.md` to establish project-wide coding conventions.

### Changed
- **Directory Reorganization:**
    - Moved global `include/` directory to `src/include/` to centralize all source-related files.
    - Updated `Makefile` and `GEMINI.md` to reflect the new header search paths.
- **Style Alignment:**
    - Updated all architectural specifications (`.plan/arch.md`) and roadmaps (`.plan/todo.md`) to use `camelCase` for variable and function names as per the new style guide.
    - Refactored placeholder headers in `src/` to follow `camelCase` and `PascalCase` conventions.
    - Renamed several identifiers in internal stubs for consistency.

### Fixed
- Fixed build system to correctly handle library object dependencies and include paths.
- Fixed a bug in the 6502 `Mode::REL` implementation where branch targets were calculated incorrectly.
