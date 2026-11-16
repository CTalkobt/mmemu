# mmemu-plugin-mc10: TRS-80 MC-10 Micro Color Computer and Matra Alice

Also the Alice 32 and Alice 90 (the Thomson EF9345's video): see
[Alice 32 and 90](#alice-32-and-90).

The Tandy TRS-80 MC-10 (#333): an MC6803 at 0.89 MHz, the MC6847 VDG, 4 KB
of RAM, the 8 KB Micro Color BASIC ROM, a 1-bit sound latch, the cassette
and a serial printer port. The board's wiring follows XRoar (`mc10.c`,
GPL-3.0-or-later) and MAME (`mame/trs/mc10.cpp`); mmsim's own code. The CPU
is described in [README-6803.md](README-6803.md); the VDG is the one the
Color Computer uses ([README-COCO.md](README-COCO.md#video-mc6847)).

## Profiles

| Profile | Machine |
|---------|---------|
| `mc10` | MC-10 with the 16K RAM pack (20 KB) |
| `mc10-4k` | MC-10, unexpanded (4 KB) |
| `alice` | Matra Alice: the French MC-10 (PAL, 312 lines at 50 Hz; AZERTY), with the 16K pack |
| `alice32` | Matra Alice 32 (1984): the EF9345, 8 KB of RAM at `$3000`, 16 KB of ROM at `$C000` |
| `alice90` | Matra Alice 90 (1985): the same with 32 KB of RAM |

```
bin/mmemu-cli -m mc10                      # Micro Color BASIC
bin/mmemu-cli -m mc10 -i game.c10          # types CLOAD and plays the tape
bin/mmemu-gui -m alice
```

## ROMs

Micro Color BASIC is Microsoft's and Tandy's: it is not in mmsim. Put your
own in `roms/mc10/` (ignored by git), or point `MMEMU_MC10_FIRMWARE` at
their directory:

| File | Contents |
|------|----------|
| `mc10.rom` | Micro Color BASIC 1.0, 8 KB at `$E000` (required for `mc10`) |
| `alice.rom` | The Alice's BASIC, 8 KB (required for `alice`; DCAlice's `alice4k.rom`, the same as MAME's) |
| `mcx128bas.rom` / `alice128bas.rom` | The MCX-128's 16 KB EPROM, MCX BASIC 2.1 (QWERTY / AZERTY; optional) |
| `alice32.rom` | The Alice 32's and 90's BASIC, 16 KB at `$C000` (required for `alice32`, `alice90`) |
| `ef9345.rom` | The EF9345's character ROM, 8 KB (MAME's `charset.rom` from its `alice32` set is taken too) |

MCX BASIC 2.1 (Darren Atkinson's, from the MCX-128 project files) opens a
boot menu: Micro Color BASIC with 32 KB, MCX BASIC, or MCX BASIC (large:
the 128 KB banks). Its EPROM replaces the whole ROM, so the Alice runs MCX
BASIC with `alice128bas.rom` even without `alice.rom`. The VMC-10
emulator's "MCX Basic for VMC-10.bin" is the same 16 KB behind a 24-byte
header, which is skipped.

Without BASIC the machine is still made (so the debugger and the plugin
checks work), with a warning naming the file and the directory (printed,
and in the GUI's notice bar); a stand-in ROM puts the same message on the
screen (on the Alice 32/90, through the EF9345; without `ef9345.rom` the
screen shows no characters, with a warning). `roms/mc10/hardware.sym` (checked in) names the 6803's registers,
`KBD_VDG` (`$BFFF`), the MCX-128's registers and the vectors.

## Memory map

The board decodes the top two address lines (as XRoar has it):

| Address | Read | Write |
|---------|------|-------|
| `$0000-$001F` | The 6803's registers (ports, timer, SCI, RAM control) | |
| `$0080-$00FF` | The 6803's 128 bytes of RAM | |
| `$0000-$3FFF` (else) | Nothing: the bus floats (the address's low byte) | |
| `$4000-$4FFF` | The 4 KB of RAM (mirrored at `$6000`; A12 high floats) | |
| `$5000-$8FFF` | The 16K RAM pack, if fitted | |
| `$8000-$BFFF` | Keyboard rows (bits 0-5) | The latch: bit 7 sound, 6 CSS, 5 A/G, 4 GM0, 3 GM1, 2 GM2 (and INT/EXT) |
| `$C000-$FFFF` | The ROM (8 KB, mirrored at `$C000`) | |

The 6803 runs in mode 2 (P20 low, P21 high, P22 low at reset). Its ports:

- Port 1 (all out): the keyboard's columns, strobed low.
- Port 2: P20 out (the cassette and the serial line), P21 in (keyboard row
  6: CONTROL, BREAK, SHIFT), P22 in (RS-232 receive, where a serial
  printer's ready line comes in: BASIC waits for it low before each byte),
  P23 in (CTS, low), P24 in (the cassette). Port 2's bits 7-5 read the
  mode latched at reset (2), as the data sheet has it (XRoar reads 0;
  BASIC doesn't look).

The VDG fetches the internal RAM through its own address counter (the
screen is `$4000`); D7 is A/S, D6 INV; GM2 is also INT/EXT. Time is
counted in ticks of 14.31818 MHz (4x the VDG's clock): an E cycle is 16.
External accesses bring the VDG, cassette and keyboard up to their cycle.

## Alice 32 and 90

Matra's second design (as MAME has it, `mame/trs/mc10.cpp`): the same 6803,
keyboard, cassette and serial port, with the Thomson EF9345 for video.

| Address | Read | Write |
|---------|------|-------|
| `$3000-$4FFF` | The Alice 32's 8 KB of RAM (the Alice 90's 32 KB: `$3000-$AFFF`) | |
| `$BF20-$BF2F` | The EF9345's registers (offset & 7; R0 is the status) | R(offset & 7); at offsets 8-15 R0's command then runs |
| `$8000-$BFFF` (else) | Keyboard rows (the Alice 90 reads only the highest column strobed) | The latch: bit 7 sound |
| `$C000-$FFFF` | The 16 KB ROM | |

The EF9345 (`src/plugins/devices/ef9345/`, after MAME's `ef9345.cpp`) has
its own 16 KB of video RAM, reached only through its commands: KRF/KRG
(24/16-bit codes), CLF/CLG, OCT, KRL (80 columns), IND (the indirect
registers TGS, MAT, PAT, DOR, ROR and the character ROM), INY and the
MVB/MVD/MVT block moves, each with its busy time. It shows 40 columns of
long (24-bit) or short (16-bit, serial attributes) codes, or 80 columns of
12-bit codes: 25 rows (the service row and 24 from ROR's origin), double
height and width, flash, conceal, negative, underline, the cursor, the
insert modes and quadrichrome characters. A row of ten lines is drawn at
its first line, 64 us lines, 312 to a frame. The picture is 336 x 270 (40
columns: 42 cells of 8 dots by 27 rows of 10 with the margin) or 492 x 270
(80 columns). Colours are 3-bit RGB.

Not done (nor in MAME): KRC (8-bit 80-column writes), the 40-column
variable and 80-column 8-bit codes (shown as margin), insert on
quadrichrome characters. CLF and CLG take 4 us a code (MAME leaves the chip
busy until the next command). The chip's timing (MAME: 60 Hz) is taken as
PAL's 50 Hz here.

`.k7` tapes (the Alice 32's name) are the `.c10` format.

## Keyboard

```
         col 0     1     2      3  4  5  6      7
  row 0  @         A     B      C  D  E  F      G
  row 1  H         I     J      K  L  M  N      O
  row 2  P         Q     R      S  T  U  V      W
  row 3  X         Y     Z      -  -  -  ENTER  SPACE
  row 4  0         1     2      3  4  5  6      7
  row 5  8         9     :      ;  ,  -  .      /
  row 6  CONTROL   -     BREAK  -  -  -  -      SHIFT
```

Host keys are positional: letters, digits and symbols by their legend
(shifted symbols press SHIFT with the MC-10 key that has them), Enter,
Space, Shift, Ctrl (CONTROL), Esc (BREAK). The arrows are CONTROL with A
(left; also Backspace), S (right), W (up) and Z (down); Home is CONTROL+Q
(line delete). The Alice's AZERTY legends are on the same matrix: A and Q
swapped, Z and W swapped, M where the MC-10's `;` is, `;` where `/` is and
`/` where M is; the arrows stay on the same keys. Typed text (`type`, MCP,
paste) is pressed for 60 ms with 40 ms between keys.

## Sound

The latch's bit 7 drives the speaker (1 bit); a playing tape is heard
too. 44.06 kHz mono.

## Cassette

`.c10` and `.cas` (the same format: the bytes as BASIC writes them, a 1 a
short cycle of about 2 kHz, a 0 a long one of about 1.1 kHz) and `.wav`
(8/16-bit PCM, any rate). The MC-10 has no motor relay, so a tape goes in
paused (as XRoar does it): `tape file.c10`, type `CLOAD`, then `tape play`.
Loading a `.c10` with `-i` or `load` types `CLOAD` and presses PLAY when
the typing is done. Recording (`tape record`, `tape save file.c10|.wav`)
captures P20.

## Serial port, printers

BASIC's printer output (LLIST, `PRINT #-2`) is bit-banged on P20 at 600
baud, 8N1, once P22 (the printer's ready) is low; mmsim decodes it.

| CLI `serial` | |
|--------------|---|
| `file:PATH` | The output to a host file |
| `loopback` | What is sent comes back on P22 |
| `tcp:PORT` | A TCP client on 127.0.0.1:PORT, both ways |
| `none` | Nothing (the printer's output is still decoded) |

Bytes from a link arrive on P22 as 8N1 frames at the port's baud rate,
for programs with a bit-banged receive routine (Micro Color BASIC has
none); a link idles marking (high), so BASIC's printer routine waits while
one is attached. Without a link P22 is the printer's ready: ready. The
`serial` expansion slot picks what is on the port: `printer` (any serial
printer or RS-232 device: the bytes as sent) or `tp10` (the TP-10 thermal
printer: its 32-column paper as text, CR ending a line); the option is the
baud rate.

## Expansion port

The port's device sees every external bus cycle first and may take it (as
XRoar's `mc10_cart.c`): `expansion expansion <device> [option]`.

| Device | |
|--------|---|
| `ram16k` | Tandy's 16K RAM Expansion Pack (26-3013): RAM at `$5000-$8FFF`, 20 KB in all (the default; BASIC finds it at power on) |
| `rompak` | A program pack: up to 16 KB of ROM at `$5000`; option: the image |
| `mcx128` | Darren Atkinson's MCX-128: 128 KB of RAM in two banks and its 16 KB EPROM in four ROM maps; option: the ROM |

Program packs (`.rom`, `.bin`, up to 16 KB) also go in with the CLI `cart`
and `load`/`-i`, the GUI's Attach Cartridge and MCP `attach_cartridge`:
the pack takes the port's place (the RAM pack comes out) and the machine
is powered on again; ejecting puts the RAM pack back. Micro Color BASIC
doesn't start a pack: `EXEC 20480` runs one at `$5000`.

The MCX-128 is decoded as its GAL's equations give it (`MCX128.pld`, the
4K board): `$BF00-$BF7F` are its registers (even addresses the RAM banks:
P0 for `$0000-$3FFF` and `$C000-$FEFF`, P1 for `$4000-$BEFF`; odd
addresses the ROM map: 0 all EPROM at `$C000`, 1 RAM at `$C000` and the
EPROM's upper half at `$E000`, 2 RAM at `$C000` and the MC-10's ROM at
`$E000`, 3 all RAM). Writes to `$C000-$FFFF` always reach the RAM;
`$FF00-$FFFF` is bank 0's. The MC-10 keeps `$4000-$4FFF` (its own RAM)
while P1 is 0, the keyboard and VDG at `$BF80-$BFFF`, and `$E000-$FFFF`
reads in map 2. The 6803's writes to its own RAM and registers are on the
bus too, so they land in the SRAM under them.

With nothing in the port the machine has 4 KB (`mc10-4k`).

## Snapshots and the debugger

Snapshots keep the RAM, the latch, the VDG, the expansion device's state
and the clock, with the 6803's registers, timer, SCI and internal RAM. The
debugger disassembles 6803 code (Motorola syntax) and assembles it; the GDB
server works with the 6803's registers (`--gdb-port`).

## Checked with the real ROMs

`test_mc10_roms.cpp` (with the files in `roms/mc10/`): Micro Color BASIC
1.0 boots, finds 19526 bytes with the 16K pack (3142 without), runs a
program, CSAVEs to the recorder and CLOADs the `.c10` back, and LLISTs to
the serial printer; MCX BASIC 2.1 boots on the MC-10 in its three
choices (31558, 28703 and 47454 bytes free) and its AZERTY build on the
Alice; the Alice's own BASIC (`alice.rom`: the MC-10's with the AZERTY key
table) boots with 3142 bytes free (19526 with the pack), runs a program,
and CSAVEs and CLOADs a `.k7`; the Alice 32 and 90's BASIC (`alice32.rom`, Micro Color BASIC 1.0)
boots on the EF9345 with 7238 and 31814 bytes free, runs a program, and
CSAVEs and CLOADs a `.k7`. Keyboard typing goes through the ROM's own scan.

## Not done

- The Alice 32/90's EF9345 gaps above.

## Testing

`src/plugins/machines/mc10/test/test_mc10_machine.cpp`, with stand-in ROMs
assembled by the 6803 line assembler:
- the memory map (RAM, mirrors, the pack, the floating bus, ROM, internal RAM; the expansion slot);
- the keyboard matrix: columns on port 1, rows at `$BFFF`, row 6 on P21, typing; the Alice's positions;
- the `$BFFF` latch: VDG mode pins, sound; text and semigraphics drawn; the Alice's 312 lines;
- a stand-in BASIC that boots and prints;
- the missing-ROM stand-in and its warning;
- the cassette both ways (bytes written on P20 decode to `.c10`; a `.c10` played into P24 timed with the 6803's counter);
- the serial printer (a bit-banged 600-baud string; the TP-10's paper);
- RS-232 input on P22 (a bit-banged receive loop; loopback; the printer's ready);
- the MCX-128's banks, registers and ROM maps as its GAL decodes, and the 6803's writes into its SRAM; snapshots;
- the Alice 32 and 90's memory map, the EF9345's registers on the bus, the Alice 90's keyboard read, the missing-ROM stand-in on the EF9345.

`src/plugins/devices/ef9345/test/test_ef9345.cpp`: the EF9345's commands
and busy times, the pointers, clearing and block moves, the 40- and
80-column pictures, the margin, the service row, negative, the cursor,
double height, the VSYNC status, snapshots (a stand-in character ROM).

`test_mc10_roms.cpp` runs with the real ROMs (above), else says so and passes.
