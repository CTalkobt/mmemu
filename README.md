# mmemu — Multi-Machine Emulator / Simulator

**mmsim** (Multi-Machine Simulator) is a universal 8/16-bit emulation platform designed with a modular, plugin-based architecture. It provides a robust host infrastructure that allows different CPU cores, memory models, and I/O devices to be composed into complete machine simulations at runtime.

Help with this development by contributing and buy me a coffee at: https://kodecoffee.com/i/ctalkobt

## 1. Core Architecture

The system is built on a strictly decoupled library system to ensure flexibility and maintainability:

- **libmem**: Abstract memory and address bus models (`IBus`).
- **libcore**: Generic CPU core interfaces (`ICore`) and machine configuration logic.
- **libdevices**: Infrastructure for memory-mapped I/O handlers.
- **libtoolchain**: Multi-ISA assembly and disassembly services.
- **libdebug**: Advanced debugging features including breakpoints, trace history, and system snapshots.

### Plugin System
All processor-specific logic and specialized devices are decoupled from the host and loaded dynamically as `.so` (or `.dll`) modules via the `PluginLoader`. At startup, the system scans the `./lib` directory for modules and registers their capabilities with the core registries.

---

## 2. Plugin Architecture

The **mmsim** ecosystem relies on a modular system where specialized logic is provided by dynamically loadable plugins.

### 2.1 Startup Discovery
At startup, all binaries (`mmemu-cli`, `mmemu-gui`, `mmemu-mcp`) use the `PluginLoader` to scan for resources:
- **Search Path**: `./lib/` relative to the current working directory.
- **Resource Types**: The loader looks for `.so` files (on Linux) that export the required ABI.

### 2.2 The Plugin ABI
Every plugin must export a C-compatible entry point that returns a manifest of its capabilities:
```cpp
extern "C" SimPluginManifest* mmemuPluginInit(const SimPluginHostAPI* host);
```

### 2.3 Supported Resource Types
A single plugin module can provide multiple resources, which are automatically registered upon loading:

- **CPU Cores (`ICore`)**: Implementations of processor logic and register sets (e.g., 6502).
- **Toolchains**: Integrated **Assemblers** and **Disassemblers** for specific ISAs.
- **Machine Presets**: Descriptor factories that define how to compose a CPU, memory bus, and devices into a specific system (e.g., C64, VIC-20).
- **I/O Devices (`IOHandler`)**: Individual memory-mapped hardware components.

---

## 3. Supported Machines

**mmsim** currently supports several 8-bit machine presets from the Commodore family:

### Commodore VIC-20
- **Profiles**: `vic20`, `vic20+3k`, `vic20+8k`, `vic20+16k`, `vic20+32k`.
- **Hardware**: MOS 6502 CPU, 6560/6561 VIC-I Video/Sound, dual 6522 VIAs, keyboard matrix, joystick.
- **Symbols**: Standard KERNAL jump table symbols are auto-loaded from `roms/vic20/kernal.sym`.

### Commodore 64
- **Profile**: `c64`.
- **Hardware**: MOS 6510 CPU, 6567/6569 VIC-II Video, 6581 SID Sound, dual 6526 CIAs, PLA banking, keyboard matrix.
- **Symbols**: Standard KERNAL jump table symbols are auto-loaded from `roms/c64/kernal.sym`.

### Commodore PET Series
- **Profiles**: `pet2001`, `pet4032`, `pet8032`.
- **Hardware**: MOS 6502 CPU, 6520 PIA, 6522 VIA, 6545 CRTC (for 4032/8032), discrete video logic (for 2001), IEEE-488 bus.
- **Symbols**: Standard KERNAL jump table symbols are auto-loaded from `roms/pet4032/kernal.sym`.

---

## 4. Symbol Management & Expressions

**mmsim** features a robust expression evaluator and symbol management system integrated into all targets.

### 4.1 Expression Evaluator
Anywhere an address or value is required (CLI, GUI dialogs, MCP, Calculator), you can use complex expressions:
- **Literal Formats**:
    - **Hex**: `$1000` or `0x1000`. (No space allowed after prefix)
    - **Binary**: `%10101010`. (No space allowed after prefix)
    - **Octal**: `0777`.
    - **Decimal**: `4096` or `3.14159`.
    - **Character**: `'a'` (returns ASCII 97). Supports escape sequences: `\n`, `\r`, `\t`, `\\`, `\'`.
