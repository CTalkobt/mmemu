# mmemu-plugin-c128: Commodore 128

The Commodore 128 in C128 mode, with the 40-column VIC-IIe and 80-column VDC screens, in C64 mode, and with its Z80, booting CP/M 3.0 (#22).
Memory map, MMU and keyboard after VICE (`src/c128/c128mem.c`,
`c128meminit.c`, `c128mmu.c`, `data/C128/gtk3_sym.vkm`).

## Profiles

| Profile | Machine |
|---------|---------|
| `c128` | Commodore 128, PAL (VIC-IIe 8566, 985248 Hz) |
| `c128-ntsc` | Commodore 128, NTSC (8564, 1022727 Hz) |

Both boot to BASIC 7.0 READY ("122365 BYTES FREE").

## Hardware

- **CPUs**: Zilog Z80 (boot BIOS, CP/M; see below) and MOS 8502, a 6510 with a seventh port pin: bit 6 reads the CAPS
  LOCK (ASCII/DIN) key. 1 MHz, or 2 MHz while VIC-IIe `$D030` bit 0 is set
  (BASIC `FAST`); time is counted at the 1 MHz clock.
- **MMU** (8722) with 128 KB RAM in two banks: see below.
- **Video**: 8564/8566 VIC-IIe, the C64's VIC-II plus `$D02F` (the extra
  keyboard lines K0-K2) and `$D030` (2 MHz, test). It fetches from the RAM
  bank RCR bit 6 picks, in the 16 KB bank CIA2 picks, and sees the C128
  character set at $x1000-$x1FFF of every 16 KB unless 8502 port bit 2 is
  set.
- **VDC** (8563) at `$D600`: registers, RAM (16 KB), auto-increment, block
  fill and copy, the status register (vertical blank from the frame timing),
  and the 80-column display: see below.
- **Sound**: 6581 SID at `$D400`.
- **CIAs**: CIA1 `$DC00` (keyboard, joysticks, IRQ, tape read on FLAG),
  CIA2 `$DD00` (VIC bank, serial bus, NMI with RESTORE).
- **Colour RAM**: 2 KB; 8502 port bit 0 picks the CPU's 1 KB half, bit 1 the
  VIC's.
- **Serial bus**: a line-level drive on unit 8 (CIA2 port A), serving the
  current folder or `disk mount 8 <folder or image>` (images are written
  back); LOAD, SAVE and
  DIRECTORY run through the real KERNAL (no 1571 fast serial yet).
- **Datasette** on the 8502 port (motor, write, sense) and CIA1 FLAG.

## The Z80 and CP/M

The C128 powers on as the real one does: the **Z80** runs first, from its
boot BIOS (the KERNAL ROM's `$D000-$DFFF`), checks for a C64 cartridge and
the C= key, points the 8502's RAM vectors at a start-up routine and hands
over through MCR bit 0; the 8502 then takes its reset vector (as in VICE).
MCR bit 0 switches between the CPUs at any time (CP/M does it for every disk
access). The Z80 runs at an effective 2 MHz (its 4 MHz clock waits for the
VIC-II) and shares /IRQ with the 8502. The CLI's `cpu` lists both CPUs and
`cpu z80` / `cpu main` picks the one `regs`, `disasm` and `m` show.

The Z80's view (VICE `z80mem.c`, C128 mode): as the 8502's map, except that
in bank 0 `$0000-$0FFF` reads the BIOS (writes go to RAM at `$D000-$DFFF`),
with I/O in `$1000-$13FF` is colour RAM, and `$D000-$DFFF` is always RAM
(no I/O, no character ROM). I/O is reached with IN/OUT: ports `$D000-$DFFF`
are the chips (the MMU only while CR has I/O in), ports `$0000-$0FFF` RAM at
`$D000-$DFFF` of bank 0, any other port the memory at that address.

**CP/M 3.0**: mount a C128 CP/M boot disk and reset; the KERNAL finds the
boot sector ("BOOTING ..."), loads CP/M and starts it on the Z80, in 40 or 80
columns by the 40/80 key:

```
disk mount 8 cpm-boot.d64
reset
```

Commodore's 28 May 87 boot disk boots to `A>` and runs DIR (checked with
`28may87-1.d64` from the funet archive, `pub/cpm/sys/c128/system`). No disk
is included: Commodore and Digital Research hold the copyrights and the
images carry no distribution terms. Disk access goes through the 8502 and
the KERNAL's serial routines (no 1571 fast serial yet), so loading CP/M takes
about a minute of machine time, as with a 1541. CP/M writes its sectors with
`U2`, which go straight to the image file (`REN`, `ERA`, `PIP` and saving
work; use a copy of a disk you want to keep unchanged).

## Function ROMs and cartridges

The C128 has two 32 KB function ROM slots (VICE `functionrom.c`,
`c128cart.c`): the **internal** one (a socket on the board) and the
**external** one (a cartridge). CR shows them where it picks 1 (internal) or
2 (external): the low 16 KB at `$8000-$BFFF` (bits 2-3), the high 16 KB at
`$C000-$FFFF` (bits 4-5, `$D000-$DFFF` too while I/O is off). A slot with
nothing fitted reads `$FF`.

