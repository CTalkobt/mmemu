# Dragon 32 and Dragon 64 (mmemu-plugin-coco)

Dragon Data's Dragon 32 and Dragon 64 are the Color Computer's board built
in Wales: the same MC6809E, MC6883 SAM, MC6847 VDG and two MC6821 PIAs,
the same cassette, joystick and cartridge ports. mmsim builds them from
the CoCo machine's code (`src/plugins/machines/coco`, see
[README-COCO.md](README-COCO.md)); what differs follows XRoar
(dragon/dragon.c, dragon32.c, dragon64.c, dragondos.c, dkbd.c;
GPL-3.0-or-later; mmsim's own code) and the Dragon's circuit diagrams.

## Profiles

| Profile | Machine |
|---------|---------|
| `dragon32` | Dragon 32: 32 KB, PAL |
| `dragon64` | Dragon 64: 64 KB, PAL, the 6551 RS-232 port and the 64K-mode BASIC |
| `tano` | Tano Dragon: the NTSC Dragon 64 (60 Hz, 14.31818 MHz, artifact colours) |

```
bin/mmemu-cli -m dragon32                      # Dragon BASIC
bin/mmemu-cli -m dragon32 -i game.dgn          # a ROM cartridge: starts itself
bin/mmemu-cli -m dragon64                      # then: expansion cart dragondos; disk mount 1 disk.vdk
bin/mmemu-gui -m dragon64
```

## ROMs

The ROMs are Microsoft's and Dragon Data's: they are not in mmsim. Put your
own in `roms/dragon/` (ignored by git, except `hardware.sym`), or point
`MMEMU_DRAGON_FIRMWARE` at their directory. Each name is tried as `.rom`,
`.bin` and `.dgn`:

| File | Contents |
|------|----------|
| `d32.rom` (or `dragon32`, `d32rom`, `dragon`) | Dragon 32 BASIC, 16 KB at `$8000` |
| `d64_1.rom` (or `d64rom1`, `dragrom`) | Dragon 64 BASIC (32K mode), 16 KB at `$8000` |
| `d64_2.rom` (or `d64rom2`) | Dragon 64 BASIC (64K mode), 16 KB, selected by PIA1 PB2 (optional: without it the machine has 32K mode only, with a warning) |
| `ddos12a.rom` (or `ddos12`, `ddos40`, `ddos15`, `ddos10`, DOSplus `dplus49b`, SuperDOS `sdose6`) | DragonDOS, 8 KB (or 16 KB), for its cartridge |

Without BASIC the machine is still made, with a warning naming the file
and where it was looked for; a stand-in ROM puts the same message on the
screen. `roms/dragon/hardware.sym` names the I/O for the debugger
(`PIA0_PA`, `ACIA_STATUS`, `FDC_CMD`, `DOS_LATCH`, the SAM's addresses).

## What differs from the CoCo

- **Keyboard**: the same keys and columns (PIA0 port B), the rows (PIA0
  port A) in another order: row 0 `0`-`7`, row 1 `8 9 : ; , - . /`, row 2
  `@ A`-`G`, row 3 `H`-`O`, row 4 `P`-`W`, row 5 `X Y Z` UP DOWN LEFT
  RIGHT SPACE, row 6 ENTER CLEAR BREAK and SHIFT (as the CoCo's). Host keys
  and `type` text go to the Dragon's positions.
- **ROM**: one 16 KB BASIC at `$8000-$BFFF` (the vectors from its top).
- **Printer port** instead of the bit banger: data on PIA0 port B (the
  keyboard's column lines), STROBE on PIA1 PA1 (a byte is taken as it
  rises), BUSY on PIA1 PB0 (pulled up; never busy here), /ACK on PIA1 CA1
  (a 7 µs pulse). `serial file:out.txt` (or `serial printer:out.txt`)
  writes what is printed.
- **PIA1 PB2**: on the Dragon 32 the RAM size (high with 4K/16K chips, low
  with 32K/64K: BASIC's 32K check); on the Dragon 64 the ROM select
  (ROMSEL, pulled up: high the 32K-mode BASIC, low the 64K-mode one).
- **Decoding**: PIA0 answers at `$FF00-$FF03` and PIA1 at `$FF20-$FF23`
  only where A2 is low (`$FF08`, `$FF28`... mirror them); an unexpanded
  Dragon 32 (32 KB) decodes them as the CoCo, through all 32 bytes.
- **Dragon 64 RS-232**: a 6551 ACIA at `$FF04-$FF07` (mirrored to
  `$FF1F`), with its own 1.8432 MHz crystal; its /IRQ isn't connected (as
  XRoar). `serial loopback`, `serial tcp:PORT`, `serial modem`.
- The cassette (PIA1 PA0 in, the DAC out, CA2 the motor), sound, joysticks
  and the cartridge port's lines (CART on PIA1 CB1, /NMI, /HALT) are the
  CoCo's.

## PAL

The Dragons are PAL machines: 50 Hz, a 14.218 MHz crystal (a CPU cycle at
0.8886 MHz). The MC6847 only makes NTSC's 262 lines; the board stops its
clock twice a frame and makes 50 lines of its own (`Mc6847::setPal`, as
XRoar's vdg_pal): 25 at the start of line 0 (in vertical blank) and 25 at
line 254 (the bottom border), so a frame is 312 lines of 57 cycles (17784
cycles, 49.97 Hz). Each padding line has an HS pulse (PIA0 CA1, the SAM),
except on the Dragon 64, whose board holds HS during the padding (262 HS a
frame). The padding lines are black and aren't in the 320x240 picture.

A PAL set shows no artifact colours, so the PAL machines have no Artifact
button. The palette is the VDG's (as XRoar's default "ideal" one for both
standards).

The CoCo has a PAL profile too: `cocopal` (a PAL Color Computer 2, 64 KB):
the 14.23753 MHz crystal, 24 padding lines at line 16 and 26 at line 256,
HS inverted on its way to PIA0 CA1 (as the PAL CoCo's board does). In code
`coco::create(model, &err, ramKb, roms, coco::Tv::PAL)` makes any CoCo
PAL, and `Tv::NTSC` a Dragon NTSC (`tano` is the Dragon 64 so).

Everything timed in the machine follows the crystal: the sound's 325-tick
samples (43.75 kHz), `.wav` tapes, the disk controller's revolutions and
the 6551's bit rates are in real time; `.cas` bits are in CPU cycles, as
BASIC writes them.

## Cartridges

`cart game.dgn` (or `.rom`, `.ccc`, or `-i`) plugs a ROM cartridge in (up
to 16 KB at `$C000`); its CART line tied to a clock starts it through
BASIC's FIRQ, as on the CoCo.

`expansion cart <device>`:

| Device | |
|--------|---|
| `dragondos` | DragonDOS: its ROM at `$C000` (option: the ROM's path; else `ddos12a.rom` ...), a WD2797 at `$FF40-$FF43` and the latch at `$FF48` |
| `rompak` | A ROM cartridge; the option its image |

The CoCo's Multi-Pak, FD-500, Orchestra-90 and RS-232 Pak aren't offered.

### DragonDOS

`coco::DragonDos` (`coco_cart.h`, `dragon_dos.cpp`): the latch at
`$FF48-$FF4F` (write) holds the drive select (bits 0-1: DragonDOS's drives
1-4), the motor (2), density (3, 1 = single), write precompensation (4) and
the NMI enable (5). The WD2797 (mmsim's `Wd17xx`, see
[README-WD17XX.md](README-WD17XX.md)) selects the side itself from its
commands' U bit; READY is tied high. Its DRQ is the CART line (PIA1 CB1,
the FIRQ); INTRQ rising with the NMI enable set is /NMI. Reset leaves the
controller idle.

Images: VDK (the Dragon's), JVC or DMK. `disk mount 1 game.vdk` is
DragonDOS's drive 1 (`disk mount 2` drive 2, ...), and `load game.vdk` or
`-i game.vdk` mounts to drive 1. Writes go back to the file.

## Not done

- The Dragon 200-E (its character ROM and keyboard), the Dragon
  Professional, the SAMx8; other DOS cartridges' hardware (Cumana, Premier
  Delta, the Becker port).
- The Dragon 64's ACIA in snapshots; printer BUSY from a host printer.
- Booting the real ROMs: none are in the repository (see below).

## Tests

`src/plugins/machines/coco/test/test_dragon_machine.cpp` (a hand-assembled
6809 stand-in for Dragon BASIC) and `src/plugins/devices/mc6847/test`:
PAL frame length and FS period (Dragon, Dragon 64 with HS held, PAL CoCo,
the Tano's NTSC), FS interrupts at 50 Hz, the Dragon keyboard matrix, PIA1
PB2 and the decoding, the printer, the Dragon 64's ROM select and 6551
(loopback), cartridge autostart, a DragonDOS VDK sector read with
DRQ-on-CART and INTRQ-to-NMI, and `-i`/`disk mount 1` to DragonDOS's drive
1. The real Dragon BASIC and DragonDOS haven't been run.