- **Symbols**: Use any defined label (e.g., `CHROUT`, `start_vector`).
- **Registers**: Use current CPU register names directly (e.g., `PC + 2`, `A`, `X`, `Y`, `P`).
- **Register shorthand** (`@`-prefix): `@A`, `@X`, `@Y`, `@SP`, `@PC` — explicitly reads the named register. Case-insensitive. Useful in expressions to avoid ambiguity with symbol names (e.g., `@A + 1`, `@PC == $C000`).
- **Status flag shorthand** (`.`-prefix): `.C`, `.Z`, `.N`, `.V`, `.I`, `.D`, `.B` — reads a single bit (0 or 1) from the CPU status register by flag letter. Case-insensitive. Useful in breakpoint conditions (e.g., `.Z == 1`, `.C`).
- **Unary Operators**:
    - `<`: High byte of result (e.g., `<$1234` is `$12`).
    - `>`: Low byte of result (e.g., `>$1234` is `$34`).
    - `!`: Logical negation (0 if non-zero, 1 if zero).
- **Binary Operators** (in order of precedence):
    - `^`: Power (exponentiation).
    - `*`, `/`, `%`: Multiplication, Division, Modulus.
        - *Note*: Modulus `%` requires a space or parenthesis if the following operand starts with `0` or `1` (to distinguish from a binary literal).
    - `+`, `-`: Addition, Subtraction.
    - `<<`, `>>`: Bitwise Left and Right shift.
    - `&`: Bitwise AND.
    - `|`: Bitwise OR.
    - `==`, `!=`, `<`, `>`, `<=`, `>=`: Comparisons (return 1 for true, 0 for false).
- **Functions**:
    - `sin(x)`, `cos(x)`, `tan(x)`: Trigonometric functions (radians).
    - `asin(x)`, `acos(x)`, `atan(x)`: Inverse trigonometric functions.
    - `sqrt(x)`: Square root.
    - `log(x)`: Base-10 logarithm.
    - `exp(x)`: Base-e exponential ($e^x$).
    - `abs(x)`: Absolute value.
    - `between(n, val, delta)`: Returns 1 if `n` is in range `[val-delta, val+delta]`, else 0.
- **Parentheses**: Use `(` and `)` to override precedence (e.g., `(PC + 2) & $FF00`).

> **Note on Precision**: The evaluator uses `double` precision for all calculations. When an integer is required (e.g., for memory addresses or register values), the result is truncated to `uint32_t`.

> **Note on Conditions**: When used in a breakpoint or watchpoint condition, any **non-zero** result is treated as `true` (trigger), while a **zero** result is treated as `false`. An empty condition always defaults to `true`.

### 4.2 Symbol Management
Manage symbols via the CLI `sym` command or the GUI **Symbols** pane:
- `sym add <label> <addr>`: Manually add a label.
- `sym del <label>`: Remove a label.
- `sym list`: List all symbols.
- `sym search <query>`: Search for symbols by name.
- `sym load <path>`: Load symbols from a `.sym` file (supports `label = value` and `label value` formats).
- `sym clear`: Clear the current symbol table.

### 4.3 Kernal and BASIC Routine Monitoring
You can monitor the entry and exit points of Kernal and BASIC routines by setting the "kernal" or "basic" logger to DEBUG level:
```bash
log level kernal debug
log level basic debug
```
When enabled, the system will log the routine name and all register states upon entry, and log them again upon exit (detected via the matching `RTS`).

---

## 5. CLI Target (Implemented)

The `mmemu-cli` binary provides an interactive REPL for low-level machine control and debugging.

### 5.1 Command Line Options
Automate startup tasks via command line arguments:
- `-m, --machine <id>`: Automatically create a machine on startup (e.g., `c64`, `vic20`).
- `-i, --mount <path>`: Mount a disk, tape, or program image immediately.
- `-t, --type <text>`: Type text into the machine virtual keyboard on startup (supports `\n`, `\r`, `\t`).
- `-h, -?, --help`: Show available options and exit.