`cart <file>` (CLI), File > Attach Cartridge (GUI) or MCP `attach_cartridge`
plugs a cartridge in and resets; `eject` takes it out; the Cartridge pane
shows it. As in VICE (`c128/cart/generic.c`):

| Image | Layout |
|-------|--------|
| raw `.bin`, 1 to 32 KB (a 2-byte load address is skipped) | from `$8000`, mirrored up to 32 KB |
| `.crt` with the `C128 CARTRIDGE  ` header, hardware type 0 | up to two CHIP packets, at `$8000` and `$C000`, up to 16 KB each, mirrored within their 16 KB |

At reset the KERNAL polls the four halves (`$E26B`): one with `CBM` at
`$xx07` is noted, and one with `$01` at `$xx06` is called at `$xx00` (a
cartridge that starts itself). `.bin` and `.crt` are shared with the Plus/4
and the C64: each machine's handler is tried in turn, and the one that fits
the machine takes it. The internal ROM has no menu yet: `c128::setInternalFunctionRom`.

## Screens: 40 and 80 columns

The frontends (GUI screen, CLI `screenshot`, MCP) show one screen, picked by
the **40/80 key** as VICE's single-window builds do: up shows the VIC-II's 40
columns, down the VDC's 80. The key locks, and the screen editor reads it at
reset to choose where BASIC prints: press it (`key 40_80 1`, or the GUI's
40/80 button) and reset for an 80-column start. Both screens are 768 x 544:
the VIC-II's frame doubled, the VDC's 768-dot lines drawn twice.

The VDC display follows VICE (`vdc-draw.c`, `vdc.c`), drawn a frame at a
time from the registers:

- Text: R1 columns of (R22 >> 4) + 1 dots, doubled in 40-column mode (R25
  bit 4), R6 rows of (R9 & 31) + 1 lines, character data on lines 0-R23;
  the screen at R12/R13, the character set at R28 bits 5-7 (16 or 32 bytes a
  character), R27 extra bytes a row.
- Attributes (R25 bit 6) at R20/R21: colour (bits 0-3), alternate character
  set (7), reverse (6), underline on line R29 (5), blink (4, rate R24 bit
  5). Without attributes R26 gives both colours (foreground high nibble).
- Bitmap mode (R25 bit 7): a byte per 8 dots a line; with attributes the
  low nibble is the foreground and the high nibble the background.
- The cursor at R14/R15, lines R10-R11, solid / off / blinking (R10 bits
  5-6); reverse screen (R24 bit 6); semigraphics (R25 bit 5); the R22
  displayed width; smooth scroll (R24 and R25 low bits).
- The 16 RGBI colours; the frame lasts (R4 + 1) x ((R9 & 31) + 1) + R5 lines
  of (R0 + 1) x 8 dots at 16 MHz.

Not drawn: interlace, register changes in mid-frame, the sync and
display-enable window (R2, R3, R7, R34, R35).

## C64 mode

`GO64` (and Y), or C= held while the machine resets, sets MCR bit 6 and the
C128 becomes a C64 until the next reset (which returns to C128 mode; keys
held through a reset stay down). As in VICE (`c128mem.c`, `c128mmu.c`):

- The memory map is the C64's, from the 8502 port's LORAM/HIRAM/CHAREN
  (VICE's configurations 0-7; no cartridge): BASIC 64 at $A000, KERNAL 64 at
  $E000, I/O or the C64 character set at $D000, RAM elsewhere.
- RAM is the bank CR held at the switch; RCR's shared areas come from bank 0.
- There is no MMU: `$D500-$D5FF` read open bus and `$FF00` is plain ROM/RAM.
  The VDC stays at `$D600`, and the VIC-IIe keeps `$D02F`/`$D030` (2 MHz
  works in C64 mode).
- Colour RAM is the first 1 KB for both CPU and VIC-II; the VIC-II sees the
  C64 character set at $1000 and $9000 (banks 0 and 2), as on the C64.

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
`restore` (Page Up in the GUI). The GUI's screen buttons: 40/80, RUN/STOP + RESTORE, RESTORE.

## ROMs

From `roms/c128/` (ftp.funet.fi, the US set VICE uses):

- `basic-4000.318018-04.bin`, `basic-8000.318019-04.bin` - BASIC 7.0
- `kernal.318020-05.bin` - screen editor ($C000), Z80 boot BIOS ($D000),
  KERNAL ($E000)
- `characters.390059-01.bin` - the C64 character set, then the C128 one
- `c64.251913-01.bin` - BASIC and KERNAL for C64 mode: the first half of
  `complete.318023-02.bin`, identical to the C64's 901226-01 / 901227-03

## Not yet emulated

- VDC interlace and mid-frame register changes (raster effects)
- Cartridges in C64 mode (EXROM/GAME), the C64 mode page 0/1 relocation
  corner cases
- The Z80 in C64 mode beyond the C64 memory map
- Bank-switching C128 cartridges, an internal function ROM menu, 1571 fast
  serial, the VIC-IIe's 2 MHz effect on the display
