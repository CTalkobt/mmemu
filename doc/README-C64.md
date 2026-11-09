# mmemu-plugin-c64: Commodore 64 Machine Implementation

This document describes the Commodore 64 machine implementation for the mmemu platform.

---

## 1. Intent
The C64 machine plugin composes the full Commodore 64 hardware stack — 6510 CPU, VIC-II video, dual CIA timers, SID audio, PLA banking controller, color RAM, and keyboard matrix — into a bootable simulation driven by the mmemu plugin architecture.

---

## 2. Functionality

### 2.1 Hardware Composition
- **CPU**: MOS 6510 (extends 6502 with built-in 6-bit I/O port at \$00/\$01).
- **Video**: MOS 6567 VIC-II generating a 384×272 pixel RGBA frame (including border).
- **Sound**: MOS 6581 SID — three voices with envelope generators and a state-variable filter.
- **I/O**: Two MOS 6526 CIA chips for timers, TOD clock, serial bus, and keyboard scanning.
- **Banking**: C64 PLA interprets LORAM/HIRAM/CHAREN signals from the 6510 port to overlay ROM/I/O regions on top of flat RAM.
- **Color RAM**: 1 KB × 4-bit dedicated color memory at \$D800–\$DBFF; upper nibble always reads as `\$F`.
- **Keyboard**: 8×8 matrix wired to CIA1 Port A (column select, output) and Port B (row sense, input).

### 2.2 Memory Map
| Range | Size | Description |
|-------|------|-------------|
| \$0000–\$00FF | 256 B | Zero Page (CPU port at \$00/\$01) |
| \$0100–\$01FF | 256 B | Stack |
| \$0200–\$9FFF | ~39 KB | Main RAM |
| \$A000–\$BFFF | 8 KB | BASIC ROM (banked out when LORAM=0) |
| \$C000–\$CFFF | 4 KB | RAM |
| \$D000–\$D3FF | 1 KB | VIC-II registers (CHAREN=1) / Char ROM (CHAREN=0) |
| \$D400–\$D7FF | 1 KB | SID registers (CHAREN=1) / Char ROM (CHAREN=0) |
| \$D800–\$DBFF | 1 KB | Color RAM (always I/O, 4-bit cells) |
| \$DC00–\$DCFF | 256 B | CIA1 registers (CHAREN=1) / Char ROM (CHAREN=0) |
| \$DD00–\$DDFF | 256 B | CIA2 registers (CHAREN=1) / Char ROM (CHAREN=0) |
| \$E000–\$FFFF | 8 KB | KERNAL ROM (banked out when HIRAM=0) |

### 2.3 PLA Banking Model
The 6510 I/O port (DDR at \$00, DATA at \$01) drives three banking signals into the PLA:

| Bit | Signal | Effect when low |
|-----|--------|-----------------|
| 0 | LORAM | BASIC ROM at \$A000–\$BFFF replaced by RAM |
| 1 | HIRAM | KERNAL ROM at \$E000–\$FFFF replaced by RAM |
| 2 | CHAREN | Char ROM visible at \$D000–\$DFFF (I/O hidden) |

Power-on state: DDR=\$00 (all inputs), DATA=\$3F → effective port \$3F → LORAM=1, HIRAM=1, CHAREN=1 (BASIC + KERNAL + I/O visible).

The cartridge's EXROM and GAME lines also take part; see [README-C64PLA.md](README-C64PLA.md) for the full map.

### 2.3.1 Cartridges
`cart <file.crt>` (CLI), File > Attach Cartridge (GUI) or MCP `attach_cartridge` plugs a `.crt` with the `C64 CARTRIDGE` header into the expansion port and resets; `eject` takes it out. Only the generic hardware type (0, plain ROM) is supported. As in VICE (`c64-generic.c`), the CHIP packets decide the mode:

| CHIP packets | Mode | Lines |
|--------------|------|-------|
| one at $8000, up to 8 KB | 8 KB game: ROML at $8000 | EXROM low |
| one at $8000, up to 16 KB | 16 KB game: ROML at $8000, ROMH at $A000 | EXROM and GAME low |
| one ending at $FFFF, optionally after one at $8000 | Ultimax: ROMH at $E000 (and ROML) | GAME low |

The KERNAL starts an 8 or 16 KB game through the `CBM80` signature at $8004; an Ultimax cartridge starts from its own reset vector.