### 5.2 Key Features:
- **Interactive REPL**: Command-driven interface with session management.
- **Machine Lifecycle**: List available machine types and create instances at runtime via the `create <id>` command.
- **Virtual Typing**: Use the `type <string>` command to simulate keystrokes (supports `\n`, `\r`, `\t`).
- **CPU Control**: 
    - `step [n]`: Execute specific number of cycles.
    - `regs`: Detailed register inspection (automatically adapts to the current CPU).
    - `asm <addr>`: **Interactive Assembly** - Enter a continuous assembly session at the specified address (exit with `.`).
    - `.<instr>`: **Direct Execution** - Use the native mini-assembler to instantly execute a single instruction (e.g., `.lda #$42`).
- **Memory Tools**:
    - `m <addr> [len]`: Hex and ASCII memory dump.
    - `f <addr> <val> [len]`: Direct memory modification (supports ranges).
    - `copy <src> <dst> <len>`: Copy memory blocks.
    - `swap <addr1> <addr2> <len>`: Swap two equal-length memory blocks.
    - `search <hex...>`: Search for a hex byte pattern.
    - `findnext` / `findprior`: Navigate search results.
    - `load <path> <addr>`: Binary file injection.
- **Symbol Table**: Full management via the `sym` command.
- **Disassembly**: Multi-ISA disassembly with integrated symbol resolution.
- **Datasette (Tape)**:
    - `tape mount <path>`: Mount a `.tap` file for playback.
    - `tape play` / `tape stop` / `tape rewind`: Playback transport controls.
    - `tape record`: Arm the datasette for recording — the machine's cassette write line is captured into an in-memory buffer. The sense line is asserted so the machine believes a button is pressed.
    - `tape stoprecord`: Stop capturing. The buffer is retained in memory.
    - `tape save <path>`: Write the captured buffer to a `.tap` file in standard `C64-TAPE-RAW` format.
    - Typical save workflow: `tape record` → run the machine's SAVE command → `tape stoprecord` → `tape save output.tap`.

---

## 6. MCP Target (Implemented)

The `mmemu-mcp` binary implements the **Model Context Protocol**, allowing AI agents to interact with the simulator.

### Available Tools:
- `step_cpu(machine_id, count)`: Execute instructions.
- `run_cpu(machine_id, max_steps)`: Run until breakpoint or limit.
- `read_memory(machine_id, addr, size)`: Inspect memory.
- `write_memory(machine_id, addr, bytes)`: Inject data.
- `read_registers(machine_id)`: Inspect CPU state.
- `disassemble(machine_id, addr, count)`: Disassemble instructions.
- `list_symbols`, `add_symbol`, `remove_symbol`, `load_symbols`, `clear_symbols`: Manage symbols.
- `set_breakpoint` / `set_watchpoint` / `delete_breakpoint`: Breakpoint management.
- `get_stack(machine_id, count)`: Show the call stack trace.
- `load_image`, `attach_cartridge`, `reset_machine`: Lifecycle and media.
- `mount_tape(machine_id, path)`: Mount a `.tap` file for playback.
- `control_tape(machine_id, operation)`: Transport control — `play`, `stop`, `rewind`, `record`, `stoprecord`.
- `record_tape(machine_id)`: Arm the datasette for recording (equivalent to `control_tape` with `record`).
- `save_tape_recording(machine_id, path)`: Stop recording and write the captured buffer to a `.tap` file.
- `list_machines`, `create_machine`: Machine management.
- `list_loggers`, `set_log_level`: Logging control.

---

## 7. GUI Target (mmemu-gui)

Multi-pane graphical debugging environment built on wxWidgets.

### 7.1 Command Line Options
Automate GUI startup tasks:
- `-m, --machine <id>`: Automatically load the specified machine.
- `-i, --mount <path>`: Mount a disk, tape, or program image on startup.
- `-t, --type "<text>"`: Type text into the machine virtual keyboard upon loading (supports `\n`, `\r`, `\t`).
- `-h, -?, --help`: Show available options and exit.

### 7.2 Toolbar & Menus
- **Ctrl+Y**: Show the Symbol Table pane.
- **Ctrl+B / Ctrl+T / Ctrl+M**: Quick access to Breakpoints, Stack, and Machine tabs.
- **Debug Menu**: Enhanced with memory fill/copy/swap and symbol management.

