# mmemu-plugin-pet: Commodore PET Machine Implementation

This document describes the Commodore PET machine implementation for the mmemu platform, covering the 2001, 4000, and 8000 series.

---

## 1. Intent
The PET machine plugin integrates the 6502 CPU, legacy discrete logic or CRTC video, dual PIAs, and the IEEE-488 bus into a functional simulation of the original Commodore personal computer line.

---

## 2. Hardware Composition

### 2.1 Core Components
- **CPU**: MOS 6502 (running at 1 MHz).
- **Video**: 
    - **PET 2001**: Discrete logic timing model (40 columns).
    - **PET 4032**: MOS 6545 CRTC (40 columns).
    - **PET 8032**: MOS 6545 CRTC (80 columns).
- **I/O**:
    - **PIA #1 (6520)**: IEEE-488 data, keyboard rows, and CA1/CB1 interrupts.
    - **PIA #2 (6520)**: IEEE-488 control signals.
    - **VIA (6522)**: Keyboard column selection (via 4-to-10 decoder), cassette control, and user port.
- **Bus**: IEEE-488 (GPIB) for peripheral communication.

### 2.2 Memory Map (Standard PET)
| Range | Size | Description |
|-------|------|-------------|
| $0000-$7FFF | 32 KB| User RAM (Variable by model) |
| $8000-$87FF | 2 KB | Video RAM |
| $8800-$8FFF | 2 KB | Video RAM Mirror (typical) |
| $9000-$AFFF | 8 KB | Expansion ROM sockets |
| $B000-$DFFF | 12 KB| BASIC ROM |
| $E000-$E7FF | 2 KB | Editor ROM |
| $E800-$E80F | 16 B | PIA #1 ($E810 mirror) |
| $E810-$E813 | 4 B  | PIA #1 Registers |
| $E820-$E823 | 4 B  | PIA #2 Registers |
| $E840-$E84F | 16 B | VIA Registers |
| $E880-$E881 | 2 B  | CRTC 6545 Registers (4000/8000 only) |
| $F000-$FFFF | 8 KB | KERNAL ROM |

---

## 3. Specific Devices

### 3.1 MOS 6545 CRTC
- Manages video timing and memory address generation for character fetching.
- Supports dynamic reconfiguration of screen geometry (columns/rows).
- Documentation: [README-6545.md](README-6545.md)

### 3.2 IEEE-488 Bus (`SimpleIEEE488Bus`)
- Implements the active-low logic of the parallel peripheral bus.
- **Signals**: ATN, DAV, NRFD, NDAC, EOI, SRQ, IFC, REN.
- Wired to PIA1 (Data) and PIA2 (Control).

### 3.3 Datasette (Tape)
The PET machine includes a Datasette wired through the VIA and PIA1:

| Signal | Source/Destination | Description |
|--------|--------------------|-------------|
| Motor | VIA CA2 (PCR `$E84C` bits 3–1 = `110` → manual low) | Motor on when CA2 is driven low. |
| Write | VIA PB7 (`$E840` bit 7) | Cassette write line from CPU. |
| Read pulse | PIA1 CA1 → CRA (`$E811` bit 7) | Each pulse sets the PIA1 CA1 IRQ1 flag; cleared by reading ORA (`$E810`). |
| Sense | PIA1 PB6 (input) | Asserted when tape is mounted. |

See [doc/README-DATASETTE.md](README-DATASETTE.md) for the full Datasette API.

### 3.4 Keyboard Matrix
- **Graphics Layout (2001/4000)**: 8x8 matrix wired to VIA (columns) and PIA1 (rows).
- **Business Layout (8000)**: Modified 8x8 matrix for the professional keyboard.
- Typing (CLI `type`, MCP `type_string`): capitals are plain key presses, as on a PET (SHIFT+letter gives a graphics character). Unknown key names, such as `restore`, are rejected.

### 3.5 NMI Button (not a RESTORE key)
**The PET has no RESTORE key.** On a stock PET nothing drives the 6502's NMI line, so software for a stock PET never expects an NMI, and you should not rely on one when writing for real hardware.

The ROMs do handle an NMI: the vector at `$FFFA` points to a `JMP ($0094)`, and at boot the KERNAL sets `$0094` to BASIC's warm start (`$C389` on the 2001, `$B3FF` on the 4032/8032), which prints `READY.` and keeps the program. Owners often wired a pushbutton to NMI for this, and a monitor can redirect `$0094` to itself.

mmemu models that added button as an **NMI trigger**, held while pressed:
- GUI: the **Trigger NMI** button under **Capture Keyboard** on the screen pane (shown only on the PET).
- CLI: `key nmi down` / `key nmi up`; MCP: the same key name.
- Machine JSON: a `sharedNmi` signal with an extra line (`"lines": ["nmi_button"]`) and `kbdWiring.nmiKey: {"key": "nmi", "line": "nmi_button"}`; the button is `ui.screenButtons: [{"label": "Trigger NMI", "keys": ["nmi"], ...}]`.

---

## 4. Dependencies
- **Depends on**:
    - [6502](README-6502.md): CPU core.
    - [pia6520](README-6520.md): Peripheral Interface Adapters.
    - [via6522](README-6522.md): Versatile Interface Adapter.
    - `crtc6545`: Video controller.
    - `pet-video`: Frame renderer and character ROM logic.
    - `cbm-loader`: PRG and CRT loading support.

---

## 5. Implementation Details
- **Source Location**: `src/plugins/machines/pet/`
- **Plugin**: `lib/mmemu-plugin-pet.so`
- **Registration Names**: `"pet2001"`, `"pet4032"`, `"pet8032"`
