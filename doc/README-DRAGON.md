# Dragon 32, 64, 200-E and Professional (mmemu-plugin-coco)

Dragon Data's Dragon 32 and Dragon 64 are the Color Computer's board built
in Wales: the same MC6809E, MC6883 SAM, MC6847 VDG and two MC6821 PIAs,
the same cassette, joystick and cartridge ports. mmsim builds them from
the CoCo machine's code (`src/plugins/machines/coco`, see
[README-COCO.md](README-COCO.md)); what differs follows XRoar
(dragon/dragon.c, dragon32.c, dragon64.c, dragonpro.c, dragondos.c,
deltados.c, dkbd.c, vdg.c;
GPL-3.0-or-later; mmsim's own code) and the Dragon's circuit diagrams.

## Profiles

| Profile | Machine |
|---------|---------|
| `dragon32` | Dragon 32: 32 KB, PAL |
| `dragon64` | Dragon 64: 64 KB, PAL, the 6551 RS-232 port and the 64K-mode BASIC |
| `tano` | Tano Dragon: the NTSC Dragon 64 (60 Hz, 14.31818 MHz, artifact colours) |
| `dragon200e` | Dragon 200-E: Eurohard's Spanish Dragon 64, its own BASIC, character generator and keyboard legends |
| `dragonpro` | Dragon Professional (Alpha): a Dragon 64 with a boot ROM, an AY-3-8912 and a built-in WD2797 floppy controller |

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
| `d200e_1.rom` (or `d200e_rom1`, `ic18_v1.4e.ic34`; else the Dragon 64's `d64_1`) | Dragon 200-E BASIC (32K mode), 16 KB |
| `d200e_2.rom` (or `d200e_rom2`, `ic17_v1.4e.ic37`; else `d64_2`) | Dragon 200-E BASIC (64K mode), 16 KB |
| `d200e_26.rom` (or `rom26.ic1`) | Dragon 200-E character generator, 4 KB (optional: without it the VDG's own font) |
| `alpha-basic.rom` (or `alpha_ba`; else `d64_1`) | Dragon Professional BASIC, 16 KB |
| `alpha-boot-v1.0.rom` (or `alpha_bt_10`, `alpha-boot-v0.4`, `alpha_bt_04`) | Dragon Professional boot ROM, 8 KB (optional: without it BASIC starts directly) |
| `ddos12a.rom` (or `ddos12`, `ddos40`, `ddos42`, `ddos15`, `ddos11c`, `ddos10`, DOSplus `dplus49b`, SuperDOS `sdose6`) | DragonDOS, 8 KB (or 16 KB), for its cartridge |
| `delta2.rom` (or `delta1a`, `delta19`, `delta`, `deltados`) | Premier Delta DOS, 8 KB |
| `cdos20.rom` (or `CDOS20`, `CumanaDOSv2.0`) | Cumana DOS 2.0 |
| `sdose6.rom` (or `sdose8`, `sdose5`, `sdose4`) | SuperDOS |
| `dplus49b.rom` (or `dplus48`, `dosplus-4.8`, `DOSPLUS`, `dplus50`) | DOSplus |

The names are XRoar's romlists (`dplus50`, `sdose8`, `ddos42`, `ddos11c`
added for images found under those names). A file called `d200rom1.rom`
/ `d200rom2.rom` is usually the plain (English) Dragon 200's ROM, which is
the Dragon 64's (CRC `84f68bf9` / `17893a42`, as the Tano's): use it as
`d64_1` / `d64_2`, not as the 200-E's (`95af0a0a` / `48b985df`).

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
to 16 KB at `$C000`; larger images are bank-switched as on the CoCo, see
[README-COCO.md](README-COCO.md#bank-switched-rom-paks)); its CART line
tied to a clock starts it through BASIC's FIRQ, as on the CoCo.

`expansion cart <device>`:

| Device | |
|--------|---|
| `dragondos` | DragonDOS: its ROM at `$C000` (option: the ROM's path; else `ddos12a.rom` ...), a WD2797 at `$FF40-$FF43` and the latch at `$FF48` |
| `deltados` | Premier Delta DOS: its ROM at `$C000`, a WD2791 at `$FF40-$FF43`, the latch at `$FF44` |
| `cumana` | Cumana DOS 2.0 on DragonDOS's hardware |
| `superdos` | SuperDOS (PNP) on DragonDOS's hardware |
| `dosplus` | DOSplus on DragonDOS's hardware |
| `rompak` | A ROM cartridge; the option its image |
| `gmc`, `banked` | A bank-switched ROM cartridge (see the CoCo's) |

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

### Delta, Cumana, SuperDOS, DOSplus

XRoar runs Cumana DOS, SuperDOS and DOSplus on DragonDOS's controller
("dragondos_compat"): only the ROM differs, so here they are
`coco::DragonDos` with another ROM and name.

`coco::DeltaDos` (`coco_paks.h`): Premier Microsystems' Delta system, as
XRoar's deltados.c and MAME's premier_fdc. The ROM at `$C000`; a WD2791
(its data bus inverted: registers read and written complemented) at
`$FF40-$FF43`; a write-only latch where A2 is set (`$FF44`): bits 0-1
drive, 2 side, 3 the 8" clock, 4 double density. No motor bit, READY tied
high (XRoar: assumed), and neither DRQ nor INTRQ reaches the CPU: Delta DOS
polls the status. XRoar mirrors the registers through `$FF5F`, MAME
decodes `$FF44` alone; the latch's bit 3 is from MAME's notes only
(unverified).

## Dragon 200-E

A Dragon 64 board (its 6551, ROM select) with Eurohard's Spanish BASIC
(v1.4e) and a 4 KB character generator (`d200e_26.rom`), after XRoar's
dragon64.c / vdg.c: in alphanumeric modes a byte with D7 low is drawn from
the ROM (address: the VDG's row << 8, GM0 as A7, the byte), its data active
low; D7 high is semigraphics as on any Dragon. The keyboard matrix is the
Dragon's; typed text uses the 200-E's legends (XRoar's dkbd.c): `;` and
`+` on the `@` key, `@` SHIFT+CLEAR, `Ñ`/`ñ` on the `;` key, `Ç` SHIFT+0,
`Ü` SHIFT+BREAK, `¿` / `¡` SHIFT+RIGHT / DOWN, `§` and `~` SHIFT+SPACE.

## Dragon Professional

The Dragon Professional (Alpha) after XRoar's dragonpro.c and MAME's
dgnalpha.cpp: a Dragon 64 with

- PIA2 at `$FF24-$FF27` (its IRQs on FIRQ): PA0/PA1 the AY's BDIR/BC1, PA2
  the ROM select (high at reset: the 8 KB boot ROM at `$8000` and `$A000`;
  low: BASIC), port B the AY's data bus, CA2 the disk's NMI enable, CB1 the
  controller's DRQ.
- An AY-3-8912 at 1 MHz (XRoar's clock; MAME has 4 MHz / 4), on the sound
  mux's fourth input. Its port A drives the drives: bits 0-3 select, 4 the
  motor, 5 single density, 6 precompensation, 7 8" (2 MHz controller clock).
- A WD2797 at `$FF2C-$FF2F` with its registers reversed (`$FF2F`
  command / status, `$FF2C` data); INTRQ to /NMI while CA2 is high. `disk
  mount 1-4` are its drives.
- The modem (MAME stubs `$FF28-$FF2B`) is not emulated.

## Not done

- The Dragon 64's ACIA in snapshots; printer BUSY from a host printer.
- The SAMx8; the Becker port.
- The Dragon Professional with its real boot ROM: none was available, so
  only the stand-in test boots it. The Dragon 200-E with its Spanish ROMs
  and character set: not available either; it was run with the Dragon 64's
  ROMs (see [Verification](#verification)).

## Verification

With user-supplied ROMs (not in the repository), November 2026:

- `dragon200e` with the Dragon 64's ROMs (as XRoar's fallback): boots to
  BASIC's `OK` and answers `PRINT`; no character set (VDG font).
- DragonDOS 1.0, 1.1C, 1.2A, 4.0, 4.2 (`dragondos`), DOSplus 4.8 / 4.9B / 5.0
  (`dosplus`), SuperDOS E6 / E8 (`superdos`) and Delta 2.0 (`deltados`) on
  the Dragon 64: each signs on; with a blank VDK in drive 1, DSKINIT (DOSplus
  asks `SURE?`) formats and verifies 40 tracks, `SAVE"HELLO"` writes a
  program, DIR lists it (`HELLO.BAS`) with the free space, and `LOAD"HELLO"` / `RUN` print its result. Delta
  2.0 lists a blank disk's directory (`702 DOMAINS FREE`); its ROM has no
  format command and no Delta disk image was at hand, so SAVE on that blank
  disk ends in `ILLEGAL ACCESS ERROR` (not investigated further). Cumana DOS
  was not available.
- DOSplus found a controller bug: Read Address raised INTRQ together with
  the sixth byte's DRQ, so DOSplus's NMI handler saw five bytes and DIR
  gave `?BT ERROR`; the command now ends a byte time later (as XRoar's
  wd279x.c).

`src/plugins/machines/coco/test/test_coco_real_roms.cpp` repeats these
when the ROMs are there (the other DOS versions with `MMEMU_DOS_SWEEP`
set) and passes, saying so, without them.

## Tests

`src/plugins/machines/coco/test/test_dragon_machine.cpp` (a hand-assembled
6809 stand-in for Dragon BASIC) and `src/plugins/devices/mc6847/test`:
PAL frame length and FS period (Dragon, Dragon 64 with HS held, PAL CoCo,
the Tano's NTSC), FS interrupts at 50 Hz, the Dragon keyboard matrix, PIA1
PB2 and the decoding, the printer, the Dragon 64's ROM select and 6551
(loopback), cartridge autostart, a DragonDOS VDK sector read with
DRQ-on-CART and INTRQ-to-NMI, and `-i`/`disk mount 1` to DragonDOS's drive
1; the Delta and Cumana cartridges' decoding, the 200-E's character
generator and keyboard legends, and the Professional's boot ROM, PIA2,
AY-3-8912 and reversed WD2797. `test_coco_real_roms.cpp` runs the real
ROMs when present (see [Verification](#verification)).