### 7.2 Panes & Tools
- **Disassembly Pane**: Real-time ISA decoding with symbol resolution.
- **Symbols Pane**: Integrated management of labels with search and "Go To" navigation.
- **Console Pane**: Full parity with `mmemu-cli`.
- **Register Pane**: Real-time inspection of all CPU registers. Status registers (P on the 6502/6510) additionally display individual flag bits in `NV-BDIZC` notation — uppercase letter = bit set, `.` = bit clear — alongside the hex value (e.g. `$36  ..-.IZ.`).
- **Stack Pane**: JSR/RTS tracking with navigation support.
- **Tape Pane**: Datasette controls — Mount, Play, Stop, Rewind, Record, and Save.
- **Engineering Calculator** (**Ctrl-Shift-C**):
    - **28-Button Dynamic Layout**: Key labels and functions adapt automatically based on the current numerical base.
    - **Modes**: Toggles between **Floating Point** and **Integer** precision.
    - **Bases**: Supports **Hex**, **Decimal**, **Binary**, and **Octal**. In Hex mode, `A-F` are primary keys; in other modes, they shift to scientific functions.
    - **Scientific Functions**: `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `sqrt`, `log`, `exp`, `abs`, `^`, `<<`, `>>`.
    - **Memory**: 8 persistent memory slots (`Rcl0`-`Rcl7`) and an `Ans` register for the last result.
    - **Hand-Typing Support**: The display is a fully editable field allowing you to type complex expressions (e.g., `sin(45) * @PC`) directly.
    - **Keyboard Accessible**: All buttons have keyboard shortcuts; the bottom-rightmost key is always mapped to **Enter** for evaluation.

---

## 8. Plugin Ecosystem

For a complete list of supported hardware, see **[doc/README-PLUGINS.md](doc/README-PLUGINS.md)**.

---

## 9. Installation

**mmsim** provides installation scripts to deploy the binaries, plugins, and resources to standard system or user directories.

### 9.1 Installation Scripts
The following scripts are located in the `tools/` directory:

- **Local User Install**: `./tools/install-local.sh`
    - Installs to `$HOME/.local/bin`, `$HOME/.local/lib/mmsim`, and `$HOME/.local/share/mmsim`.
    - Recommended for development and single-user use.
- **System Install**: `sudo ./tools/install-system.sh`
    - Installs to `/usr/local/bin`, `/usr/local/lib/mmsim`, and `/usr/local/share/mmsim`.
    - Requires root privileges.

Both scripts will automatically generate and install **man pages** (via `make man`) if `pandoc` is available on your system.

### 9.2 Search Paths & Resource Discovery
The emulator uses a centralized discovery system to find its components at runtime. It searches the following locations in order:

#### Plugins (`.so` modules)
1. `./lib/` (Local development)
2. `$HOME/.local/lib/mmsim/plugins/`
3. `/usr/local/lib/mmsim/plugins/`
4. `/usr/lib/mmsim/plugins/`

#### Data (ROMs, Machine JSONs)
1. `./` (Local development)
2. `$HOME/.local/share/mmsim/`
3. `/usr/local/share/mmsim/`
4. `/usr/share/mmsim/`

#### Man Pages
1. `$HOME/.local/share/man/`
2. `/usr/local/share/man/`

---

## 10. Build and Test Toolchain

### Assemblers
The **45GS02 test suite** uses **ca45**, a cross-assembler for the 45GS02 CPU:
- **ca45** — Official assembler for 45GS02 and 6502-family processors
- **Project**: [github.com/CTalkobt/m65compiler](https://github.com/CTalkobt/m65compiler) (includes ca45 binary and source)
- **Usage**: Test programs are written in `.s` format and assembled with `ca45 test.s -o test.prg`
- **Test Validation**: The `tests/45gs02/validate.py` script assembles `.s` files and cross-validates output against both **mmsim** and **xmega65** emulators.

---

## 11. Getting Started

### Prerequisites
- **C++17 Compiler** (GCC 9+, Clang 10+).
- **Libraries**: `spdlog`, `fmt`, `wxWidgets` (3.0+), `ALSA` (`libasound`), `nlohmann/json`.
- **Test Tools**: `ca45` assembler (from [github.com/CTalkobt/m65compiler](https://github.com/CTalkobt/m65compiler)).
- **Documentation**: `pandoc` (optional, required for `make man`).

### Building
```bash
make all      # Build everything
make man      # Generate man pages
make test     # Build and run the test suite (161+ tests)
```

---

## 12. Development Standards
- Adhere to [STYLEGUIDE.md](STYLEGUIDE.md).
- Track updates in [CHANGELOG.md](CHANGELOG.md).
