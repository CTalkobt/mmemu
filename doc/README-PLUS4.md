# mmemu-plugin-plus4: Commodore Plus/4 and C16

The Commodore 264 family (#9): a 7501/8501 CPU (a 6502 with an I/O port at
$00/$01) clocked by the TED (MOS 7360/8360), which also does the video,
sound, timers and keyboard/joystick input. See [README-TED.md](README-TED.md)
for the chip itself. The memory map and ROM banking follow VICE
(`plus4mem.c`, `ted-mem.c`).

## Profiles

| Profile | Machine | RAM | Clock |
|---------|---------|-----|-------|
| `plus4` | Plus/4, PAL | 64 KB | 1,773,447 Hz (TED double clock) |
| `plus4-ntsc` | Plus/4, NTSC | 64 KB | 1,789,772 Hz |
| `c16` | C16, PAL | 16 KB (mirrored) | as `plus4` |
| `c16-ntsc` | C16, NTSC | 16 KB (mirrored) | as `plus4-ntsc` |

The C16 has no 3-plus-1 ROM and no ACIA. The CPU runs single clock while the
TED fetches the display and stalls on bad lines and attribute lines, as on
the real machine.

## Memory map

| Range | Contents |
|-------|----------|
| $0000-$0001 | 7501 I/O port (tape, IEC, reads `& $DF`) |
| $0002-$7FFF | RAM (C16: 16 KB mirrored through $0000-$FFFF) |
| $8000-$BFFF | RAM, or the low ROM bank: BASIC 3.5, 3-plus-1 low, cartridge 1, cartridge 2 |
| $C000-$FFFF | RAM, or the high ROM bank: KERNAL, 3-plus-1 high, cartridges |
| $FC00-$FCFF | always the KERNAL while ROM is on |
| $FD00-$FD0F | 6551 ACIA (Plus/4; not emulated yet) |
| $FD10-$FD1F | 6529 user port latch |
| $FD30-$FD3F | 6529 keyboard row latch |
| $FDD0-$FDDF | ROM bank select: address bits 0-1 pick the low bank, bits 2-3 the high bank |
| $FF00-$FF3F | TED registers; a write to $FF3E selects ROM, $FF3F RAM |

The TED fetches screen, colour and character data through the same banking
(its "fetch from ROM" bit picks ROM or RAM for character data).

## ROMs

From `roms/plus4/` (ftp.funet.fi, as in VICE):

- `basic.318006-01.bin` - BASIC 3.5
- `kernal.318004-05.bin` (PAL) / `kernal.318005-05.bin` (NTSC)
- `3-plus-1.317053-01.bin`, `3-plus-1.317054-01.bin` - the built-in
  software (Plus/4 only; F1 at the READY prompt starts it)

## Keyboard

The 8x8 matrix is read by writing a row mask to the $FD30 latch and a column
mask to $FF08; the TED latches the result. The host map follows VICE's
the matrix follows VICE's Plus/4 keymaps. Key names (as used by `type`,
`key` and the GUI) include `run_stop`, `home` (CLR/HOME), `delete`
(INST/DEL; `insert` is shifted), `control`, `cbm`, `escape` (the Plus/4 ESC
key), `help`, `pound` and `f1`-`f3`. Aliases: `alt`/`commodore` -> `cbm`,
`ctrl` -> `control`, `backspace` -> `delete`, `f8` -> `help`, `clr` -> `home`,
`kpeq` -> `equal`. Shifted keys: `f4`-`f6` are shifted `f1`-`f3`, `f7` is
shifted `help` (the key cap reads F7/HELP).

Both joystick ports are read through $FF08 (port 1 with bit 2 low, port 2
with bit 1 low).

## Loading and saving

The `kernal-fileio` intercept (`intercept set kernal-fileio path DIR`) maps
device 8 to a host directory, as for the C64 and VIC-20. BASIC 3.5 keeps its
strings in the RAM under the ROM, so the machine supplies
`MachineDescriptor::readRam` and the KERNAL HLE reads file names and SAVE
data through it.

## Not yet emulated

- 6551 ACIA, tape (1531) and the IEC serial bus (real drives)
- Cartridges
- C116 and the 4 KB / 32 KB RAM variants
