# mmemu-plugin-trs80: TRS-80 Model I, III, 4 and 4P

Tandy's Z80 TRS-80s (#330): the Model I (Level I or Level II BASIC, with
the Expansion Interface), the Model III, the Model 4 and the Model 4P. The
Z80 is mmsim's (`src/plugins/z80`), the floppy controller its WD17xx
(`src/plugins/devices/wd17xx`: the FD1771 and FD1793). The memory maps,
ports and interrupt wiring follow the Radio Shack technical reference
manuals, xtrs (Tim Mann; MIT-style licence) and MAME (`trs80.cpp`,
`trs80m3.cpp`; BSD-3-Clause); mmsim's own code.

## Profiles

| Profile | Machine |
|---------|---------|
| `trs80m1` | Model I, Level II BASIC, 1.774 MHz, the Expansion Interface: 48 KB, FD1771, printer, 40 Hz clock |
| `trs80m1l1` | Model I, Level I BASIC, 4 KB (no Expansion Interface) |
| `trs80m3` | Model III, 2.028 MHz, 48 KB, FD1793 (4 drives), RS-232 board |
| `trs80m4` | Model 4, 2.028 / 4.055 MHz, 128 KB, FD1793, RS-232 board |
| `trs80m4p` | Model 4P: the Model 4 with a 4 KB boot ROM that loads `MODELA/III` from disk |

```
bin/mmemu-cli -m trs80m1                  # Level II: MEMORY SIZE?
bin/mmemu-cli -m trs80m3 -i ldos.dsk      # boot a DOS (JV1, JV3, DMK)
bin/mmemu-cli -m trs80m4 -i lsdos6.dmk    # LS-DOS 6 in Model 4 mode
bin/mmemu-gui -m trs80m1 -i game.cas      # then CLOAD (or SYSTEM)
bin/mmemu-cli -m trs80m1 -i scripsit.cmd  # a /CMD program, run at its transfer address
```

## ROMs

The ROMs are Tandy's and Microsoft's: they are not in mmsim. Put your own
in `roms/trs80/` (ignored by git), or point `MMEMU_TRS80_FIRMWARE` at
their directory:

