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
| $FD00-$FD0F | 6551 ACIA (Plus/4) |
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

Unit 8 is a serial-bus (IEC) drive at the line level (`IecDrive`,
`src/plugins/devices/iec_drive/`): the KERNAL's own serial routines talk to
it through the 7501 port ($01 bit 0 DATA, 1 CLK, 2 ATN out, a 1 pulling the
line low; bits 7 DATA and 6 CLK in, 1 = released). It serves the current
directory by default; `disk mount 8 <folder>` serves another folder and
`disk mount 8 <image>` a D64/D71/D80/D81/D82 image, through the same DOS as
the PET's IEEE-488 drive. LOAD, SAVE, VERIFY, `DIRECTORY`, `LOAD"$",8`,
OPEN/PRINT#/INPUT# and the error channel (`OPEN 15,8,15`) all work; a missing
file gives FILE NOT FOUND as on a 1541.

Transfers run at the speed of the serial protocol (about 1.4 ms per byte as
the drive sends), so a 10 KB program takes some 14 s of emulated time;
`--speed max` runs that in about a second.

Units 9 and up go to the `kernal-fileio` intercept
(`intercept set kernal-fileio path DIR`). BASIC 3.5 keeps its strings in the
RAM under the ROM, so the machine supplies `MachineDescriptor::readRam` and
the KERNAL HLE reads file names and SAVE data through it.

## Tape

A 1531 datasette (device `Tape`) sits on the cassette port: motor on $01
bit 3 (low = running), write on bit 1 (shared with the serial CLK output),
read on bit 4, and the sense switch on the 6529 at $FD10 bit 2 (low = a
button down).

```
tape mount game.tap      # insert and press PLAY, then LOAD
tape record              # press RECORD & PLAY, then SAVE"NAME"
tape stoprecord
tape save out.tap        # write what was recorded
```

The read line is a level that flips on each half-wave, as in VICE's C16 tape
behaviour. A `C16-TAPE-RAW` version 2 TAP holds one half-wave per value
(value x 16 TED cycles); version 0/1 TAPs (C16 or C64 headers) hold full
waves, played as two equal halves. Recordings are saved as C16 version 2
TAPs (VICE writes version 1 for the Plus/4), starting on a falling edge of
the write line, the start of a wave.

## RS-232 (Plus/4)

A MOS 6551 ACIA (`Acia6551`, `src/plugins/devices/acia6551/`, after VICE's
`aciacore.c`) at $FD00 on the IRQ: data, status (a write is a programmed
reset), command and control registers; 50-19200 bps with characters taking
their real time on the wire; receive-full, overrun and transmit IRQs. The
C16 has no ACIA.

The far end is set with the CLI `serial` command:

```
serial loopback          # what is sent comes back
serial tcp:25232         # a TCP server on 127.0.0.1:25232; one client is the modem
serial none              # nothing attached (DCD and DSR not asserted)
```

The KERNAL sends from its IRQ handler, so open the channel with transmit
IRQs on, e.g. 1200 bps 8N1:

```
OPEN 2,2,0,CHR$(8)+CHR$(5)      : REM control $08, command $05
PRINT#2,"HELLO";
GET#2,A$
```

With a far end attached, ST reads 64 after RS-232 I/O: the KERNAL inverts
the ACIA's DSR bit into ST bit 6 (`EOR #$40` at $EB30).

## Not yet emulated

- Tape fast-forward/rewind timing, wobble and the motor's run-down delay
- Fast-loaders and drive code (`M-E`/`M-W`): the drive is a DOS, not an emulated 1541 CPU
- Cartridges
- C116 and the 4 KB / 32 KB RAM variants