### 2.4 VIC-II Bank Selection
CIA2 Port A bits 1–0 (inverted) select which 16 KB window the VIC-II DMA engine sees:

| CIA2 PA bits 1–0 | VIC bank base |
|------------------|---------------|
| 11 (default) | \$0000 |
| 10 | \$4000 |
| 01 | \$8000 |
| 00 | \$C000 |

The machine factory installs a CIA2 Port A write callback that updates `VIC2::setBankBase()` on every write.

### 2.5 Interrupt Wiring
- **IRQ** (shared open-collector): VIC-II raster IRQ and CIA1 timer/TOD underflow both feed the CPU IRQ line.
- **NMI** (wired-OR, edge-triggered): CIA2's interrupt output and the RESTORE key (`restore`, GUI Page Up). RUN/STOP+RESTORE warm-starts BASIC. The GUI's screen pane has **RUN/STOP + RESTORE** and **RESTORE** buttons under Capture Keyboard.

### 2.6 Keyboard Matrix
CIA1 Port A drives columns (output, active-low); CIA1 Port B reads rows (input, active-low). The KERNAL scans by writing `\$FE`, `\$FD`, … to Port A and reading Port B.

Key name strings passed to `MachineDescriptor::onKey`:

| Key | Name string | Key | Name string |
|-----|-------------|-----|-------------|
| Letters | `a`–`z` | Digits | `0`–`9` |
| Return | `return` | Delete | `delete` |
| Space | `space` | Shift (L) | `left_shift` |
| Shift (R) | `right_shift` | Ctrl | `ctrl` |
| CBM | `cbm` | Run/Stop | `run_stop` |
| Cursor right | `crsr_right` | Cursor down | `crsr_down` |
| F1/F3/F5/F7 | `f1` `f3` `f5` `f7` | Home | `home` |
| `+` / `-` | `plus` `minus` | `*` / `/` | `asterisk` `slash` |
| `@` / `:` | `at` `colon` | `;` / `=` | `semicolon` `equal` |
| `,` / `.` | `comma` `period` | £ | `pound` |
| ↑ (arrow up) | `arrow_up` | ← (arrow left) | `arrow_left` |

### 2.6.1 Joysticks
Two control ports on CIA1, as on the real machine (and in VICE): **control port 1** pulls Port B low, alongside the keyboard rows; **control port 2** pulls Port A low, alongside the columns. Port A's bits are outputs while the KERNAL scans the keyboard, so CIA1 reads its output bits back from the pins (`setOutputsReadPins`): a joystick on port 2 shows in `PEEK(56320)` either way. As on the hardware, moving joystick 1 can look like a key press to the KERNAL.

`onJoystick` port 0 is control port 1 ($DC01) and port 1 is control port 2 ($DC00), the one most games read (`primaryJoystickPort` = 1). Bits are active low: up, down, left, right, fire. Input: CLI `joy 1 up fire` (port 2); MCP `set_joystick`; the GUI's host controllers (the first one goes to control port 2).

In the machine JSON: `"joystick": [ {"device": "CIA1", "port": "B"}, {"device": "CIA1", "port": "A"} ], "joystickPrimary": 1`.

Not modelled: a joystick on port 2 holding a column line low does not change which keys the keyboard matrix reports.

### 2.7 ROM Files
Required in `roms/c64/` (not distributed; see `.gitignore`):

| File | Size | Contents |
|------|------|----------|
| `basic.bin` | 8 KB | BASIC V2 ROM (\$A000–\$BFFF) |
| `kernal.bin` | 8 KB | KERNAL ROM (\$E000–\$FFFF) |
| `char.bin` | 4 KB | Character ROM |

If any ROM file is absent the machine still initialises; the affected region reads as zeroes.

### 2.8 Datasette (Tape)
The C64 machine includes a Datasette device wired to the 6510 I/O port and CIA1:

| Signal | Source/Destination | Description |
|--------|--------------------|-------------|
| Motor | 6510 port bit 5 (active-low) | Enables tape transport when CPU drives bit 5 low. |
| Write | 6510 port bit 3 | Cassette write line from CPU to tape head. |
| Read pulse | CIA1 FLAG (`\$DC0D` bit 4) | Each incoming pulse sets the CIA1 FLAG interrupt. |
| Sense | 6510 port bit 4 (input) | Asserted by Datasette when a button is pressed/tape mounted. |

See [doc/README-DATASETTE.md](README-DATASETTE.md) for the full Datasette API.

