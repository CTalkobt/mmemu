# mmemu-plugin-coco: TRS-80 Color Computer 1, 2, 2B and 3

The Tandy TRS-80 Color Computer (#331): the MC6809E, the MC6883 SAM, the
MC6847 VDG (the MC6847T1 on the CoCo 2B), two MC6821 PIAs and the cartridge
port. The chips follow their data sheets and the Color Computer Technical
Reference Manual; where those leave the behaviour open (the VDG mid-line,
the SAM's video counter and rate changes) mmsim does what XRoar does
(GPL-3.0-or-later; mmsim's own code).

The Dragon 32, Dragon 64, Dragon 200-E and Dragon Professional, and the
Dragon's disk cartridges (DragonDOS, Delta, Cumana DOS, SuperDOS, DOSplus),
are built from the same code: see [README-DRAGON.md](README-DRAGON.md),
which lists their ROM file names (XRoar's: `d32`, `d64_1` / `d64_2`,
`d200e_1` / `d200e_2` / `d200e_26`, `alpha-basic` / `alpha-boot-v1.0`,
`ddos12a`, `delta2`, `cdos20`, `sdose6`, `dplus49b`, in
`MMEMU_DRAGON_FIRMWARE` or `roms/dragon/`).

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
| `pic-7040-510.bin` | The Speech/Sound Cartridge's TMS7040 mask ROM, 4 KB (required for `ssc`; MAME's name) |
| `sp0256-al2.bin` | The SP0256-AL2's allophone ROM, 2 KB (optional: without it the SSC's speech is silent) |
| `sdc-dos.rom` (or `sdcdos.rom`, `cocosdc.rom`) | The CoCo SDC's flash: SDC-DOS (16 KB, bank 0) or the whole 128 KB (optional) |

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
| `$FF40-$FF5F` | Cartridge I/O (/SCS): the FD-500, the Disto SC-II, the CoCo SDC, Burke & Burke (`$FF50`), the Becker port (`$FF41/$FF42`) | |
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

**CoCo Max 3 hi-res adapter** (CoCo 3 only): `expansion joyright cm3`.
As MAME's `coco_cm3_hires_joy`: the same ramp, but started when PIA0
PA0-PA3 are all driven low (PA0-PA3 as outputs written 0) and stopped
otherwise, rising 500 µs + position × 2624 µs / 1023 later. MAME marks
those two figures "chosen by fair dice roll": **unverified** against the
adapter or CoCo Max III.

**CoCo Max Hi-Res Input Pack** (CoCo Max I/II, CoCo 1/2): a cartridge,
below (`cocomax`).

Sound: the DAC (PIA1 PA2-PA7) through the analog multiplexer (PIA1 CB2
on, PIA0 CA2/CB2 selecting DAC, cassette, cartridge), the single-bit
output (PIA1 PB1), and the Orchestra-90's DACs and the Speech/Sound
Cartridge, at 44.06 kHz.

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
writes what BASIC sends (8N1, 600 baud: BASIC's default). The receive
line (PIA1 PB0): `serial in:file.bin` sends a host file's bytes down it,
8N1 at the same rate (the device option `baud`), the line at mark
(high) between bytes. With nothing connected PB0 reads low (XRoar's
observation). On the CoCo 3 the line is
also the GIME's EI2 interrupt (below). `serial drivewire` puts the built-in
DriveWire server on the two lines (below: DriveWire over the bit banger).

Both lines change on the bus cycle the 6809 reads or writes PIA1: the
CPU's dead cycles are placed among its accesses as the data sheet has them
(an `LDA $FF22` reads on its fifth cycle, after a dead one; a `ROR $FF20`
reads on its fifth and writes on its seventh), so software that counts
cycles between samples sees the line where its count puts it.

## Cartridges

`cart game.ccc` (or `.rom`, or `-i`) plugs a ROM Pak in (up to 16 KB at
`$C000`; on a CoCo 3 a whole 32 KB at `$8000-$FEFF`; larger images are
bank-switched, see below). Its CART line is tied to a clock, so BASIC's
cartridge FIRQ starts it.

`expansion cart <device>` fits a device in the cartridge port:

| Device | |
|--------|---|
| `mpi` | The Multi-Pak Interface: four slots (`expansion mpi1 <device>` ... `mpi4`); `$FF7F` routes /CTS (bits 4-5) and /SCS (bits 0-1); the option is the front switch (1-4, both selects at reset). A ROM Pak attached with `cart` goes in the switch's slot; what was in the port moves there when the Multi-Pak goes in |
| `orch90` | Orchestra-90: stereo DACs at `$FF7A` (left) and `$FF7B` (right), on the cartridge sound input (the analog multiplexer's source 2); the option its ROM |
| `ssc` | Speech/Sound Cartridge (26-3144): TMS7040, SP0256-AL2 and AY-3-8913 behind `$FF7D/$FF7E` (option: `[TMS7040 ROM][,SP0256 ROM]`; below) |
| `cocomax` | CoCo Max Hi-Res Input Pack: an A/D for its mouse at `$FF90-$FF97`, moved by the host mouse (not on a CoCo 3; below) |
| `rs232` | Deluxe RS-232 Program Pak: a 6551 ACIA at `$FF68-$FF6B`, its IRQ on CART; `serial loopback`, `serial tcp:PORT`, `serial modem` |
| `fd500` | FD-500 disk controller: Disk BASIC (option: the ROM's path; else `disk11.rom`) and its latch at `$FF40` |
| `sc2` | Disto Super Controller II: an FD-502-compatible WD1773 controller with a 256-byte no-halt buffer at `$FF74-$FF77` and the Mini-Expansion Bus at `$FF50` (option: `[ROM][,alt]`; below) |
| `sdc` | The CoCo SDC: a host folder as its SD card (option: `FOLDER[,FIRMWARE[,BANK]]`) |
| `burke` | Burke & Burke CoCo XT-RTC hard disk interface at `$FF50-$FF53` and its DS1215 clock (option: drive 0's raw image[,ROM]) |
| `becker` | The Becker port at `$FF41/$FF42` to mmsim's built-in DriveWire 4 server (option: `[ROM][,cow][,net]`; below) |
| `dosrom` | A DOS ROM at `$C000` with no hardware behind it and no autostart: HDB-DOS for DriveWire over the bit banger (`serial drivewire`; below) |
| `rompak` | A ROM Pak; the option its image |
| `gmc` | The Games Master Cartridge (bank-switched ROM and an SN76489); the option its image |
| `banked` | A banked ROM Pak as MAME's (16 KB banks, register at `$FF40`); the option its image |

### Bank-switched ROM Paks

`coco::BankedPak` (`coco_paks.h`). As XRoar's rombank the image (up to
256 KB) is padded with `$FF` to a power of two and seen 16 KB at a time at
`$C000-$FEFF`; reset selects bank 0; CART is on Q as a plain Pak's.

- `gmc`: John Linville's Games Master Cartridge, as XRoar's gmc.c: a write
  to an even address in `$FF40-$FF5F` selects bank D (masked to the
  image), an odd one goes to its SN76489 (4 MHz), mixed into the sound.
  XRoar runs Tandy's own bank-switched CoCo 3 Paks (RoboCop, Predator) as
  this: their bank register is at `$FF40` too.
- `banked`: MAME's "CoCo Program PAK (Banked)" (coco_pak.cpp, `banked
  16K`): only `$FF40` selects, bank D modulo the image; no sound.

**Auto-detection** (`cart`, `-i`; XRoar's cart.c): an image XRoar knows by
CRC-32 keeps its type (the CoCo 3's unbanked 32 KB Paks, e.g. Super
Pitfall, Rampage!, Arkanoid, Silpheed: a plain Pak; Mind Roll, RoboCop,
Predator, Dunjunz, Blockdown: `gmc`); any other image over 16 KB is a GMC,
16 KB or less a plain Pak. `expansion cart gmc|banked|rompak IMAGE`
chooses explicitly.

On the CoCo 3 the cartridge sees A14 inverted (as XRoar's coco3.c), so a
32 KB Pak's first half is at `$C000` in 16K mode and the whole image at
`$8000-$FEFF` in the GIME's 32K external ROM mode.

Not done: XRoar's MOOH (512 KB RAM with an MMU and SPI) and NX32 (RAM
expansion) are RAM cartridges, not ROM bank schemes; they are not
modelled.

Verified (November 2026, user-supplied images from the Color Computer
Archive, not in the repository; `test_coco_real_roms.cpp` repeats it
when they are in `roms/coco/paks/`): on the `coco3`, RoboCop (128 KB) and
Predator (64 KB) are detected as GMC-banked, switch banks and show their
title screens; Super Pitfall and Rampage! (32 KB) are detected as plain
Paks and ask their RGB-monitor questions; Mind-Roll (32 KB, banked) runs on
a `coco2`. No GMC with sound (Blockdown, Dunjunz) was tried.

### The Speech/Sound Cartridge

`coco::SpeechSoundPak`, after MAME's `coco_ssc` (tim lindner). The
cartridge is a TMS7040 microcontroller with its own firmware, 2 KB of
static RAM, an SP0256-AL2 and an AY-3-8913; the CoCo talks only to the
firmware, so mmsim **runs the firmware** on a TMS7000 core
(`src/plugins/devices/tms7000`, after MAME's) rather than imitating its
command language, which no document available here describes. The cost:
the TMS7040's mask ROM (`pic-7040-510.bin`) is needed, and without it the
cartridge isn't made (the message names the file).

- `$FF7D` (write): bit 0 high holds the TMS7040, the SP0256 and the AY
  in reset (the service manual's flip-flop IC4); low lets them run. The
  host's RESET clears it. Not readable (MAME returns `$FF`; the
  service manual's 74LS138 decodes only the two writes and the `$FF7E`
  read, at `$FF7C-$FF7F` with A7 high).
- `$FF7E` (write): a byte to the firmware (TMS7040 port A, its INT3), and
  BUSY until the firmware acknowledges it (port C bit 7 rising). Read:
  bit 7 low = BUSY, bit 6 the SP0256's SBY (high: silent), bit 5 low while
  the sound activity circuit hears the AY, bits 0-4 high.
- Inside, as MAME: port B the RAM's A0-A7, port C bits 0-2 A8-A10 (bit 0
  also the AY's BC1), 3 R/W (the AY's BDIR), 4 the RAM's /CS, 5 the
  SP0256's /ALD, 6 the AY's /CS, 7 BUSY's reset; port D the data bus; the
  SP0256's LRQ on INT1. Clocks: the TMS7040 and AY from E × 2 (1.79 MHz),
  the SP0256 from its 3.12 MHz crystal (10 kHz samples).
- The SP0256 (`src/plugins/devices/sp0256`, after MAME's / jzIntv's): the
  microsequencer and LPC-12 filter, driven from its ROM
  (`sp0256-al2.bin`). Without that ROM each allophone halts at once:
  silent, SBY and LRQ still answer (but SBY's busy time is then a sample,
  not the allophone's length).
- The cartridge's sound reaches the CoCo on the cartridge connector's SND
  pin, which is the analog multiplexer's cartridge input: software
  selects it (the owner's manual: `POKE &HFF01,52: POKE &HFF03,63` and
  `POKE &HFF23,60`). mmsim now routes every cartridge's audio (the
  Orchestra-90's too) through the multiplexer's source 2 instead of mixing
  it in always.
- The AY-3-8913 is mmsim's AY core (the Spectrum's, after Fuse); the 8913
  has no I/O ports, so registers 14-15 read back what was written. The
  sound activity circuit (`$FF7E` bit 5) and the mix are MAME's model.

What the documents settle (Tandy's owner's manual, service manual
26-3144A and schematic): the two addresses and the status bits (bit 7
BUSY*, bit 6 speaking, bit 5 sound active, all active low; the other bits
unused), the reset by writing 1 then 0 to `$FF7D`, the write latch and
BUSY flip-flop raising the PIC 7040's INT3 and cleared when the 7040 has
the byte, LRQ* on INT1, port A from the host, port B the RAM's address,
port C control, port D the internal bus, the 1.78 MHz clock (E XOR Q),
the 3.12 MHz SP0256 resonator, the mixer (sound x 2, speech x 1.5), the
4 kHz speech filter (not modelled) and the sound activity circuit (an
AC-coupled rectifier against 0.8 V, so volumes of 3 or less may not
register). Port C's individual bit assignments are MAME's (the
schematic scan is too coarse to read them); the service manual says the
7040 clears BUSY by "bringing C7 low", while MAME clears it on C7
rising: mmsim follows MAME (a difference of a few cycles). The owner's
manual's command language (`$80-$FF`: buffers, text-to-speech after a
carriage return, register access with `$AF`) is the firmware's and runs as
it is.

Not verified: neither ROM is available here, so the cartridge has only
been run with a stand-in firmware (the tests'); the demonstration
programs (SS1-SS7.CAS) need the firmware. Snapshots don't include the
cartridge.

### The CoCo Max Hi-Res Input Pack

`coco::CocoMax`, after MAME's `coco_max`: an 8-bit A/D converter for the
mouse (or joystick) plugged into the pack, at `$FF90-$FF97`. A read
returns the last conversion and starts the next of its channel: `$FF90`
Y, `$FF91` X (0-255), `$FF92` the left button (`$00` pressed), `$FF93`
the right; `$FF94-$FF97` start nothing. With the pack in (or in a
Multi-Pak slot) the host mouse moves it, a step a pixel, instead of the
joystick. It has no ROM in mmsim (MAME's has none). The CoCo 3's GIME is
at `$FF90`, so it is refused there (CoCo Max III uses the `cm3` joystick
adapter above).

Checked against: "Hi-Res Adapter (Colorware).txt" (a reverse-engineering
note: `$FF90` Y, `$FF91` X, `$FF92` the button; a modified pack at
`$FF60`) and CoCo Max II's own code on its disk, which reads a channel
as `LDY #$FF90: LDA B,Y`, waits a delay loop, then `LDA B,Y` again (and
`$FF92` the same way): the read-starts-a-conversion model. CoCo Max II
itself has not been run (Disk BASIC's DIR gives an I/O error from the
CLI with these images, with or without the pack: a separate problem).

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

### The Disto Super Controller II

`coco::DistoSc2` (`src/plugins/machines/coco/main/coco_storage.h`), a
`Fd500` with the SC-II's extras. Sources: the **Super Controller II
Instruction Manual** (CRC Computers), the Disto schematic **SCII13.SCH
rev 1.3** ("Disto FDC Model II", April 1993), MAME's `coco_fdc.cpp`
(`coco_scii`) and NitrOS-9's `rb1773.asm` (built with `SCII`). Fit it
with `expansion cart sc2` (Disk BASIC from `disk11.rom`), `expansion cart
sc2 cdos.rom`, `expansion cart sc2 alt` or in a Multi-Pak slot; disks
mount as on the FD-500 (`disk mount 1 game.dsk`).

From the manual and schematic:

- A WD1773; the drive latch at `$FF40` (mirrored to `$FF47`) and the
  WD1773 at `$FF48-$FF4B` (mirrored to `$FF4F`) as the Radio Shack
  controller's. At power-up it is in **normal mode**, where it is an
  FD-502: Disk BASIC (HALT on DRQ, INTRQ to /NMI) runs unchanged.
- `$FF50-$FF57`: the **Mini-Expansion Bus** (J1, 17 pins: RESET, E,
  A0-A2, D0-D7, /CE, R/W, +5 V, GND), eight locations for an add-on
  board; J2 puts an add-on's interrupt on the cartridge FIRQ (CART).
  Unlike a Radio Shack controller the SC-II decodes this area properly,
  so it is not a mirror of the latch. `coco::MebDevice` is the bus; no
  add-on board is modelled yet (Disto's real-time clock / printer
  "RTIME", 3-in-1, EPROM programmer and hard disk adapters), so with
  nothing on it `$FF50-$FF57` is not driven.
- `$FF58-$FF5F`: nothing, unless jumper J3 is moved to 2-3 (the option's
  `alt`), which puts the buffer's registers there instead of `$FF74`.
- **Buffered mode**: a 6116 RAM addressed by an 8-bit counter (74HC393,
  A8-A10 grounded: 256 bytes) between the WD1773 and the CPU. Its four
  registers at `$FF74-$FF77` (assigned to Disto by Tandy), A1 picking:
  - `$FF74`/`$FF75` buffer data: read or write the byte under the counter,
    which then counts up;
  - `$FF76`/`$FF77` control. Write: bit 0 1 = read (the WD1773's bytes go
    into the buffer), 0 = write (the buffer's bytes go to the WD1773), in
    buffered mode only; bit 1 buffered mode; bit 2 masks INTRQ from /NMI;
    bit 3 puts INTRQ on CART (FIRQ). Any write clears the counter. Read:
    bit 7 is INTRQ, inverted (0 = the command has ended).

  In buffered mode the board answers each DRQ itself and never asserts
  /HALT; the CPU runs on, then polls bit 7 (or takes the FIRQ) and reads
  the sector from `$FF74`. NitrOS-9's driver: READ SECTOR, `$07` to
  `$FF76`, wait for bit 7 low, `$04` (clears the counter), 256 reads of
  `$FF74`; to write, `$04`, 256 writes to `$FF74`, WRITE SECTOR, `$06`.
  512-byte sectors and formatting use HALT mode.
- The DOS socket on /CTS: a 24-pin 8 KB ROM, or a 2764, 27128 or 27256
  (jumpers J4-J6); 16 KB is visible at `$C000`, an 8 KB ROM repeats.

What could not be settled, and what mmsim does:

- **Which pair is data and which control.** The manual's table 2 puts
  control at `$FF74` and data at `$FF76`, but its own notes contradict it
  ("`$FF77` mirror of `$FF75`"; "any write to `$FF74` or `$FF75` clears
  the counter"). The schematic shows A1 (with R/W) choosing between the
  two; MAME and NitrOS-9's `rb1773` (which drives real boards) put data
  at `$FF74`/`$FF75` and control at `$FF76`/`$FF77`, and mmsim follows
  them. The alternate block is data `$FF58`, control `$FF5A` (rb1773's
  `SCIIALT`); A2 is not decoded there.
- **Bits 0-6 of a control read.** The manual documents only bit 7; mmsim
  returns the last value written in bits 0-6, as MAME (the schematic seems
  to drive D7 alone, which would leave the rest floating).
- **The 27256**: the manual says "a small modification is needed to access
  all of" it and doesn't say what; mmsim shows the image's first 16 KB
  (as MAME). A larger image is refused.
- The common 512-byte buffer upgrade (MAME's default, rb1773's
  `SCIIHACK`) is not modelled: the counter wraps at 256 as on a stock
  board.
- The DRQ is served at once; the board's own timing (its 16 MHz crystal
  and E-clock synchronisation) is not modelled.

With the real `disk11.rom`, Disk BASIC on the SC-II lists a disk with
`DIR` and SAVEs, LOADs and RUNs a program (`real_disk_basic_on_disto_sc2`,
which runs when the ROMs are in `roms/coco/`). Not yet run with Disto's
C-DOS or NitrOS-9's SC-II driver.

### The CoCo SDC

`coco::CocoSdcCart` (`coco_storage.h`) with `CocoSdc`
(`src/plugins/devices/coco_sdc`), on the CoCo 1, 2 and 3, in the port or
a Multi-Pak slot. The source is Darren Atkinson's **CoCo SDC User Guide**
(v4, 2020): its register table (chapter 1), image formats (chapter 6) and
command reference (chapter 7).

```
expansion cart sdc ~/coco/sd                       # the folder is the SD card
expansion cart sdc ~/coco/sd,~/roms/sdc-dos.rom    # and the flash image
expansion cart sdc ~/coco/sd,~/roms/flash.bin,1    # ... and the DIP switches' bank
disk mount 1 ~/coco/sd/GAMES.DSK                   # SDC drive 0 (or "M:" from the CoCo)
```

- **Registers** (CoCo scheme): `$FF40` drive control latch, `$FF42` flash
  data, `$FF43` flash control, `$FF48` command/status (command mode),
  `$FF49-$FF4B` parameters (FDC track/sector/data registers in FDC
  emulation). The Dragon scheme (`DRGN` switch) is not modelled.
- **FDC emulation**: an FD-502: the latch at `$FF40` and a WD1793 at
  `$FF48-$FF4B` (the FD-500's code), its drives 0 and 1 the two images
  mounted, so stock Disk BASIC sees them.
- **Command mode**: `$43` to `$FF40` (0 returns to FDC emulation). Read /
  write logical sector `$80`/`$A0` (+ drive; bit 1 the single-sided LSN
  flag; bit 2 (read) / 3 (write) for 8-bit transfers through `$FF4B`),
  stream `$90` (512-byte blocks, `$D0` aborts), `$C0` + `I` (the image's
  32-byte directory record), `Q` (its size in sectors), `C` (the current
  directory's record), `>` (a listing page: 16 records), `+` (next disk in
  the set), `V` (version: 1.24); `$D0` + `#` (disk in set); `$E0` + a
  256-byte `X:path` block: `M`/`m` mount (as an image / raw blocks; `M:`
  alone ejects), `N`/`n` mount new (630-sector DSK, an SDF of the given
  cylinders and sides, or an empty raw file), `D` set directory, `L`
  start a listing, `K` make a directory, `X` delete. Status: `$01` BUSY,
  `$02` READY, `$80` FAILED with `$04` invalid path, `$08` hardware /
  CRC / no listing, `$10` not found / invalid LSN / no image, `$20` in use
  / not empty (and, without FAILED, a deleted data mark), `$40` write
  protected, per command as the guide lists them. Data blocks move two
  bytes a pair (`$FF4A` then `$FF4B`, or `LDD`/`STD`).
- **Images**: DSK (geometry by size as the guide's table: up to 720
  sectors single-sided, 1440 double-sided 40 tracks, 2880 80 tracks; at
  least 324), JVC and VDK headers, and SDF (read and written back). A DSK
  over 2880 sectors is a hard disk: LBA for all of it, its first 1440
  sectors to the FDC as 80 single-sided tracks (read only in mmsim, a
  copy). A directory mounted is a disk set (files ending 1-9).
  `STARTUP.CFG` (`0=`, `1=`, `D=`) is read at power-on. Names are 8.3,
  matched without regard to case, with `*`/`?` and "name.*" for a missing
  extension; paths cannot leave the folder.
- **Flash**: 8 banks of 16 KB at `$C000`; the DIP switches' bank at reset.
  The guide gives no bit layout for `$FF42`/`$FF43`: mmsim takes `$FF43`
  bits 0-2 as the bank (unverified) and doesn't model programming.
- **Not done / differences**: rename (firmware 115 has one; the guide
  doesn't document it); the LED, the disk-switch button, DriveWire;
  command timing (results are ready when first polled). Not yet checked
  with SDC-DOS or a real card.

### Burke & Burke CoCo XT (XT-RTC)

`coco::BurkeBurke` with `XtHdc` (`src/plugins/devices/xt_hdc`) and a DS1215
clock. Checked against the **XT-RTC Rev D schematic** (WormFood's KiCad
transcription of a real board, with photos), **B&B's OS-9 boot ROM**
(an 8 KB XT-ROM dump, disassembled) and MAME's `isa/hdc.cpp` and
`machine/ds1215.cpp`; also the **CoCo XT & XT-RTC User's Manual v2.4**
(appendix A) and **Application Note 5**. The reference files are the
user's (`roms/coco/xt-rtc/`, not in the repository).

- **Address decoding** (schematic): the CoCo's A0-A15 go to the ISA slot
  with A4, A5, A6 (U3D-F) and A14 (U3A) inverted; AEN, BA16 and BA17 are
  tied low, BA18 and BA19 high. /IOR and /IOW are made from R/W alone (U2D,
  U2A; their other input is a 7400 output tied high, which the
  transcriber notes looks like a late change), so the controller's own
  decode of ISA `$320-$323` is what puts it at `$FF50` data, `$FF51`
  status (read) / reset (write), `$FF52` jumpers (read) / select (write),
  `$FF53` DMA/IRQ mask (write). Nothing on the card uses /SCS for the
  controller.
- **ROM socket**: /SMEMR is /CTS, so the controller's BIOS socket (ISA
  `$C8000`) answers at `$C000`, scrambled by the inverted A4-A6 (an
  XT-ROM: `expansion cart burke IMAGE,ROM` takes the EPROM as burned).
  B&B's ROM starts `DK` and runs from `$C002` like Disk BASIC.
- **Controller protocol** (the boot ROM, `$C56A-$C6F8`): reset, mask 0,
  wait for status & `$09` = 0, select, a DCB byte each time status & `$0F`
  = `$0D` (BUSY, C/D, REQ; `$0F` there means the command ended early),
  read data at `$0B`, write data at `$09`, the completion byte at status &
  `$07` = `$07`; completion bit 1 → request sense (`$03`, 4 bytes; codes 0
  and `$18` count as success). DCB: command, drive/head, cylinder bits 8-9
  / sector, cylinder bits 0-7, count, control. It uses recalibrate `$01`,
  read `$08`, seek `$0B`, test ready `$00` and `$0C` (8 bytes: cylinders,
  heads, reduced write current, precompensation, ECC burst). This is what
  `XtHdc` does; command codes, DCB, completion and sense bytes agree with
  MAME.
- **How it boots**: it waits for the drive (about 13 s, a key skips it),
  tests RAM, sets the Multi-Pak's /SCS slot to its own /CTS slot, resets
  the controller, recalibrates, reads physical sector 0 (cylinder 0, head
  0) to `$0600` and sends its bytes `$44-$45` (cylinders) and `$46`
  (heads) with `$0C`, then reads 9 sectors of **cylinder 128** (129 with
  a key held), head 0, to `$2600`. They must start `OS`; it jumps to
  `$2602`. Failing that it boots from floppy. The real-ROM test boots a
  hand-built image this way on a CoCo 2.
- **XT-RTC clock** (schematic U4, a DS1215 in ROM mode with a 32.768 kHz
  crystal and a 3 V cell): /CEI = /(SCS · E), /OE = /(R/W · E), /WE = A2,
  D = A0, Q = D0, RST = /RESET. So any `$FF40-$FF5F` cycle with A2 low,
  read or write, writes bit A0, and a read with A2 high reads a bit on
  D0; 64 bits of `$C5 $3A $A3 $5C $C5 $3A $A3 $5C` (low bit first) open
  the eight BCD registers (hundredths, seconds, minutes, hours, day,
  date, month, year) for 64 bits, as MAME's `ds1216e`. mmsim reads the
  host's clock (the same model as the Apple II No-Slot Clock); setting
  it isn't kept.
- The controller is a PC hard disk controller (WD1002-WX1 recommended;
  WD1002-27X, DTC 5150/5160, Adaptec 2072 work): "no halt", no interrupt
  or DMA. Sectors are 512 bytes, 17 a track. Images are raw (CHS order);
  until `$0C` the geometry is 4 heads and the file's length.

Still **unverified**: the ROM's repeat at `$E000-$FEFF` (it depends on how
much of ISA `$C8000-$CBFFF` the controller decodes); whether the
controller also answers `$FF54-$FF57` (ISA `$324-$327`); the jumper value
(0); status bit 4 (MAME: DMA request) and bit 5's exact rule; sense codes
other than 0/`$04`; D1-D7 when the clock answers (0 here); whether a
mismatched pattern bit needs a read (A2 high) before the DS1215 listens
again (mmsim, like AppleWin, needs one; MAME doesn't). Not modelled: since
/IOR and /IOW ignore /SCS and the controller decodes only A0-A9, the real
card would also see writes to `$x350-$x353` all through memory (every
1 KB) and, in a Multi-Pak, `$FF50` writes when another slot has /SCS;
mmsim's cartridge port only passes the selected slot's `$FF40-$FF5F`.
Not yet run with B&B's OS-9 driver or a real OS-9 hard disk image.

```
expansion cart mpi ; expansion mpi1 burke ~/coco/os9hd.img ; expansion mpi4 fd502
```

(XRoar's `ide.c`/`idecart.c` model the Glenside IDE cartridge, a
different interface that mmsim doesn't have.)

Disto's hard disk adapters (on the SC-II's Mini-Expansion Bus, or the
SCSI "H Disk" adapters) are not modelled.

### DriveWire and the Becker port

DriveWire (Cloud-9's; the DriveWire 4 protocol) puts a CoCo's disks, clock
and serial channels on a host computer at the far end of a byte stream.
mmsim has the host side built in: `DriveWireServer`
(`src/plugins/devices/drivewire`), which takes the guest's bytes and gives
back its answers, knowing nothing of the transport. The first transport is
the **Becker port** (Gary Becker's, from the CoCo3FPGA; XRoar's `becker.c`
talks to an external server over TCP instead):

| Address | Read | Write |
|---------|------|-------|
| `$FF41` | status: bit 1 set while a byte from the server waits | - |
| `$FF42` | the server's next byte (0 if none) | a byte to the server |

```
expansion cart becker roms/coco/hdbdw3bc3.rom      # the Becker port, HDB-DOS at $C000
disk mount 1 nos96809l2v030300coco3_becker.dsk     # DriveWire drive 0
DOS                                                # (typed at OK) boots NitrOS-9
```

`coco::BeckerCart` decodes the two registers on /SCS (in a Multi-Pak, the
slot `$FF7F` gives `$FF40-$FF5F` to) and answers /CTS with its option ROM
as a disk controller's DOS does (8 KB repeated over `$C000-$FEFF`); there is
no floppy controller on it. `disk mount N FILE` puts an image in DriveWire
drive N-1 (drives 0-255 are possible; `expansion` lists them). Images are
raw 256-byte sectors (LSN n at byte n x 256: NitrOS-9 and RS-DOS `.dsk`);
reading past the end gives zeros, writing there extends the file. Writes go
straight through to the file (as a real DriveWire server's), or with
`,cow` stay in memory. The server answers at once; NitrOS-9's Becker
`dwread` polls `$FF41` with a timeout, HDB-DOS's without one.

**Protocol coverage** (the DriveWire 4 specification on the DriveWire
wiki, checked against what NitrOS-9's `rbdw`, `dwio`, `clock2_dw` and
`boot_dw`, HDB-DOS and pyDriveWire do):

- Disks: `OP_READEX` `$D2` / `OP_REREADEX` `$F2` (256 bytes, the guest's
  16-bit checksum, then 0 or `$F3` E$CRC; an unreadable sector sends 256
  zeros and then its error), `OP_READ` `$52` / `OP_REREAD` `$72` (DW3:
  status, checksum, data), `OP_WRITE` `$57` / `OP_REWRITE` `$77` (0, `$F3`
  on a bad checksum, `$F2` E$WP read only, `$F6` E$NotRdy no image).
  The checksum is the 16-bit sum of the 256 bytes.
- System: `OP_DWINIT` `$5A` (answers 4: a DriveWire 4 server, so NitrOS-9's
  `dwio` installs its virtual-serial poll), `OP_TIME` `$23` (six bytes:
  year-1900, month, day, hour, minute, second from the host's local
  time), `OP_SETTIME` `$24` (taken, not applied), `OP_INIT` `$49`,
  `OP_TERM` `$54`, `OP_RESET1-3` `$FE` `$FF` `$F8`, `OP_NOP` `$00`,
  `OP_GETSTAT` `$47` / `OP_SETSTAT` `$53` (logged only).
- Virtual serial channels (below): `OP_SERREAD` `$43`, `OP_SERREADM`
  `$63`, `OP_SERWRITE` `$C3`, `OP_SERWRITEM` `$64`, `OP_FASTWRITE`
  `$80-$9F`, `OP_SERINIT` `$45`, `OP_SERTERM` `$C5`, `OP_SERGETSTAT` `$44`
  (advertised by the driver; no answer, but SS.KySns `$27` answers 0: Level
  1's `scdwv` reads a byte back there, Level 2's doesn't, and a new command
  drops answer bytes left unread) and `OP_SERSETSTAT` `$C4`
  (SS.Open `$29`, SS.Close `$2A`, SS.ComSt `$28` with PD.OPT's 26 bytes).
- `OP_PRINT` `$50` bytes are kept (`printBuffer`), `OP_PRINTFLUSH` `$46`;
  `OP_NAMEOBJ_MOUNT` / `CREATE` `$01` / `$02` answer 0 (failed).
- Any other opcode is one byte and ignored; a command whose bytes stop for
  250 ms of guest time (the spec's host timeout) is dropped, and a CoCo
  reset drops a half-received command and unread answers.

**HDB-DOS for the Becker port.** NitrOS-9's `_becker` disks are booted
the real way by HDB-DOS built for the Becker port (its `DOS` command loads
the boot track, LSN 612-629, from DriveWire drive 0). It is free (ToolShed)
but not in the repository; build it with LWTools:

```
# LWTools 4.24 from http://www.lwtools.ca/releases/lwtools/ (make; no install needed)
# ToolShed from https://github.com/nitros9project/toolshed
cd toolshed/cocoroms
lwasm --symbol-dump --no-output equates.asm bas.asm extbas.asm > ecb_equates.asm
cd ../hdbdos
make AS="lwasm -r --pragma=condundefzero,nodollarlocal,noindex0tonone" hdbdw3bc3.rom hdbdw3bck.rom
cp hdbdw3bc3.rom hdbdw3bck.rom <mmsim>/roms/coco/
```

`hdbdw3bc3.rom` (`-DDW -DLevel=2 -DBECKER`) is for the CoCo 3,
`hdbdw3bck.rom` (`-DLevel=1`) for the CoCo 1/2. Both sign on as "HDB-DOS
1.5 BECKER"; on power-up they read LSN 0 and the directory track looking
for AUTOEXEC.BAS. (A key typed the moment `OK` appears can be lost;
the test waits a second.)

**DriveWire over the bit banger.** On a real CoCo the usual DriveWire
link is the serial port's own lines (PIA1 PA1 out, PB0 in) and a cable to
the PC's serial port, the CoCo's software bit-banging 8N1 at 57600 baud
(the CoCo 1/2 at 0.89 MHz; 38400 for the CoCo 1's slower drivers) or
115200 (the CoCo 3 at 1.79 MHz). `serial drivewire` puts the built-in
server at the far end of that cable:

```
expansion cart dosrom roms/coco/hdbdw3cc3.rom      # HDB-DOS for the serial port, no controller
serial drivewire                                   # (or drivewire:38400, drivewire,cow,net)
disk mount 1 nos96809l2v030300coco3_dw.dsk         # DriveWire drive 0
DOS                                                # (typed at OK) boots NitrOS-9
```

The rate is the machine's (57600 on the CoCo 1/2, 115200 on the CoCo 3)
unless `:BAUD` gives another; `,cow` keeps writes in memory. While it's
attached `disk mount N FILE` puts an image in DriveWire drive N-1 (the
serial port answers before the cartridge; `disk mount N FILE cart` picks
the cartridge's drive); `serial none` takes it off. Only one DriveWire
server has the drives: with a Becker cartridge plugged in (or in a
Multi-Pak slot) `serial drivewire` is refused.

The guest's frames are decoded from PA1 as BASIC's printer output is (the
middle of each bit cell from the start bit's edge); the byte that
completes a command goes to the server, and its answer goes down PB0 8N1
at the same rate, its first start bit 500 us after that byte's stop bit
(`BitBanger::TURNAROUND_US`, the device option `turnaround` in
microseconds; a PC's serial port takes at least that), back to back after
it. The client's receive loop is waiting by then: NitrOS-9's and HDB-DOS's
`DWRead` look for the start bit every 7 cycles and sample each bit 15 or
16 cycles after the last (15.5 a bit: 0.894886 MHz / 57600 and 1.789773
MHz / 115200), from 18 cycles after they saw the start bit, so the line
is right only if PB0 is sampled on the cycle the read happens, which is
the bus-cycle timing above. Their `DWWrite` writes `$FF20` on the same
15/16 cycle grid.

HDB-DOS for the bit banger goes in a disk controller's ROM socket; it
needs no hardware of its own, so `dosrom` is a cartridge with just that
ROM at `$C000` (8 or 16 KB) and CART not tied to Q (no autostart). Build
it as the Becker one above:

```
make AS="lwasm -r --pragma=condundefzero,nodollarlocal,noindex0tonone" hdbdw3cc3.rom hdbdw3cc2.rom hdbdw3cc1.rom
cp hdbdw3cc3.rom hdbdw3cc2.rom hdbdw3cc1.rom <mmsim>/roms/coco/
```

`hdbdw3cc3.rom` (`-DDW -DLevel=2`) is the CoCo 3's (it switches to 1.79
MHz for each transfer: 115200), `hdbdw3cc2.rom` (`-DLevel=1`) the CoCo
2's (57600), `hdbdw3cc1.rom` (`-DLevel=1 -DBAUD38400`) the CoCo 1's
(38400: `serial drivewire:38400`). They sign on as "HDB-DOS 1.5 DW3 COCO
3" (2, 1).

**Checked**: byte-level tests of every transaction above; a 6809 program
reading a sector through the Becker port on a CoCo 2; with the local
HDB-DOS builds, NitrOS-9 v3.3.0's `_becker` disks (Level 2 6809 and 6309
on the CoCo 3, Level 1 on the CoCo 2) boot to the shell and run `dir`, and
`date` shows the host's date once `clock2_dw` has asked for it (it does at
each minute; sysgo first sets the build date). Over the bit banger: the
6809's bus cycles instruction by instruction (`m6809_dead_cycles_in_place`)
and PB0 read on the exact cycle (a line falling as LDA's read cycle starts
is seen low, a tick later high, on the CoCo 2 and the CoCo 3 at 1.79 MHz);
a cycle-counted 6809 client in the drivers' style (our own code) reads a
sector with READEX at 57600 on a CoCo 2 and at 115200 on a CoCo 3 (6809
and 6309) and fails at the wrong rate; with the local HDB-DOS builds the
`_dw` disks boot to the shell and run `dir`: Level 2 6809 and 6309 on the
CoCo 3 (115200), Level 1 on the CoCo 2 (57600) and the CoCo 1 (38400).

**Virtual serial channels.** NitrOS-9's `scdwv` driver puts SCF devices
on DriveWire channels, numbered by the descriptor's port: `/Term` (the
headless builds' console) 0, `/N1-/N13` 1-13, `/MIDI` 14, the windows
`/Z1-/Z7` (`/Z1-/Z15` possible) `$11-$1F`; `/N` is a wildcard that takes
the next free `/Nx`. The driver opens a channel with `OP_SERINIT`, writes
to it (`OP_FASTWRITE` `$80` + port, one byte; `SERWRITE`, `SERWRITEM`),
tells the server of each path's open and close (`SERSETSTAT` SS.Open /
SS.Close) and closes it with `OP_SERTERM`. Bytes for the guest wait at the
server until `dwio` polls, `OP_SERREAD` from a VIRQ (every 3-40 ticks,
faster while data flows), which the server answers, round robin over the
channels the guest has open (checked against `dwio.asm`'s IRQ handler and
pyDriveWire):

| Answer | Meaning |
|--------|---------|
| `0, 0` | nothing |
| `1 + port, byte` | one byte for serial channel 0-14 (sent this way while one or two wait) |
| `$11 + port, n` | n (up to 255) wait: the guest asks `OP_SERREADM port, count` (count up to n, the room in its buffer) |
| `$10, port` | the host closed serial channel `port`: the guest sends S$HUP to the processes with a path to it. Sent 0.5 s (guest time) after the guest last took a byte of the channel: S$HUP to a reader that hasn't yet run cuts its read short (`dw` died with error 4 when it came at once) |
| `$40 + (port & $0F), key` | a key for window `$10-$1F` |

Each channel's host side is the in-process API (`DriveWireServer::
hostWrite`, `channelOutput`, `hostClose`, for tests and the frontends)
and one of these far ends, set with the CLI's `drivewire` command
(`drivewire channel CH SPEC`, CH being `term`, `n1`-`n13`, `midi`,
`z1`-`z15` or a number):

| Spec | The far end |
|------|-------------|
| `tcp:PORT` | a listening socket on 127.0.0.1, one client at a time (`nc localhost PORT`, `telnet`); text in is telnet-style (LF and CR LF become SCF's CR, IAC sequences dropped); `tcp:PORT,raw` passes bytes as they come. A client hanging up a `/N` channel is the guest's S$HUP |
| `file:PATH` | what the guest writes, to a file |
| `loopback` | what the guest writes comes back |
| `modem` | (the default on `/N1-/MIDI`) the DriveWire server's virtual modem, below |
| `none` | (the default on `/Term` and the windows) the in-process API only |

```
expansion cart becker roms/coco/hdbdw3bc3.rom
disk mount 1 nos96809l2v030300coco3_becker_headless.dsk
drivewire channel term tcp:6809       # the headless console on a TCP port
drivewire channel n1 tcp:6810         # (then on the guest: shell i=/n1&)
DOS                                   # (typed at OK); then `nc localhost 6809`
drivewire                             # the drives and channels
drivewire type term dir               # a line to /Term from the CLI ...
drivewire read term                   # ... and what the guest wrote since
drivewire hangup n1                   # S$HUP to /N1's processes
```

The **virtual modem** answers the lines the guest writes to a `/N`
channel with no far end, as the DriveWire 4 server's API (NitrOS-9's
`dw`, `telnet` and `inetd` open `/N` and write commands there; the
replies are in the form `dw.as` and `net.as` read): `dw disk show`, `dw
disk eject N`, `dw server version` and `dw port show` answer `OK`, the
text, and then the channel closes (which ends `dw`); `tcp connect HOST
PORT` connects the channel to a TCP server (`OK connected to HOST:PORT`,
then the channel carries the connection until either end closes: the
guest's `telnet`) when network access is on; anything else answers `FAIL 208 not supported: ...`
(`dw ...` then closes; E$UnkSvc). `tcp listen` / `tcp join` (`inetd`),
the AT command set, `dw disk insert` and the rest of the DW4 server's
commands are not there.

**Network access is off by default**: `tcp connect` would let a program
in the guest open a connection to any host, so it answers `FAIL 205
network access is off` until the host allows it. Turn it on with `,net`
when fitting the server (`expansion cart becker [ROM],net`, `serial
drivewire,net`), with `drivewire network on|off` while it runs, with the
device option `network` (`devopt cart network on` for the Becker
cartridge, `devopt rs-232 network on` for the bit banger; a machine
JSON's device `"options": {"network": "on"}`), or in the GUI with the
**DriveWire network access** toggle in the Devices pane (the `Cartridge`
or `RS-232` device). `drivewire` alone shows the setting. While a
connection opens, the emulation waits for it.

A device option can also be **held until startup has finished**: `devopt
cart network on when started` waits until the guest's DriveWire driver
starts (it sends DWINIT, or DriveWire 3's INIT; HDB-DOS at its prompt
doesn't, NitrOS-9's boot does), and `devopt cart network on when 5` waits
five seconds of guest time from when it was set. A machine JSON's device
option takes `{ "value": "on", "when": "started" }` for the same.
`devinfo` and the GUI's Devices pane list the held options; a held option
that the device refuses when its time comes is shown there as "Held option
refused". The Becker cartridge's port and the bit banger take `network`,
and the bit banger also `baud` and `turnaround`, this way.

The `_becker_headless` and `_dw_headless` builds
boot to their shell on `/Term`; `test_coco_real_roms.cpp` runs `dir` there,
a second shell on `/N1`, and `dw disk show` (Level 2 on the CoCo 3, Becker
and bit banger; Level 1 on the CoCo 2, whose `dw` prints the reply but
then hangs the machine when the channel's close is announced: not
understood, see below).

**Unverified / not done**: the bit-banger link's behaviour against a
real CoCo (its exact sampling points within a cycle, the PIA's own
delays: all edges are at cycle boundaries here), a DriveWire server's
real turnaround time, the `/Z` windows with a guest (the release disks
have their descriptors but no window-aware program; the protocol is per
`dwio.asm`), Level 1's hang after a channel-close notice (`dw` on the
CoCo 2 headless build: the system stops polling once `dwio` takes `$10,
port`), `inetd`'s listen side, named objects, the
printer's output, `OP_SETTIME` changing anything, DW4's turbo mode, and a
TCP link to an external server (pyDriveWire, DW4) as XRoar's. The
CoCo3FPGA's exact decoding of the Becker port (mirrors, the WiFi variant
at `$FF6C`) is not modelled. HDB-DOS's floppy paths (it keeps Disk
BASIC's FDC code) find no controller on this cartridge.

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
expansion ram ram 2M                         # Disto's 1 or 2 MB MMU extension
expansion monitor rgb                        # a CM-8 (default: composite)
expansion monitor composite bluered          # NTSC artifact colours (or redblue, off)
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
whatever the map.

**Disto 1-2 MB** (`expansion ram ram 1M` / `2M`): the RAM is 512 KB banks,
chosen by a board that latches extra page-register bits, as XRoar models
it: a write to `$FFA0-$FFAF` keeps bits 6-7 (bit 6 only on 1 MB) as that
page's bank; `$FF9B` bits 0-1 choose the bank the video is fetched from.
With the MMU off, and in the constant page, the CPU sees bank 0. Both read
back (`$FFA0-$FFAF` bits 6-7 with the GIME's six; `$FF9B` bits 0-1). The
board shares the GIME's MMUEN, MC3 and task bit (XRoar keeps its own copies,
which /RESET leaves set); the bank bits, like the page registers, survive a
reset.

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
border starts), VBORD (the bottom border starts), EI2 (serial: the RS-232
receive line at space, so a start bit's falling edge; the line is PIA1
PB0's, fed by `serial in:`), EI1 (keyboard: any of PIA0 PA0-PA6 low,
including the fire buttons, re-read as each PIA0 write changes the
strobe, as XRoar does every cycle), EI0
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
grey; VMODE MOCH shows the intensity only). A line is drawn as the beam
goes (XRoar's model): before each write that could change it (the GIME's
registers, the palette, the SAM bits, PIA1 port B as snooped) the line is
drawn up to where the beam will be when the write's cycle ends, a whole
fetched byte at a time; the rest at the right border and the line's end.
So palette, mode and offset changes part way along a line show from the
next byte (character) on; the border colour and the line's horizontal
timing are taken as its left border starts. A CM-8 pulls PIA1 PB3 low (the
monitor detect, as on XRoar).

**PAL** (`coco3p`): a PAL CoCo 3 encodes its RGB outputs for the TV rather
than using the GIME's NTSC composite, so composite shows the RGB colours at
the outputs' measured levels (0, 1, 1.6, 2 V: `$00`, `$80`, `$CC`, `$FF`;
XRoar's measurement); MOCH, a property of the GIME's own composite output,
has no effect there. The board's encoder is unmeasured (XRoar approximates
it too), so finer differences are not modelled.

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
  the text rows' line count (`$0F`: 12).
- **Artifact colours** (composite, NTSC; `expansion monitor composite
  bluered` / `redblue`, or the **Artifact** button): a line drawn in a
  VDG-compatible graphics mode goes through a model of the TV: the line as
  a composite signal, a sample a pixel tick (four a colour cycle; a line is
  exactly 228 cycles, so the phase is the same on every line), decoded with
  a low-pass that nulls the subcarrier and its harmonic. A run of one
  colour decodes to its own composite colour; PMODE 4's dot pairs become
  colours. The GIME's subcarrier phase against the dots is fixed, but has
  not been measured, so it is the setting (`bluered`: a black dot then a
  white one is blue, as on a CoCo 2 in that phase). VMODE BPI inverts the
  burst, swapping them. CoCo 3 modes are shown without the decoder.

**Expansions**: `expansion ram ram 512K`; the FD-502 (`fd502`, the FD-500's
code with Disk BASIC 1.1), the Deluxe RS-232 Pak (`rs232`, the 6551 at
`$FF68`; `serial`), the CoCo SDC (`sdc`; its `$FF40-$FF4F` needs INIT0's
SCS bit, as on the real machine), Burke & Burke (`burke`), the Becker
port (`becker`, DriveWire), the
Orchestra-90 and ROM Paks, alone or in the
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
- EI2: XRoar leaves the GIME's serial input unconnected and has PIA1 PB0
  read low ("until I implement serial"); mmsim does the same with nothing
  connected. Once `serial in:` drives the line, EI2 follows it, idle at
  mark (PB0 high); that polarity at PB0 and EI2 follows the transmit
  side's (PA1 high at mark) and is not verified on hardware.
- Composite colours: mmsim uses XRoar's measured voltages as they are;
  XRoar's own palette adds a brightness lift (+0.2) and less saturation
  before its display transfer, which are display tuning rather than
  measurement.

**Checked against XRoar** (the settings most likely to need tuning, without
a real ROM to boot): the GIME's power-on and /RESET state (registers and
palette 0, page registers kept), VSC's text row heights in the
VDG-compatible modes (`$FF9C`, XRoar's row masks), the interrupt
enable/acknowledge rules and the timer's reload and rates are as XRoar has
them; the keyboard's EI1 is now re-read on each PIA0 write.

**Checked with the real ROMs** (local only): Super Extended Color BASIC 2.0
(`coco3.rom`) boots and runs BASIC; with the FD-502 and Disk BASIC 1.1,
`DIR` lists a disk, and `DOS` boots the OS-9 Level 2 v02.00.01 system disk
(TERM_VDG) to its `Time ?` prompt, after which `mdir` lists the modules.
That needed two fixes: the 6809 sees /HALT only at the end of the
instruction after the one that raises it (Disk BASIC's read loop stores each
sector's last byte before INTRQ's NMI), and the WD279x reads DDEN during
its ID search (OS-9's boot driver selects double density after writing
READ SECTOR).

**NitrOS-9 v3.3.0** (the project's release disks, from
sourceforge.net/projects/nitros9, kept in `roms/coco/nitros9/`) boots to
its shell and lists its root directory with `dir` on:

| Build | Machine | Booted by |
|-------|---------|-----------|
| Level 2, 6809 (`_40d_1`, `_80d`) | CoCo 3 | Disk BASIC `DOS`, FD-502 |
| Level 2, 6309 (`_40d_1`) | CoCo 3 with HD6309 (`coco3h`) | Disk BASIC `DOS`, FD-502 |
| Level 2, 6809 (`_cocosdc`) | CoCo 3 | Disk BASIC `DOS` in the CoCo SDC's flash; then its SDC driver by LSN |
| Level 1 (`coco2`, `coco1` `_40d_1`) | CoCo 2, CoCo 1 | Disk BASIC `DOS`, FD-502 / FD-500 |
| Level 1 (`d64` `_40d_1`) | Dragon 64 | DragonDOS 1.2 `BOOT` |
| Level 2, 6809 and 6309 (`_becker`) | CoCo 3 (`coco3h` for the 6309) | HDB-DOS (Becker) `DOS` over DriveWire; then `rbdw`/`dwio` |
| Level 1 (`coco2` `_becker`) | CoCo 2 | HDB-DOS (Becker) `DOS` over DriveWire |
| Level 2, 6809 and 6309 (`_dw`) | CoCo 3 (`coco3h` for the 6309) | HDB-DOS (`hdbdw3cc3`) `DOS` over DriveWire on the bit banger, 115200 baud; then `rbdw`/`dwio` |
| Level 1 (`coco2`, `coco1` `_dw`) | CoCo 2, CoCo 1 | HDB-DOS (`hdbdw3cc2`, `hdbdw3cc1`) over the bit banger, 57600 / 38400 baud |

The Dragon's boot needed a CLI fix: DragonDOS waits for the disk's FIRQ
with `SYNC` and I and F set, which the 6809 treated as the end of a
program (it is, on a bare CPU: nothing can wake it). Now that is so only
while nothing drives the CPU's interrupt lines; a machine's devices do.
`test_coco_real_roms.cpp` boots each build when its disk is there (the
Dragon's text is drawn on a graphics screen, so it checks that `dir` is
loaded from CMDS instead of reading the screen). The `_becker` builds
boot through the Becker port and the built-in DriveWire server (above),
the `_dw` builds through the same server on the serial port (`serial
drivewire`); the `_becker_headless` and `_dw_headless` builds (their
console DriveWire channel 0) take `dir`, a second shell on `/N1` and `dw
disk show` through the server's virtual channels.

## Not done

- MOOH and NX32 RAM cartridges.
- The Speech/Sound Cartridge with its real firmware and allophone ROM
  (not available here); the CoCo Max 3 adapter's timing (MAME's guess).
- CoCo 3: artifact colours in the CoCo 3 modes (only the VDG-compatible
  graphics are decoded); a PAL composite encoder.
- Storage: Disto's Mini-Expansion Bus boards (clock, printer, hard disk
  adapters) and the Glenside IDE cartridge;
  the CoCo SDC's flash programming; DriveWire's named objects, `inetd`'s
  `tcp listen`. The SDC is not yet checked with
  SDC-DOS, nor Burke & Burke with HDB-DOS or a NitrOS-9 hard disk.

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
pixels, lines per row, the offsets and VSC, the VDG-compatible modes,
palette, border, mode and CSS changes part way along a line, the Disto
banks, the artifact decoder in both phases and with BPI, PAL composite) and a
CoCo 3 running a stand-in ROM that sets up a 40-column screen (plus EI2 from
the receive line, the 2 MB board through the bus, and the monitor options).
`src/plugins/devices/coco_sdc/test`, `xt_hdc/test` and
`test_coco_storage.cpp` cover the CoCo SDC (command mode, LSN reads and
writes in 16- and 8-bit modes on hard disk, DSK and SDF images, the
single-sided flag, raw mounts and streaming, mount by name, listings,
directories, new / make directory / delete, disk sets, STARTUP.CFG,
flash banks, a stand-in firmware in its flash reading a sector on a
CoCo 2) and the XT controller (reads across tracks, writes, geometry,
sense, the sector buffer; Burke & Burke's ROM socket and XT-RTC clock,
read bit by bit and by a 6809 program in a stand-in XT-ROM; Burke & Burke
and the SDC in a Multi-Pak on a CoCo 3) and the Disto Super Controller II
(buffered reads and writes, the address map and jumper J3, INTRQ to FIRQ
and the NMI mask, a Mini-Expansion Bus board, and a 6809 reading a sector
without HALT on a CoCo 2 and through a CoCo 3's Multi-Pak).
`test_coco_real_roms.cpp` boots B&B's own boot ROM from a hand-built hard
disk image when `roms/coco/xt-rtc/` has it.
`src/plugins/devices/drivewire/test` covers the DriveWire server byte by
byte (READEX with its checksum, E$CRC and REREADEX, the DW3 READ, WRITE and
REWRITE with a bad checksum, writes past the end, copy-on-write and read
only, TIME, SETTIME, DWINIT, INIT/TERM/RESET, the printer's opcodes, named
objects, unknown opcodes and the command timeout; the virtual channels:
SERREAD's single-byte, multi-byte (with SERREADM), window-key and
channel-closed answers, round robin, SERWRITE / SERWRITEM / FASTWRITE per
channel, SS.Open / SS.Close / SS.ComSt, a cooked far end, the virtual
modem's `dw` replies and FAIL, `tcp connect` to a local socket and its hang-up, the CLI's command);
`test_coco_storage.cpp` the Becker port's registers (also in a Multi-Pak)
and a 6809 reading a sector through it on a CoCo 2.
