# mmemu-plugin-coco: TRS-80 Color Computer 1, 2, 2B and 3

The Tandy TRS-80 Color Computer (#331): the MC6809E, the MC6883 SAM, the
MC6847 VDG (the MC6847T1 on the CoCo 2B), two MC6821 PIAs and the cartridge
port. The chips follow their data sheets and the Color Computer Technical
Reference Manual; where those leave the behaviour open (the VDG mid-line,
the SAM's video counter and rate changes) mmsim does what XRoar does
(GPL-3.0-or-later; mmsim's own code).

The Dragon 32 and Dragon 64 are built from the same code: see
[README-DRAGON.md](README-DRAGON.md).

## Profiles

| Profile | Machine |
|---------|---------|
| `coco` | Color Computer 2, 64 KB (the same as `coco2`) |
| `coco1` | Color Computer (1): MC6847, 32 KB; prefers Color BASIC 1.2 |
| `coco2` | Color Computer 2: MC6847, 64 KB |
| `coco2b` | Color Computer 2B: MC6847T1 (true lower case), 64 KB |
| `cocopal` | A PAL Color Computer 2, 64 KB: 50 Hz, 312 lines (see [PAL](README-DRAGON.md#pal)) |
| `coco3` | Color Computer 3: the GIME, 128 KB (512 KB as an expansion), Super Extended Color BASIC (see [Color Computer 3](#color-computer-3)) |
| `coco3p` | A PAL Color Computer 3 (its 14.2373 MHz crystal; 50 Hz is the GIME's H50 bit, set by a PAL ROM) |
| `coco3h` | A Color Computer 3 with the HD6309 CPU upgrade and 512 KB |

RAM is 4, 16, 32 or 64 KB: `expansion ram ram 16K` (the machine powers on
again). BASIC finds the size itself, from PIA1 PB2 (tied low with 4K chips,
high with 16K, and to PIA0 PB6 with 64K).

```
bin/mmemu-cli -m coco2                    # Extended Color BASIC
bin/mmemu-cli -m coco2b -i game.ccc       # a ROM Pak: starts itself
bin/mmemu-gui -m coco1
```

## ROMs

The BASIC ROMs are Microsoft's and Tandy's: they are not in mmsim. Put
your own in `roms/coco/` (ignored by git), or point `MMEMU_COCO_FIRMWARE`
at their directory:

| File | Contents |
|------|----------|
| `bas13.rom` (or `bas12.rom`, `bas11.rom`, `bas10.rom`) | Color BASIC, 8 KB at `$A000` (required; the CoCo 1 tries 1.2 first) |
| `extbas11.rom` (or `extbas10.rom`) | Extended Color BASIC, 8 KB at `$8000` (optional: without it the machine runs Color BASIC alone, with a warning) |
| `disk11.rom` (or `disk10.rom`) | Disk Extended Color BASIC, 8 KB, for the FD-500 cartridge |
| `orch90.rom` | The Orchestra-90 program (optional) |

Without Color BASIC the machine is still made (so the debugger and the
plugin checks work), with a warning naming the file it looked for and
where (printed, and in the GUI's notice bar); a stand-in ROM puts the same
message on the screen.

`roms/coco/hardware.sym` (checked in) names the I/O for the debugger:
`PIA0_PA`, the SAM's set/clear addresses (`SAM_F1_SET`), the vectors.

## Memory map

| Address | Map type 0 (`$FFDE`) | Map type 1 (`$FFDF`) |
|---------|------------------|------------------|
| `$0000-$7FFF` | RAM | RAM |
| `$8000-$9FFF` | Extended Color BASIC | RAM (writes also strobe the ROM select) |
| `$A000-$BFFF` | Color BASIC | RAM |
| `$C000-$FEFF` | Cartridge (/CTS) | RAM |
| `$FF00-$FF1F` | PIA0 (every 4 bytes) | |
| `$FF20-$FF3F` | PIA1 | |
| `$FF40-$FF5F` | Cartridge I/O (/SCS): the FD-500 | |
| `$FF60-$FFBF` | Cartridges that decode their own addresses (`$FF68` RS-232 Pak, `$FF7A` Orchestra-90, `$FF7F` Multi-Pak) | |
| `$FFC0-$FFDF` | SAM control register (write: even address clears the bit, odd sets it) | |
| `$FFE0-$FFFF` | Vectors, from Color BASIC (`$BFE0-$BFFF`) | |

The SAM's address multiplexer maps the RAM as the chips have it: with 4K
or 16K chips the SAM's RAS1 (A12 or A14) picks a second bank, which isn't
fitted (it reads `$FF`); in 64K mode P1 (`$FFD5`) puts the upper 32K at
`$0000`.

## Timing

Time is counted in ticks of the 14.31818 MHz crystal. A CPU cycle is 16
ticks (0.89 MHz) or 8 (1.78 MHz), as the SAM's rate bits say: R1
(`$FFD9`) fast everywhere, R0 alone (`$FFD7`, the "speed-up poke") fast
except for RAM and `$FF00-$FF1F`. A change of rate costs the SAM's extra
ticks (15, 17 or 25) to keep the VDG's slots. The 6809's internal cycles
put `$FFFF` on the bus (fast in the address-dependent mode). Each I/O
access brings the VDG, the cassette and the cartridge up to its cycle
first.

The VDG: 912 ticks a line (57 slow cycles), 262 lines a frame (14934
cycles, 59.92 Hz). HS goes to PIA0 CA1 and the SAM; FS to PIA0 CB1 and the
SAM. PIA0's interrupts are the 6809's /IRQ, PIA1's its /FIRQ. A PIA's
flag is set on the active edge, its /IRQ output follows 1 µs (14 ticks,
tRS3) later, as XRoar has it; the edge is timed from where the VDG's HS or
FS fell, not from the instruction that was running.

The SAM's video address counter is XRoar's divider chain: switching V
mid-line re-routes the X and Y dividers under a running count, with the
"glitches" of the paths between them (an input briefly from ground or B4)
moving the address as the real chip does; a new F takes effect at the
next FS; at R1 (1.78 MHz) the VDG shows the byte the CPU last addressed
while the counter runs on. The tests replay mid-line switches against
XRoar's model, address for address.

PAL (`cocopal`, and the Dragons): the board pads the VDG to 312 lines
(24 blank lines at line 16, 26 at line 256: 17784 cycles, 50 Hz), runs
from a 14.23753 MHz crystal and inverts HS to PIA0 CA1; no artifact
colours. See [README-DRAGON.md](README-DRAGON.md#pal).

## Video (MC6847)

All the modes: alphanumerics with the internal font (D6 inverts, CSS
picks green or orange), semigraphics 4, semigraphics 6 (GM0 is INT/EXT
on the CoCo), the eight graphics modes; the SAM's V modes give
semigraphics 8, 12 and 24 and the PMODEs. Mode changes take effect
mid-line. The MC6847T1 (`coco2b`) shows lower case with GM0, inverse text
with GM1 and a coloured text border with GM2.

The picture is 320x240: the 256x192 active area and a border of 32 dots
and 24 lines. **Artifact colours** (PMODE 4 with CSS: black and buff dots
seen as colour on an NTSC set) are off by default; the **Artifact** button
under the screen (or the key name `artifact`) cycles off, blue-red and
red-blue (the two phases a CoCo could start in).

## Keyboard

The host keyboard by position: letters, digits, `@ : ; , - . /`, the
arrows, Enter, Space; Backspace is LEFT, Home is CLEAR, Escape is BREAK.
The PC's shifted symbols type the CoCo's (Shift+2 types `"` as SHIFT+2 on
the CoCo; `=` is SHIFT+`-`, `+` SHIFT+`;`, `*` SHIFT+`:`). `type` text
presses one key at a time (letters unshifted, as BASIC wants).

The **RESET** button restarts BASIC with the program kept.

## Joysticks and sound

The two analog joysticks (0 right, 1 left) go through the comparator on
PIA0 PA7: BASIC's JOYSTK sets the 6-bit DAC and compares. A host joystick
or game pad moves a stick to its ends (fire on PA0/PA1); the mouse moves
the right stick (left button fires).

**Hi-res joystick interface** (Radio Shack 26-3028): `expansion joyright
hires` (or `joyleft`; `normal` takes it off). As MAME's
`coco_tandy_hires_joy` (XRoar has no model of it): software writes the DAC
(PIA1 PA2-PA7) 0 to start the interface's ramp, and counts until the
comparator (PIA0 PA7) goes high, 560 µs + the stick's 10-bit position ×
4856 µs / 1023 later (MAME's figures, tuned to the stock Multi-Vue), so a
12-cycle loop reads about 360 steps against the DAC's 64. Any other DAC
value stops the ramp; changing the selects (CA2 axis, CB2 port) after it
has run out starts it again. The mouse moves the stick on a hi-res port
(the left one if only it has the interface) at 256 steps a host pixel.

Sound: the DAC (PIA1 PA2-PA7) through the analog multiplexer (PIA1 CB2
on, PIA0 CA2/CB2 selecting DAC, cassette, cartridge), the single-bit
output (PIA1 PB1), and the Orchestra-90's DACs, at 44.06 kHz.

## Cassette

`tape mount file.cas` (or `.wav`; `load file.cas` too) puts the tape in
with PLAY down; BASIC's motor relay (PIA1 CA2) runs it, and CLOAD/CLOADM
read it through the comparator (PIA1 PA0). `.cas` bits are a cycle each
(a 1 about 2 kHz, a 0 about 1.1 kHz, least significant bit first).
`tape record`, then CSAVE, then `tape save out.cas` (or `.wav`) records
what the DAC sends while the motor runs. `tape stop`, `tape play`,
`tape rewind`.

## Serial

The bit banger (PIA1 PA1, BASIC's printer port): `serial file:out.txt`
writes what BASIC sends (8N1, 600 baud: BASIC's default).

## Cartridges

`cart game.ccc` (or `.rom`, or `-i`) plugs a ROM Pak in (up to 16 KB at
`$C000`). Its CART line is tied to a clock, so BASIC's cartridge FIRQ
starts it.

`expansion cart <device>` fits a device in the cartridge port:

| Device | |
|--------|---|
| `mpi` | The Multi-Pak Interface: four slots (`expansion mpi1 <device>` ... `mpi4`); `$FF7F` routes /CTS (bits 4-5) and /SCS (bits 0-1); the option is the front switch (1-4, both selects at reset). A ROM Pak attached with `cart` goes in the switch's slot; what was in the port moves there when the Multi-Pak goes in |
| `orch90` | Orchestra-90: stereo DACs at `$FF7A` (left) and `$FF7B` (right), mixed into the sound; the option its ROM |
| `rs232` | Deluxe RS-232 Program Pak: a 6551 ACIA at `$FF68-$FF6B`, its IRQ on CART; `serial loopback`, `serial tcp:PORT`, `serial modem` |
| `fd500` | FD-500 disk controller: Disk BASIC (option: the ROM's path; else `disk11.rom`) and its latch at `$FF40` |
| `rompak` | A ROM Pak; the option its image |

### The FD-500 disk controller

`coco::Fd500` (`src/plugins/machines/coco/main/coco_cart.h`): Disk BASIC's
ROM, the latch at `$FF40` (drive select, motor, precompensation, density,
HALT enable) and a WD2793 at `$FF48-$FF4B` (`Wd17xx`, see
[README-WD17XX.md](README-WD17XX.md)), READY tied high, as XRoar's. While
HALT is enabled the CPU stops until the controller's DRQ (a byte ready) or
INTRQ; with double density selected INTRQ reaches /NMI, which ends Disk
BASIC's transfers. Reset leaves the controller idle (no INTRQ, so no NMI
before Disk BASIC is up), as in XRoar.

Fit it with `expansion cart fd500` (or in a Multi-Pak slot,
`expansion mpi1 fd500`), then mount JVC (`.dsk`), VDK or DMK images:
`disk mount 1 game.dsk` is Disk BASIC's DRIVE 0 (units 1-4 are drives 0-3,
numbered from 1 as on the other machines), and `load game.dsk` or `-i`
mounts to DRIVE 0. Writes go back to the image file.

## Color Computer 3

The Color Computer 3 (#332) is the same board (PIAs, keyboard, joysticks,
sound, cassette, bit banger, cartridge port) around a TCC1014 **GIME** in
place of the SAM and the VDG: `src/plugins/devices/gime`. The GIME follows
Sockmaster's GIME register reference and the CoCo 3 service manual; video
timings and the VDG-compatible rendering are XRoar's measurements (its
`tcc1014.c`, GPL; mmsim's own code).

```
bin/mmemu-cli -m coco3                       # Super Extended Color BASIC
expansion ram ram 512K                       # the 512 KB upgrade
expansion monitor rgb                        # a CM-8 (default: composite)
expansion gime gime 1986                     # the first GIME (default 1987)
expansion cart fd502 ; disk mount 1 os9.dsk  # the FD-502 and Disk BASIC 1.1
```

**ROMs**: `coco3.rom`, the 32 KB Super Extended Color BASIC (`$8000-$FFFF`
in the ROM map; a PAL `coco3p` tries `coco3p.rom` first), and `disk11.rom`
for the FD-502, in `roms/coco/` (not checked in) or `$MMEMU_COCO_FIRMWARE`.
Without `coco3.rom` a stand-in says so on the screen.

**MMU**: eight 8 KB slots, each mapped by a page register of task 0
(`$FFA0-$FFA7`) or task 1 (`$FFA8-$FFAF`, `$FF91` TR) when `$FF90` MMUEN is
set; otherwise pages `$38-$3F`. Pages are 6 bits (64 x 8 KB = 512 KB); on a
128 KB machine only A0-A16 reach the RAM, so pages `$00-$2F` mirror
`$30-$3F`, as on the hardware. `$FE00-$FEFF` with MC3 is page `$3F`
whatever the map. The 1-2 MB Disto upgrades are not modelled.

**ROM map** (TY clear, `$FFDE`): pages `$3C-$3F` are ROM by INIT0 MC1/MC0:
`0x` 16K internal + 16K cartridge (`$C000-$FEFF` as on a CoCo 2), `10` 32K
internal, `11` 32K cartridge. The ROM's address pins are on the CPU bus,
so the byte read is CPU A0-A14 whichever ROM page is mapped. `$FFE0-$FFFF`
(the vectors) always come from the internal ROM. TY set (`$FFDF`): all RAM.

**I/O**: PIA0 at `$FF00` (mirrored to `$FF0F`), PIA1 at `$FF20` (to
`$FF2F`); `$FF40-$FF4F` reaches the cartridge's /SCS only with INIT0 MC2;
`$FF50-$FF5F` always; the cartridge decodes `$FF60-$FF8F` itself
(Multi-Pak `$FF7F`, Deluxe RS-232 Pak `$FF68`, Orchestra-90 `$FF7A`). The
GIME's registers: `$FF90-$FF9F` (write-only, but `$FF92`/`$FF93`), the page
registers, the palette (`$FFB0-$FFBF`), the SAM's set/clear bits
(`$FFC0-$FFDF`: V and F for the VDG modes, R1 `$FFD8/$FFD9` for 1.79 MHz,
TY). Unmapped reads see the data bus's last byte.

**Speed**: every cycle (RAM and ROM alike) is 16 ticks of 14.31818 MHz, or 8
with R1 (the "double speed poke", `POKE 65497,0`).

**Interrupts**: `$FF92` IRQENR and `$FF93` FIRQENR: TMR, HBORD (the right
border starts), VBORD (the bottom border starts), EI2 (serial: not wired),
EI1 (keyboard: any of PIA0 PA0-PA6 low, including the fire buttons), EI0
(the cartridge's CART falling). An event sets its bit in either status it
is enabled in; reading a register returns its status and clears it;
writing it clears what it disables and raises what it newly enables while
the condition holds (a key held, the timer stopped). /IRQ to the CPU is
PIA0's or the GIME's (with IEN), /FIRQ PIA1's or the GIME's (with FEN).

**Timer**: 12 bits (`$FF94` low nibble, `$FF95`), counted at the line rate
(TINS 0, 63.695 us) or 3.58 MHz (TINS 1, 279.365 ns). Writing `$FF94`
starts it; at zero it raises TMR, flips the text blink and reloads; 0 stops
it. The 1986 GIME counts value + 2, the 1987 value + 1 (Sockmaster); the
revisions' horizontal timings (HSYNC width, where VBORD falls on a line)
also differ, after XRoar's scope measurements.

**Video**: the frame is 704 x 240 (the 640-tick active area with 32 ticks
of border each side; 512-tick modes are centred), in the GIME's six-bit
colours through the monitor: RGB (R1 G1 B1 R0 G0 B0, each channel
0/0x55/0xAA/0xFF) or composite (intensity bits 4-5, hue bits 0-3; hue 0
grey; VMODE MOCH shows the intensity only). A line is drawn when its right
border starts, from the registers as they are then.

- CoCo 3 text: 32/40/64/80 columns (VRES HRES), with or without attribute
  bytes (CRES bit 0: background palette 0-7 in bits 0-2, foreground 8-15
  in bits 3-5, underline bit 6 on the row's last line, blink bit 7 with the
  timer), the GIME's 128-character font (`gime_font.cpp`, 8 x 12; the
  character's row is the row counter + 1). Lines per row from VMODE LPR
  (1, 2, 8, 9, 10, 11, infinite).
- CoCo 3 graphics: 16-160 bytes a row at 1, 2 or 4 bits a pixel (up to
  640 x 225 x 4, 320 x 225 x 16), 192/200/225 lines (VRES LPF; "infinite"
  set during the picture runs it on), the vertical offset (`$FF9D/$FF9E`,
  address bits 3-18), the horizontal offset (`$FF9F`: X in 2-byte steps;
  HVEN a 256-byte virtual row), VSC (`$FF9C`) for the first text row's
  starting line, the border (`$FF9A`), 60 Hz or 50 Hz (VMODE H50: 263 or 314
  lines).
- VDG-compatible (INIT0 COCO): the mode from PIA1 port B as written to
  `$FF22` (the GIME snoops the data bus), the SAM's V and F; text in the
  GIME's font (GM0 gives lower case for codes `$00-$1F`, GM1 inverts, as an
  MC6847T1), SG4, the CG and RG graphics, all through the palette (0-7 the
  CG colours, 8-11 the RG pairs, 12-15 the text). The address is
  `$FF9D/$FF9E` bits 16-18 and 3-8 with the SAM's F from bit 9. `$FF9C` is
  the text rows' line count (`$0F`: 12). No artifact colours.

**Expansions**: `expansion ram ram 512K`; the FD-502 (`fd502`, the FD-500's
code with Disk BASIC 1.1), the Deluxe RS-232 Pak (`rs232`, the 6551 at
`$FF68`; `serial`), the Orchestra-90 and ROM Paks, alone or in the
Multi-Pak (`mpi`, slots `mpi1-mpi4`); the hi-res joystick interface
(`expansion joyright hires`, which the host mouse drives) as on the CoCo 2.

**CPU**: an MC6809E, or on `coco3h` the Hitachi HD6309 upgrade (512 KB;
for NitrOS-9's 6309 builds). The HD6309 starts in emulation mode with the
6809's timings, so Super Extended BASIC runs unchanged; see
`doc/README-6809.md` for the core.

**Where the references differ** (and what mmsim does):

- TINS 1's rate: Sockmaster and XRoar 3.58 MHz (279.365 ns); MAME's code
  counts at half that. mmsim: 3.58 MHz.
- EI0: Sockmaster says the CART line's falling edge; XRoar raises it as
  its cartridge asserts the line. mmsim: CART falling.
- The timer offset: MAME applies the 1986 GIME's + 2 to both revisions
  (its 1987 switch is compiled out); mmsim follows Sockmaster (+ 2 / + 1).
- The internal ROM's address: the CPU's A0-A14 (XRoar; the ROM's pins are
  on the CPU bus), not the MMU page.

## Not done

- The Speech/Sound Pak.
- The CoCo Max 3 style hi-res adapter (only the Tandy interface is
  modelled).
- CoCo 3: composite artifact colours; mid-line GIME changes (a line is
  drawn whole at its right border); the Disto 1-2 MB MMU extension
  (`$FF9B`, page bits 6-7); the serial EI2 input; hard disk interfaces
  and the CoCo SDC.

## Tests

`src/plugins/devices/mc6847/test`, `mc6883/test`, `mc6821/test` and
`src/plugins/machines/coco/test` use hand-built stand-in ROMs (6809 code
assembled in `coco_test_rom.h`): the VDG's modes and timing, the SAM's
register, map, memory sizes, rates and video counter, the PIA's flags,
edges and Cx2 modes, the keyboard, joystick comparator, DAC sound,
cartridge autostart, the Multi-Pak, the hi-res joystick interface read
by a timing loop, the PIA's IRQ delay, the SAM's mid-line V switches, cassette `.cas`/`.wav` round trips and
the bit banger. `src/plugins/devices/gime/test` and
`src/plugins/machines/coco/test/test_coco3_machine.cpp` cover the GIME
(MMU mapping, tasks, RAM-size mirroring, the constant page, the ROM
modes, the speed bit, the palette, the timer on both revisions and both
clocks, the interrupt enable/acknowledge rules, every video mode family's
pixels, lines per row, the offsets and VSC, the VDG-compatible modes) and a
CoCo 3 running a stand-in ROM that sets up a 40-column screen.
