# mmemu-plugin-coco: TRS-80 Color Computer 1, 2 and 2B

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
SAM. PIA0's interrupts are the 6809's /IRQ, PIA1's its /FIRQ.

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

## Not done

- The hi-res joystick interface; the Speech/Sound Pak.
- The PIA's 1 µs IRQ delay, the SAM's video glitches beyond the divider
  switches.

## Tests

`src/plugins/devices/mc6847/test`, `mc6883/test`, `mc6821/test` and
`src/plugins/machines/coco/test` use hand-built stand-in ROMs (6809 code
assembled in `coco_test_rom.h`): the VDG's modes and timing, the SAM's
register, map, memory sizes, rates and video counter, the PIA's flags,
edges and Cx2 modes, the keyboard, joystick comparator, DAC sound,
cartridge autostart, the Multi-Pak, cassette `.cas`/`.wav` round trips and
the bit banger.
