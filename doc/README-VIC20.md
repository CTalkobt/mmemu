# mmemu-plugin-vic20: Commodore VIC-20 Machine Implementation

This document describes the Commodore VIC-20 machine implementation for the mmemu platform.

---

## 1. Intent
The VIC-20 machine plugin acts as the composition layer that integrates individual hardware components (CPU, memory, video, I/O) into a coherent, bootable simulation of the classic Commodore VIC-20 home computer.

---

## 2. Functionality

### 2.1 Hardware Composition
- **CPU**: Instantiates a MOS 6502 core.
- **Video**: Instantiates a MOS 6560 VIC-I chip.
- **I/O**: Instantiates two MOS 6522 VIA chips.
- **Memory**: Configures a 64 KB address space with appropriate mappings for RAM, ROM, and I/O windows.

### 2.2 Memory Map
| Range | Size | Description |
|-------|------|-------------|
| $0000-$03FF | 1 KB | Zero Page, Stack, System Vectors |
| $0400-$0FFF | 3 KB | Expansion Area (User RAM) |
| $1000-$1DFF | 3.5KB| Main User RAM |
| $1E00-$1FFF | 0.5KB| Screen RAM (Default) |
| $8000-$8FFF | 4 KB | Character ROM |
| $9000-$900F | 16 B | VIC-I Registers |
| $9110-$911F | 16 B | VIA #1 Registers |
| $9120-$912F | 16 B | VIA #2 Registers |
| $9400-$97FF | 1 KB | Color RAM |
| $C000-$DFFF | 8 KB | BASIC ROM |
| $E000-$FFFF | 8 KB | KERNAL ROM |

### 2.3 Datasette (Tape)
The VIC-20 machine includes a Datasette wired through VIA2 and VIA1:

| Signal | Source/Destination | Description |
|--------|--------------------|-------------|
| Motor | VIA2 PB3 (`$9120` bit 3, active-low) | Motor on when CPU drives bit 3 low. |
| Write | VIA2 PB7 (`$9120` bit 7) | Cassette write line from CPU. |
| Read pulse | VIA1 CA1 → IFR (`$911D` bit 1) | Each pulse sets the VIA1 CA1 interrupt flag. |
| Sense | VIA2 PB6 (input) | Asserted when tape is mounted. |

See [doc/README-DATASETTE.md](README-DATASETTE.md) for the full Datasette API.

### 2.3.1 Joystick
One control port, wired as on the real machine (and in VICE's `vic20via1.c`/`vic20via2.c`):

| Switch | Line | Register |
|--------|------|----------|
| Up | VIA1 PA2 | `$911F`/`$9111` bit 2 (37151, 37137) |
| Down | VIA1 PA3 | bit 3 |
| Left | VIA1 PA4 | bit 4 |
| Fire | VIA1 PA5 | bit 5 |
| Right | VIA2 PB7 | `$9120` bit 7 (37152), shared with keyboard column 7 |

Bits read 0 while pressed. To read right, a program makes PB7 an input first: `POKE 37154,127:R=PEEK(37152)AND128:POKE 37154,255`.

`onJoystick` port 0 (the only one). Input: CLI `joy 0 up fire`, MCP `set_joystick`, or a host USB controller in the GUI. In the machine JSON: `"joystick": {"lines": [{"device": "VIA1", "port": "A", "up": 2, "down": 3, "left": 4, "fire": 5}, {"device": "VIA2", "port": "B", "right": 7}]}`.

Not modelled: holding right does not change which keys the keyboard matrix reports (on the hardware it makes column 7 look selected).

### 2.4 Scheduler
- Implements a synchronous cycle-stepper that ticks all I/O devices (`tickAll`) in lock-step with CPU instruction execution.

---

## 3. Dependencies
This plugin is a **compositional node** and has heavy dependencies on other plugins:
- **Depends on**:
    - [mmemu-plugin-6502](README-6502.md): Provides the execution core.
    - [mmemu-plugin-6522](README-6522.md): Provides the I/O and timers.
    - [mmemu-plugin-6560](README-6560.md): Provides the video generation.
    - [mmemu-plugin-kbd-vic20](README-KBD-VIC20.md): Provides the keyboard matrix.

---

## 4. Implementation Details
- **Source Location**: `src/plugins/machines/vic20/`
- **Plugin**: `lib/mmemu-plugin-vic20.so`
- **Registration Name**: `"vic20"`