| File | Contents |
|------|----------|
| `level1.rom` | Model I Level I BASIC, 4 KB |
| `level2.rom` (or MAME's `rom-a.z1`, `rom-b.z2`, `rom-c.z3`) | Model I Level II BASIC, 12 KB |
| `model3.rom` (or MAME's `8041364.u104`, `8040332.u105`, `8040316c.u106`) | Model III, 14 KB |
| `model4.rom` (MAME's `trs80m4.rom`; else the Model III's) | Model 4, 14 KB |
| `model4p.rom` (MAME's `8075332.u69`) | Model 4P boot ROM, 4 KB |
| `mcm6670p.z29` (optional) | The Model I's character generator, 1 KB |
| `8044316a.u36` / `8049007.u103` (optional) | The Model III/4 / 4P character generator, 2 KB |
| `esf.rom` (optional) | The Exatron Stringy Floppy's ROM (Model I, up to 2016 bytes at `$3000`) |

Without its ROM a machine is still made (the debugger and the plugin
checks work), with a warning naming the file and where it looked; a
stand-in ROM puts the same message on the screen. Without a character
generator ROM a built-in font is used (mmsim's own: the upper case from
its MC6847 glyphs, the lower case from its MC6847T1's); the Model III/4's
special characters (`$C0-$FF`) are then stand-ins (inverted letters; the
alternate set underlined).

`roms/trs80/hardware.sym` (checked in) names the memory-mapped I/O for the
debugger (`FDC_STATUS`, `KBD_ROW0`, `VIDEO`...). The Z80 disassembler,
assembler and GDB register layout are the Z80 plugin's.

## Memory maps

**Model I**

| Address | |
|---------|--|
| `$0000-$2FFF` | ROM (Level I: `$0000-$0FFF`) |
| `$37E0-$37E3` | read: the interrupt latch (7 clock, 6 FDC); write: drive select (bits 0-3; bit 3 is also the side for drives 0-2) |
| `$37E4` | cassette 1/2 select (bit 0: the second recorder) |
| `$37E8` | printer: data out, status in (`$30` ready) |
| `$37EC-$37EF` | FD1771: status/command, track, sector, data |
| `$3000-$37DF` | the Exatron Stringy Floppy's ROM (when fitted) |
| `$3800-$3BFF` | keyboard (address bits 0-7 select rows) |
| `$3C00-$3FFF` | video RAM (D6 not stored without the lower case mod) |
| `$4000-` | RAM: 4 or 16 KB in the keyboard unit, to 48 KB with the EI |

`$37E0-$37EF` exist only with the Expansion Interface. With no disk in any
drive the FDC's status reads `$FF` (as xtrs), so the ROMs skip the disk
boot and start BASIC.

Port `$FF`: write bits 1-0 the cassette output, 2 the motor relay, 3 the
32-column mode; read bit 7 the cassette flip-flop, bit 6 NOT 32 columns.

**Model III**: ROM `$0000-$37FF` (the printer's status over `$37E8`),
keyboard `$3800`, video `$3C00` (8 bits), RAM from `$4000` (16, 32 or 48 KB).

**Model 4** (port `$84` bits 1-0):

| Map | `$0000-$37FF` | `$3800-$3BFF` | `$3C00-$3FFF` | `$4000-$F3FF` | `$F400-$F7FF` | `$F800-$FFFF` |
|-----|---------------|---------------|---------------|---------------|---------------|---------------|
| 0 | ROM | keyboard | video (1K page) | RAM | RAM | RAM |
| 1 | RAM | keyboard | video (1K page) | RAM | RAM | RAM |
| 2 | RAM | RAM | RAM | RAM | keyboard | video (2K) |
| 3 | RAM | RAM | RAM | RAM | RAM | RAM |

Port `$84`: bit 7 the video page at `$3C00`, bits 6-4 the 32K banks (with
128 KB: 2 or 3 put block 2 or 3 at `$8000`, 6 or 7 put it at `$0000`), bit
3 inverse video, bit 2 80 x 24. The 4P's boot ROM is over `$0000-$0FFF`
until port `$9C` bit 0 is cleared; its map 0 reads RAM below `$3800`.

## Ports (Model III/4)

| Port | Read | Write |
|------|------|-------|
| `$E0-$E3` | interrupt latch, active low: 6 RS-232 error, 5 receive, 4 send, 2 clock, 1/0 cassette fall/rise | interrupt mask |
| `$E4-$E7` | NMI latch, active low: 7 FDC INTRQ, 6 motor timeout, 5 RESET | NMI mask (bits 7, 6) |
| `$E8-$EB` | RS-232: modem status, switches, UART status, data | UART reset, baud rates, control, data |
| `$EC-$EF` | acknowledges the clock interrupt | 6 4 MHz (Model 4), 3 alternate characters, 2 32 columns, 1 cassette motor |
| `$F0-$F3` | FD1793 status, track, sector, data | command, track, sector, data |
| `$F4-$F7` | | drive select: 7 double density, 6 WAIT, 4 side, 3-0 drives |
| `$F8-$FB` | printer status | printer data |
| `$FC-$FF` | 7 cassette flip-flop, 6-1 port `$EC`'s, 0 the 1500 baud level (clears the cassette interrupts) | cassette output (bits 1-0) |
| `$84-$87` | | Model 4: the memory map, video (above) |
| `$75`, `$79` | | the Orchestra-90: right, left DAC (`expansion orchestra orchestra`) |
| `$80-$83` | `$82`: hi-res data | the hi-res board: x, y, data, mode (`expansion hires`) |
| `$88/$89` | | Model 4: CRTC register select and data (12/13: the display's start) |
| `$8C-$8E` | | Model 4: the hi-res board's x offset, y offset, overlay |
| `$90-$93` | | Model 4: the sound bit (0) |
| `$9C-$9F` | | Model 4P: the boot ROM in (bit 0) |
| `$C0-$CF` | the hard disk (below) | the hard disk |

On the Model I: `$B5`/`$B9` the Orchestra-85's right/left DACs, `$C0-$CF`
the hard disk, `$E8-$EB` the RS-232 board (`$E9` reads its sense
switches) and `$F0-$F7` the Stringy Floppy's drives.

## Interrupts and timing

The Model I's Expansion Interface ticks at 40 Hz: bit 7 of the latch at
`$37E0`, pulling /INT until the latch is read; the FD1771's INTRQ is bit 6
(cleared by reading its status). The Model III's clock is 30 Hz (the
Model 4's 60 Hz at 4 MHz), latched in bit 2 and acknowledged by reading
`$EC`; /INT is the latch ANDed with the mask at `$E0`. The FD1793's INTRQ
and the drives' motor timeout (3 s after the last select) go to the NMI
latch (`$E4`); writing `$F4` with bit 6 holds the Z80 in WAIT until DRQ or
INTRQ (or the motor stops), so the DOSes' `OUT ($F4),A : INI` loops take a
byte as it comes, and INTRQ's NMI ends the sector.

RESET: on the Model I an NMI (Level II restarts from its handler, memory
kept); on the III/4 the Z80 is reset (memory kept). Both reset the disk
controller, cassette and UART. The GUI's RESET button, or the key `reset`.

Video waits (Model III/4, port `$EC` bit 5): a CPU access to video RAM
while the display is reading it waits for the line's horizontal blanking.
The CRTC is mask-programmed, so the raster is fixed: 264 lines of 63.1 us
(640 dots at 10.1376 MHz in 64 x 16; 800 at 12.672 MHz in 80 x 24, MAME's
Model 4 raster), 192 or 240 of them shown, each for 80% of the line. Without
the waits (the default) the CPU is never held, as on the Model I (whose
real hardware shows "snow" instead, which mmsim doesn't draw).

Time is kept in ticks of a master clock: 10.6445 MHz on the Model I (6 a
T-state: 1.774 MHz), 20.2752 MHz on the III/4 (10 a T-state, 5 with the
Model 4's 4 MHz: `speed 4` / `speed 2`, or port `$EC` bit 6). The debugger's cycle
counts and pacing are in T-states at the base clock.

## Video

| Model | Text | Cell | Frame |
|-------|------|------|-------|
| I | 64 x 16 (32 x 16) | 6 x 12 | 384 x 384 |
| III | 64 x 16 (32 x 16) | 8 x 12 | 512 x 384 |
| 4 | 64 x 16 (32 x 16) and 80 x 24 (40 x 24) | 8 x 12, 8 x 10 | 640 x 480 |

The Model 4's CRTC registers are kept as written (`crtcRegister()`, the
debugger), but only R12/R13, the display's start, act: the CRTC is
mask-programmed (MAME), and its cursor and light pen outputs are not used
(the ROM and DOSes draw their own cursor).

Each line is drawn twice. Characters `$80-$BF` (and `$C0-$FF` on the
Model I) are 2 x 3 block graphics (bits 0/1 the top pair, 2/3 the middle,
4/5 the bottom); on the III/4 `$C0-$FF` are the special characters (or,
with port `$EC` bit 3, the alternate set). The Model 4's inverse video
(port `$84` bit 3) shows `$80-$FF` as `$00-$7F` inverted. Without the
lower case modification (`expansion lowercase lowercase` fits it) the
Model I stores no D6: writes keep it NOT (D5 or D7), so lower case reads
back as upper case symbols, as on the real machine.

## Keyboard

The matrix at `$3800`: row 0 `@ A-G`, 1 `H-O`, 2 `P-W`, 3 `X Y Z`, 4 `0-7`,
5 `8 9 : ; , - . /`, 6 ENTER CLEAR BREAK UP DOWN LEFT RIGHT SPACE, 7
SHIFT (the III/4: left, right; the 4: CTRL, CAPS, F1-F3). Host keys by
position; Backspace is LEFT, Home CLEAR, Esc or End BREAK, Tab RIGHT. The
PC's shifted symbols (`!`, `"`...) press SHIFT and the TRS-80 key that has
them. Typed text (`type`, paste) is held 60 ms a key.

## Hi-res graphics (Model III/4)

`expansion hires tandy` (Radio Shack's board, 26-1126) or `expansion hires
grafyx` (Micro-Labs' Grafyx Solution), after xtrs: 32 KB of graphics, 128
bytes (bit 7 the leftmost dot) by 256 lines, 640 x 240 of it shown. Port
`$80` x, `$81` y, `$82` the byte at x, y, `$83` the mode (bit 0 on; 1
Micro-Labs: text off, Radio Shack: wait states, not modelled; 2/3 x/y
count down; 4/5 x/y held after a read; 6/7 after a write); on the Model 4
`$8C`/`$8D` the x/y offsets and `$8E` bit 0 the overlay (Radio Shack).
Overlaid, the graphics are XORed with the text dot for dot (byte n under
column n, line n under scan line n); otherwise they are the screen alone.
The Model III's Grafyx Solution also takes port `$FF` writes with bit 5
set (6 on, 7 video RAM reaches the graphics: `$3C00 + n` is byte `n % 64`
of line `(n / 64) * 12` plus bits 4-1). The Model III's screen is 640
wide with a board fitted.

## Orchestra-85 / Orchestra-90

`expansion orchestra orchestra`: two signed 8-bit DACs, the Model I's
Orchestra-85 at `$B9` (left) and `$B5` (right), the Model III/4's
Orchestra-90 at `$79` and `$75` (xtrs's ports). mmsim's sound is mono:
the channels are mixed, and (as xtrs) heard with the cassette motor off.
The Orchestra-80 (the Model I card before the -85) is not modelled apart
from it.

## Cassette

`.cas` (the bits as recorded) and `.wav`: `-i game.cas`, `tape mount`,
the GUI's tape pane; `tape record` / `tape save x.cas` (or `.wav`) record
what the machine writes while the motor is on. A `.cas` plays at the speed
the machine reads at: 250 baud on Level I, 500 on Level II and the Model
III/4, 1500 on the III/4 once its cassette interrupts are enabled (`$E0`
bits 1-0: the ROM's high speed). The pulse shapes are xtrs's. With the
motor off the cassette output (and the Model 4's port `$90`) is the sound.

The Model I has two recorders (`$37E4` bit 0 selects: the relay, the
output and the input go to it): `tape` is the first, `tape 2 mount x.cas`
(and `tape 2 play`...) the second.

## Disks

`-i image` or `disk mount 1 image` (unit 1-4 = drive `:0`-`:3`); DMK, JV1
(10 single-density 256-byte sectors a track: a `.dsk` that is not DMK or
JV3 is taken as JV1) and JV3, read and written (saved back to the file).

- Model I: the FD1771 at `$37EC` (single density); `expansion doubler
  percom` / `tandy` / `both` fits a double-density adapter (the Percom
  Doubler's commands `$FE`/`$FF`, the Radio Shack doubler's sector register
  values `$80`/`$A0` switch to its 1791).
- Model III/4: the FD1793 at `$F0` (`expansion fdc` on the III: fitted by
  default).

## Hard disk

The Radio Shack hard disk interface (`expansion harddisk`, fitted on all
but Level I; on the Model I it needs the EI), after xtrs: a WD1010 at
`$C8-$CF` and the board's `$C0` (read: the drives' write protect) and
`$C1` (control: 4 reset), with up to four drives of 256-byte sectors, 32 a
track. As in xtrs the ports read `$FF` until a drive is mounted, and the
commands finish at once. Images are Matthew Reed's `.hdv` format (xtrs,
TRS32): `disk mount 1 work.hdv` (units 1-4; `-i work.hdv` is unit 1),
written back on eject and at exit; a write-protected header or read-only
file protects the drive. The DOSes need their hard disk drivers (LDOS's
and LS-DOS's `RSHARD`, or xtrs's `XTRSHARD/DCT`).

## Stringy Floppy (Model I)

The Exatron Stringy Floppy (`expansion stringy stringy`; mounting a wafer
fits it), after xtrs: eight wafer drives at `$F0-$F7` (out: 0 motor, 2
write gate, 7 flux; in: 0 write protected, 2 end of tape, 3 no wafer, 7
flux) and the Exatron's ROM at `$3000` (`esf.rom`, which you supply; its
source is in xtrs as `esfrom.z80`). `.esf` wafers (xtrs, TRS32): `disk mount
1 prog.esf` (units 1-8), written back on eject and at exit. The wafer is a
loop of 124 T-state cells; starting the motor starts it from the beginning
(as xtrs, which the ROM's `@SAVE` needs).

## Printer and RS-232

The printer (Model I `$37E8`, III/4 `$37E8` and `$F8`) is always ready;
`serial printer:out.txt` sends the paper to a file. The RS-232 board
(TR1602 at `$E8-$EB`; `expansion rs232 rs232` on the Model I, in the EI;
fitted on the III/4) takes `serial loopback`, `serial file:out.txt`
(transmit) or `serial tcp:PORT` (a socket on localhost), with frame timing
at the programmed baud rate and the receive/send/error interrupts on the
III/4.

The Model I's board has sense switches (`$E9`): `expansion rs232 rs232
300-7E1` sets them as baud-bits-parity-stop (110 to 9600 baud; 5-8 bits;
N, E or O; 1 or 2 stop bits) or as the byte (`$6F`, the default: 9600
8N1). Bits 2-0 the baud rate, 3 no parity, 4 two stop bits, 6-5 the word
length (5, 7, 6, 8), 7 even parity. The III/4's read `$FF` (xtrs).

## Not done

Booting the real ROMs and DOSes (Level I/II BASIC, the Model III/4/4P
ROMs; TRSDOS, LDOS, NEWDOS/80, LS-DOS) has not been checked: the ROMs and
DOSes are the user's, and none are in mmsim's tests. Nor has the Stringy
Floppy with the Exatron's ROM, or the hard disk with RSHARD. The Radio
Shack hi-res board's wait states, the Model I's video "snow" and the
Germany-only HRG1B Model I graphics card are not modelled.

## Tests

`src/plugins/machines/trs80/test/`, with hand-assembled Z80 stand-in ROMs:
the memory maps of each model (the Model 4's maps, banks and video page,
the 4P's boot ROM), the keyboard matrix and typing, video modes as pixels
(Model I graphics and 32 columns, Model III special characters, Model 4
80 x 24 and inverse), the clock interrupt rates (40, 30, 60 Hz), an FDC
sector read on the Model III through WAIT, INI and the NMI and on the Model
I by polling the FD1771, `-i image.dmk` mounting, /CMD loading, snapshots,
and the cassette: `.cas` at 250/500/1500 baud and `.wav` played and
recorded back, Level II-style reading through port `$FF`, and the Model
III's 1500 baud interrupts. `test_trs80_addons.cpp`: the hard disk (Reed
headers, reads, writes, multiple sectors, errors, write protection, files),
the Stringy Floppy (flux runs written and read back, the leader, write
protection, files), the hi-res boards (counting, alone and overlaid, the
Model III's Grafyx Solution), the Orchestra, the Model I's second cassette
and sense switches, the CRTC registers, video waits and snapshots.
