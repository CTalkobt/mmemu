# mmemu-plugin-mc10: TRS-80 MC-10 Micro Color Computer and Matra Alice

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
| `alice.rom` | The Alice's BASIC, 8 KB (required for `alice`) |
| `mcx128bas.rom` / `alice128bas.rom` | The MCX-128's 16 KB EPROM (optional) |

Without BASIC the machine is still made (so the debugger and the plugin
checks work), with a warning naming the file and the directory (printed,
and in the GUI's notice bar); a stand-in ROM puts the same message on the
screen. `roms/mc10/hardware.sym` (checked in) names the 6803's registers,
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
  6: CONTROL, BREAK, SHIFT), P22 in (RS-232 receive, idle high), P23 in
  (the printer's ready, low), P24 in (the cassette).

The VDG fetches the internal RAM through its own address counter (the
screen is `$4000`); D7 is A/S, D6 INV; GM2 is also INT/EXT. Time is
counted in ticks of 14.31818 MHz (4x the VDG's clock): an E cycle is 16.
External accesses bring the VDG, cassette and keyboard up to their cycle.

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
baud, 8N1; mmsim decodes it to a host file with `serial file:PATH`
(`serial none` stops). The `serial` expansion slot picks what is on the
port: `printer` (any serial printer or RS-232 device: the bytes as sent)
or `tp10` (the TP-10 thermal printer: its 32-column paper as text, CR
ending a line); the option is the baud rate. The printer's ready line
(P23) reads ready. Receiving on P22 is not wired to the host (it idles
high).

## Expansion port

The port's device sees every external bus cycle first and may take it (as
XRoar's `mc10_cart.c`): `expansion expansion <device> [option]`.

| Device | |
|--------|---|
| `ram16k` | Tandy's 16K RAM Expansion Pack (26-3013): RAM at `$5000-$8FFF`, 20 KB in all (the default; BASIC finds it at power on) |
| `rompak` | A program pack: up to 16 KB of ROM at `$5000`; option: the image |
| `mcx128` | Darren Atkinson's MCX-128: 128 KB of RAM in two banks (`$BF00`) and its 16 KB EPROM in four ROM maps (`$BF01`), as MAME maps it; option: the ROM |

With nothing in the port the machine has 4 KB (`mc10-4k`).

## Snapshots and the debugger

Snapshots keep the RAM, the latch, the VDG, the expansion device's state
and the clock, with the 6803's registers, timer, SCI and internal RAM. The
debugger disassembles 6803 code (Motorola syntax) and assembles it; the GDB
server works with the 6803's registers (`--gdb-port`).

## Not done

- The real ROM could not be run here: booting Micro Color BASIC, BASIC's
  RAM-size check with the pack, its CLOAD/CSAVE timing and its printer
  routine are exercised only through the hand-assembled stand-ins in the
  tests.
- RS-232 input on P22; the MCX-128's mirror of the 6803's own RAM into its
  RAM; the Alice 32/90 (their EF9345 video).

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
- the MCX-128's banks and ROM maps; snapshots.