### 2.9 Scheduler
One `schedulerStep` call executes one CPU instruction (via `ICore::step()`), then ticks all IORegistry devices by the returned cycle count. The GUI timer calls this in a tight loop for ~33 333 cycles per 33 ms frame (~1 MHz effective).

While a device holds the bus (an REU's DMA: `MachineDescriptor::busMasterStep`), each step runs one cycle of that instead of an instruction, and the devices tick as usual.

### 2.10 REU (RAM Expansion Unit)
A Commodore REU can be fitted in the expansion port (the machine's `port` expansion slot) once the machine is made:

```
expansion port reu 512K        # CLI: a 1750
expansion port reu 16M         # the largest
expansion port reu ram.reu     # a VICE .reu image (its size picks the unit)
expansion port save ram.reu    # the RAM as an image
expansion port remove
```

MCP `expansion` (`slot`, `device`, `option`, `save`) and the GUI's Control > Expansions... do the same. The machine resets after a change; the REU's RAM survives later resets.

It is the `reu` device plugin (`src/plugins/devices/reu/`), after VICE's `src/c64/cart/reu.c`:

| Reg | |
|-----|--|
| `$DF00` | status (a read clears bits 5-7 and the IRQ): IRQ, end of block, verify error, 256K chips, version 0 |
| `$DF01` | command: bit 7 execute, 5 autoload, 4 no `$FF00` trigger, 0-1 the type (C64 to REU, REU to C64, swap, verify) |
| `$DF02-$DF03` | the C64 address |
| `$DF04-$DF06` | the REU address and bank |
| `$DF07-$DF08` | the length (0 is 64 KB) |
| `$DF09` | the IRQ mask: bit 7 on, 6 end of block, 5 verify error |
| `$DF0A` | address control: bit 7 fixes the C64 address, 6 the REU address |

- The registers repeat every 32 bytes up to `$DFFF`; `$DF0B-$DF1F` read `$FF`.
- **Starting a transfer:** execute with bit 4 set starts at once. With bit 4 clear, the REU waits for any write to `$FF00`; the write still goes on to the RAM under the KERNAL.
- **Timing:** the CPU waits while the REU transfers one byte a cycle (a swap takes two).
  - Verify stops at the first difference, plus one cycle when bytes were left. With one byte left, that byte is compared too.
  - A finished transfer leaves the registers past the block, with length 1. With autoload they are reloaded instead.
- **What the DMA sees:** the CPU's memory map, so I/O and ROMs are included. The 6510's port at `$00/$01` is not on the bus, so a transfer there writes the RAM underneath.
- **Sizes:**
  - 128K (1700, 64K chips: status bit 4 clear, wraps at 128 KB), 256K (1764) and 512K (1750).
  - 1M-16M behave like the "1750 XL" style units.
  - The REC's address counter wraps at 512 KB. A 1764's upper 256 KB are unbacked: reads float and writes are lost.
  - Above 512 KB the bank register's upper bits are a latch straight to the DRAM, so those units wrap at their size and each 512 KB wraps within itself.
- **IRQ:** the REU's IRQ goes to the expansion port's /IRQ, which is the `expansion_irq` line of the machine's shared IRQ.

Not modelled:
- Badlines and sprite fetches don't pause the DMA. The C64 machine has no VIC-II cycle stealing yet.
- The REU's pass-through doesn't arbitrate with a cartridge that also uses I/O2.

The slot is declared in `machines/c64.json` (`"expansions"`). Any JSON machine can offer devices this way: a device type, its options, an IRQ line and whether it can take the bus.

---

## 3. Dependencies
- [mmemu-plugin-6502](README-6502.md): Provides the MOS 6510 execution core (registered as `"6510"`).
- [mmemu-plugin-c64-pla](README-C64PLA.md): PLA banking controller.
- [mmemu-plugin-cia6526](README-6526.md): CIA1 (\$DC00) and CIA2 (\$DD00) — timers, TOD, I/O ports.
- [mmemu-plugin-vic2](README-VIC2.md): VIC-II video and raster IRQ.
- [mmemu-plugin-sid6581](README-SID.md): SID audio synthesis.

---

## 4. Implementation Details
- **Source location**: `src/plugins/machines/c64/`
- **Plugin**: `lib/mmemu-plugin-c64.so`
- **Registration name**: `"c64"`
- **Display name**: `"Commodore 64"`
- **License class**: `"proprietary"` (ROM images are not redistributable)
