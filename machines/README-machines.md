# Machine Descriptor JSON Format

This document defines the JSON schema used to describe and reconstruct the various 8-bit machine architectures supported by **mmsim**. These descriptors are located in the `machines/` directory and are used by the `JsonMachineLoader` (`libcore`) to instantiate machines at runtime.

---

## Architectural Constraint

> **Machine-specific code must never reside in the core libraries (`libmem`, `libcore`, `libdevices`, `libtoolchain`, `libdebug`).** Core libraries contain only machine-agnostic abstractions (interfaces, registries, generic helpers). Any class whose name, behavior, or header path implies a specific machine or chip family belongs in the corresponding machine plugin under `src/plugins/`.
>
> The JSON loader (`libcore`) expresses DMA/bus wiring in terms of **generic** concepts rather than named machine-specific classes.

---

## Top-Level Structure

```json
{
  "machines": [ <MachineDescriptor>, ... ]
}
```

---

## MachineDescriptor Fields

| Field | Type | Description |
|---|---|---|
| `id` | string | Unique registry key for the machine (e.g., `"c64"`, `"vic20"`) |
| `displayName` | string | Human-readable name (e.g., `"Commodore 64"`) |
| `licenseClass` | string | `"proprietary"` or `"open"` |
| `clockHz` | number | (Optional) Device clock in Hz (e.g. `985248` for a PAL C64). The GUI and CLI start machines at real time and pace them at this rate (`--speed max` or `speed max` runs unpaced; #169); machines created elsewhere, e.g. in tests or the MCP server, start unpaced. Omit for a machine that is never paced. |
| `bus` | BusSpec | Primary system memory bus configuration |
| `cpu` | CpuSpec | CPU core type and bus wiring |
| `devices` | DeviceSpec[] | Array of I/O devices to instantiate and register |
| `roms` | RomSpec[] | ROM images to load and map or pass to devices |
| `ramExpansions` | RamSpec[] | Optional/Expansion RAM blocks |
| `signals` | SignalSpec[] | Interrupt and internal signal line definitions |
| `kbdWiring` | KbdWireSpec | Keyboard matrix port mapping |
| `ui` | UiSpec | Debugger panes for this machine's hardware and its VIC graphics formats |
| `plaWiring` | PlaWireSpec | (C64 specific) PLA banking signal mapping |
| `joystick` | JoySpec or [JoySpec] | Joystick port(s) `{device, port}` (switches on bits 0-4), or `{lines: [{device, port, up, down, left, right, fire}]}` to put each switch on a chosen bit of one or more ports (the VIC-20); array index = joystick port. A port shared with the keyboard reads both. |
| `joystickPrimary` | int | The joystick port most software reads (a host's first controller goes here) |
| `ioHook` | IoHookSpec | Custom I/O address mirroring and remapping |
| `scheduler` | SchedSpec | Execution policy and frame synchronization |
| `intercepts` | InterceptSpec[] | (Optional) Host-side handlers for firmware routines and calls, see below |

---

## Sub-Object Specifications

### BusSpec
Defines the main system address bus.
- `type`: Usually `"FlatMemoryBus"`.
- `name`: Identifier for the bus (typically `"system"`).
- `addrBits`: Number of address lines (e.g., `16` for 64 KB).

### CpuSpec
Defines the CPU core and its primary bus connections.
- `type`: CPU core registry key (e.g., `"6502"`, `"6510"`).
- `dataBus`: Name of the bus used for data access.
- `codeBus`: Name of the bus used for instruction fetching.
- `portBusAsData`: (Optional, 6510 specific) If `true`, the CPU's internal port bus is used as the data bus.

### DeviceSpec
Individual hardware components.
- `type`: Device registry key (e.g., `"6522"`, `"6567"`, `"c64_pla"`).
- `name`: Instance name (e.g., `"VIA1"`, `"VIC-II"`).
- `baseAddr`: Hex string of the memory-mapped base address, or `null`.
- `clockHz`: (Optional) Custom clock frequency for the device.
- `colorRam`: (Optional, bool) Indicates the device needs access to the color RAM.
- `dmaBus`: (Optional, bool) Indicates the device needs a DMA bus.
- `lines`: (Optional) Map of device pins to signal names (e.g., `{"ca1": "vsync"}`).
- `linkedDevices`: (Optional) Map of internal device pointers to other named devices.
- `ramData`: (Optional) For banking controllers, the name of the bus providing raw RAM.

### RomSpec
- `label`: Unique identifier for the ROM (e.g., `"kernal"`, `"char"`).
- `path`: Path to the binary file relative to the project root.
- `size`: Size of the ROM in bytes.
- `overlayAddr`: Hex string address where the ROM is mapped to the bus, or `null` if not directly mapped.
- `writable`: (bool) Whether the overlay is writable.
- `passToDevice`: Array of device names that should receive a pointer to this ROM data.
- `cpuAddr`: (Optional) CPU address of the ROM's first byte when it is banked in (e.g. `"0xE000"` for a C64 KERNAL passed to the PLA); needed for `intercepts` on a ROM without an `overlayAddr`.
- `intercepts`: (Optional) InterceptSpec[] that fire only while this ROM is visible at the address (the CPU reads the ROM's bytes there), e.g. not when the C64 KERNAL is banked out.

### InterceptSpec (intercepts)
An intercept is a host-side handler that stands in for a firmware routine or call. Plugins provide the types; the machine (or one of its ROMs) installs instances. `intercept types` in the CLI lists the types loaded.

- `type`: (required) e.g. `"cbm.kernal.fileio"`, `"mega65.hdos"`.
- `name`: (optional) name for the CLI (`intercept on|off|set <name> ...`); defaults to the type.
- `enabled`: (optional, default `true`).
- Call intercepts (`mega65.hdos`): `trap` and `functions` select the calls served (absent = all).
- Any other keys are the type's own settings.

| Type | Plugin | Kind | Settings |
|---|---|---|---|
| `cbm.kernal.fileio` | cbm-hle | PC (KERNAL `LOAD`/`SAVE`) | `load`, `save` (addresses, default `$FFD5`/`$FFD8`; SAVE writes to the folder, replacing a file only with `@:`), `path` (host folder), `zeroPage` {`status`, `fnlen`, `sa`, `fa`, `fnadr`} (default C64/VIC-20: `$90`, `$B7`, `$B9`, `$BA`, `$BB`) |
| `mega65.hdos` | mega65 | Call (HYPPO trap `$00`) | `paths` (host folders, the first that exists), `filename` (overrides setname's name) |

```json
{ "label": "kernal", "path": "roms/c64/kernal.bin", "size": 8192, "cpuAddr": "0xE000",
  "intercepts": [ { "type": "cbm.kernal.fileio", "name": "kernal-fileio", "path": "." } ] }
```

At run time: `intercept [list]`, `intercept on|off <name>`, `intercept set <name> <setting> <value>`; `hle on|off|path <dir>` is a shortcut for `cbm.kernal.fileio`.

### RamSpec (ramExpansions)
- `name`: Identifier for the RAM block.
- `base`: Hex string base address.
- `size`: Size in bytes.
- `installed`: (bool) Whether this RAM block is present by default.

### SignalSpec
- `name`: Unique signal identifier.
- `type`: `"simple"`, `"shared"`, `"latch"`, `"nmi"` or `"sharedNmi"`.
- `sources`: Array of device names or `Device.Line` identifiers that drive this signal.
- `"sharedNmi"`: a wired-OR NMI (edge-triggered at the CPU). A source `"DEV"` (or `"DEV.irqA"`/`"DEV.irqB"`) is a chip's interrupt output wired to NMI, as with the VIC-20's VIA1 and the C64's CIA2; `"DEV.nmi"` is a device's own NMI output. `lines`: names of extra inputs, e.g. `["restore"]` for a RESTORE key wired straight to NMI (see `kbdWiring.restore`).

### UiSpec (ui)
Which hardware-dependent debugger panes the GUI shows for this machine (others, such as the console, CPU and memory panes, are always shown). A machine without `ui` gets every pane.
- `panes`: pane ids: `graphics-vic` (VIC-I/II/IV sprites and characters), `sidscope`, `cartridge`, `tape`, `drive`, `mega65` (MEGA65 Status).
- `graphicsFormats`: the `graphics-vic` formats the video chip has (`gfx formats` ids), in menu order; omitted = all. The CLI `gfx` and MCP `render_graphics` accept only these.
- `screenButtons`: buttons under **Capture Keyboard** on the GUI screen pane, `[{ "label", "keys", "tooltip" }]`. A button holds its `keys` (onKey names, pressed in order, released in reverse) while it is held, and for at least 150 ms after a click so the machine sees it. The C64 and VIC-20 have **RUN/STOP + RESTORE** (`["run_stop", "restore"]`) and **RESTORE**; the PET **Trigger NMI** (`["nmi"]`). C++ machines set `MachineDescriptor::screenButtons` (the MEGA65 has the same two as the C64).

```json
"ui": { "panes": ["tape", "cartridge", "graphics-vic"],
        "graphicsFormats": ["char", "char-mc", "char16", "char16-mc"] }
```

C++ machines set `MachineDescriptor::uiPanes` (with `uiPanesSet = true`) and `graphicsFormats`.

### Wiring between devices
- A device's `"lines"` (`ca1`, `cb1`, `ca2`, `cb2`, `irqA`, `irqB`) name a signal from `signals`, or another device's line as `"DEV.line"` (its `getSignalLine`), e.g. `"ca2": "IEEE.ndacOut"`.
- `portWiring`: `[{ "device", "port": "A"|"B", "from": "DEV.port" }]` attaches a device's named port (`IOHandler::namedPort`, e.g. an IEEE-488 bus's `dataIn`) to a chip port, sharing it with a device already there (a keyboard).
- `pinWiring`: `[{ "device", "port", "bit", "signal" }]` puts a signal's level on one input bit of a chip port, e.g. the PET's retrace on VIA PB5 or its tape sense on PIA1 PA4.
- `tapeWiring` lines (`sense`, `motor`, `write`, `readPulse`) are `"DEV.pin"`, a signal declared in `signals` (e.g. a latch the tape sets and `pinWiring` reads), or a CPU line.

### KbdWireSpec (kbdWiring)
Wires a keyboard matrix device to I/O ports.
- `device`: Name of the keyboard device instance.
- `colPort`: `{ "device": "...", "port": "A" or "B" }`
- `rowPort`: `{ "device": "...", "port": "A" or "B" }`
- `nmiKey` (optional): a plain NMI button for a machine with no RESTORE key (the PET): `{ "key": "nmi", "line": "<signal>" }`, same `line` forms as `restore`. Its GUI button is declared in `ui.screenButtons`.
- `restore` (optional): the RESTORE key, outside the matrix. `{ "line": "<signal>" }` (a `sharedNmi` line, asserted while held) or `{ "line": "DEV.pin", "activeLow": true }` (e.g. the VIC-20's `VIA1.ca1`). `onKey("restore", down)` drives it.

### PlaWireSpec (plaWiring)
- `device`: Name of the PLA instance.
- `signals`: Map of PLA inputs to CPU signals (e.g., `{"loram": "loram"}`).

### IoHookSpec (ioHook)
- `mirrorRanges`: Array of objects defining address mirrors:
    - `from`: Hex start of mirror range.
    - `to`: Hex end of mirror range.
    - `target`: Name of the device being mirrored.
    - `targetBase`: Hex base address of the target device.

### SchedSpec (scheduler)
- `type`: Execution policy (usually `"step_tick"`).
- `frameAccumulator`: (Optional) Frame sync settings:
    - `cycles`: Number of cycles per frame.
    - `signal`: Name of the signal to pulse at end of frame (e.g., `"vsync"`).
    - `retraceSignal`, `retraceCycles`: (Optional) a latch signal held low for the last `retraceCycles` cycles of each frame (a vertical retrace level, e.g. the PET's VIA PB5).

---

## Implementation Notes

### Inheritance (`_extends`)
The JSON loader supports a documentation/planned convention of `_extends`. A machine can inherit all properties of a parent machine and only override specific fields. When merging:
- Simple fields are overwritten.
- Arrays (`devices`, `roms`, `ramExpansions`) are usually replaced entirely.
- In some implementations, entries with `"_same": true` in the child's array can be used to copy the parent's entry at that index.

### Device Wiring
The loader automatically wires devices together after instantiation:
1. **Signals**: `SharedIrqManager` or simple lines are created and connected to the `setIrqLine` / `setNmiLine` methods of specified devices.
2. **ROMs**: Pointers to loaded ROM buffers are passed to devices via `setRom(ptr, size)` or specific setters (e.g., `setCharRom`).
3. **I/O Ports**: Port callbacks (like `setPortAWriteCallback`) are installed to handle cross-device communication (e.g., VIC-II bank selection from CIA2).
