# mmemu-plugin-c128: Commodore 128

The Commodore 128 in C128 mode, with the 40-column VIC-IIe screen (#22).
Memory map, MMU and keyboard after VICE (`src/c128/c128mem.c`,
`c128meminit.c`, `c128mmu.c`, `data/C128/gtk3_sym.vkm`).

## Profiles

| Profile | Machine |
|---------|---------|
| `c128` | Commodore 128, PAL (VIC-IIe 8566, 985248 Hz) |
| `c128-ntsc` | Commodore 128, NTSC (8564, 1022727 Hz) |

Both boot to BASIC 7.0 READY ("122365 BYTES FREE").

## Hardware

- **CPU**: MOS 8502, a 6510 with a seventh port pin: bit 6 reads the CAPS
  LOCK (ASCII/DIN) key. 1 MHz, or 2 MHz while VIC-IIe `$D030` bit 0 is set
  (BASIC `FAST`); time is counted at the 1 MHz clock.
- **MMU** (8722) with 128 KB RAM in two banks: see below.
- **Video**: 8564/8566 VIC-IIe, the C64's VIC-II plus `$D02F` (the extra
  keyboard lines K0-K2) and `$D030` (2 MHz, test). It fetches from the RAM
  bank RCR bit 6 picks, in the 16 KB bank CIA2 picks, and sees the C128
  character set at $x1000-$x1FFF of every 16 KB unless 8502 port bit 2 is
  set.
- **VDC** (8563) at `$D600`: registers, RAM (16 KB), auto-increment, block
  fill and copy, the status register. The KERNAL's screen editor sets it up
  at reset. The 80-column display itself is not emulated yet.
- **Sound**: 6581 SID at `$D400`.
- **CIAs**: CIA1 `$DC00` (keyboard, joysticks, IRQ, tape read on FLAG),
  CIA2 `$DD00` (VIC bank, serial bus, NMI with RESTORE).
- **Colour RAM**: 2 KB; 8502 port bit 0 picks the CPU's 1 KB half, bit 1 the
  VIC's.
- **Serial bus**: a line-level drive on unit 8 (CIA2 port A), serving the
  current folder or `disk mount 8 <folder or image>`; LOAD, SAVE and
  DIRECTORY run through the real KERNAL (no 1571 fast serial yet).
- **Datasette** on the 8502 port (motor, write, sense) and CIA1 FLAG.

## Memory map (C128 mode)

The configuration register CR is at `$FF00` (always) and `$D500` (with I/O):

| CR bits | Area | Values |
|---------|------|--------|
| 0 | $D000-$DFFF | 0 I/O, 1 the high area's ROM or RAM |
| 1 | $4000-$7FFF | 0 BASIC low, 1 RAM |
| 2-3 | $8000-$BFFF | 0 BASIC high, 1 internal function ROM, 2 external, 3 RAM |
| 4-5 | $C000-$FFFF | 0 KERNAL (screen editor at $C000, character ROM at $D000 while I/O is off), 1/2 function ROMs, 3 RAM |
| 6-7 | all RAM | the bank (0 or 1) |

$0000-$3FFF is always RAM. `$FF00-$FF04` are always the MMU: a write to
`$FF01-$FF04` copies the preconfiguration register A-D (`$D501-$D504`) into
CR. Function ROMs are not fitted (they read `$FF`). Writes under a ROM go to
the RAM there.

Other MMU registers (`$D505-$D50B`):

- **MCR** `$D505`: bit 0 the CPU (1 = 8502), bit 6 C64 mode, bit 7 reads the
  40/80 key (1 = up, 40 columns), bits 4/5 the cartridge's GAME/EXROM.
- **RCR** `$D506`: shared RAM, 1, 4, 8 or 16 KB at the bottom (bit 2) and/or
  top (bit 3) of every bank, taken from bank 0; bit 6 the VIC-II's RAM bank.
- **P0L/P0H, P1L/P1H** `$D507-$D50A`: where page 0 and page 1 are; the page
  they move to answers at $0000/$0100 (they swap). The high byte is latched
  until the low byte is written.
- **Version** `$D50B`: reads `$20`.

`$D50C-$D5FF` read `$FF`.

## Keyboard

The C64 matrix on CIA1, plus three lines K0-K2 driven by `$D02F` and read on
CIA1 port B:

| Line | Bits 0-7 |
|------|----------|
| K0 | HELP, 8, 5, TAB, 2, 4, 7, 1 (keypad) |
| K1 | ESC, +, -, LINE FEED, ENTER, 6, 9, 3 |
| K2 | ALT, 0, ., up, down, left, right, NO SCROLL |

Key names: the C64's (`run_stop`, `cbm`, `ctrl`, `crsr_right`...), and
`help`, `esc`, `tab`, `alt`, `line_feed`, `no_scroll`, `kp0`-`kp9`,
`kp_plus`, `kp_minus`, `kp_period`, `kp_enter`, `up`, `down`, `left`,
`right` (the separate cursor keys). As in VICE: F9 = ESC, F10 = TAB,
F11 = ALT, End = HELP, Page Down = LINE FEED, F12 = CAPS LOCK. CAPS LOCK
(`caps_lock`) and 40/80 (`40_80`) lock: each press toggles them. RESTORE is
`restore` (Page Up in the GUI).

## ROMs

From `roms/c128/` (ftp.funet.fi, the US set VICE uses):

- `basic-4000.318018-04.bin`, `basic-8000.318019-04.bin` - BASIC 7.0
- `kernal.318020-05.bin` - screen editor ($C000), Z80 boot BIOS ($D000),
  KERNAL ($E000)
- `characters.390059-01.bin` - the C64 character set, then the C128 one
- `c128_c64part.325182-01.bin` - BASIC and KERNAL for C64 mode (not used yet)

## Not yet emulated

- The VDC's 80-column display (the 40/80 key down at reset selects it)
- C64 mode (`GO64`, the C= key held at reset)
- The Z80 and CP/M: the machine starts on the 8502, where the Z80 boot
  BIOS would hand over
- Function ROMs, cartridges, 1571 fast serial, the VIC-IIe's 2 MHz effect
  on the display
