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
| `$FF40-$FF5F` | Cartridge I/O (/SCS): the FD-500, the CoCo SDC, Burke & Burke (`$FF50`) | |
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
writes what BASIC sends (8N1, 600 baud: BASIC's default). The receive
line (PIA1 PB0): `serial in:file.bin` sends a host file's bytes down it,
8N1 at the same rate (the device option `baud`), the line at mark
(high) between bytes. With nothing connected PB0 reads low (XRoar's
observation). On the CoCo 3 the line is
also the GIME's EI2 interrupt (below).

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
| `sdc` | The CoCo SDC: a host folder as its SD card (option: `FOLDER[,FIRMWARE[,BANK]]`) |
| `burke` | Burke & Burke CoCo XT hard disk interface at `$FF50-$FF53` (option: drive 0's raw image[,ROM]) |
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

### Burke & Burke CoCo XT

`coco::BurkeBurke` with `XtHdc` (`src/plugins/devices/xt_hdc`). From the
**CoCo XT & XT-RTC User's Manual v2.4** (appendix A) and **Application
Note 5**, these are settled:

- The interface decodes /SCS with A4 high: the controller's registers at
  `$FF50` data (read/write), `$FF51` status / force reset, `$FF52` read
  jumpers / start command, `$FF53` (write) select mode.
- The controller is a PC hard disk controller (WD1002-WX1 recommended;
  WD1002-27X, DTC 5150/5160, Adaptec 2072 work) with a 2 KB buffer: "no
  halt", and the CoCo XT doesn't use its interrupt. Sectors are 512 bytes
  (B&B's driver packs two 256-byte OS-9 sectors into each).
- The controller's BIOS ROM socket answers /CTS at `$C000` with address
  lines A4-A6 inverted (an XT-ROM): `expansion cart burke IMAGE,ROM`.
- It works on the CoCo 1, 2 and 3, normally in a Multi-Pak slot.

What the manual leaves out ("beyond the scope of this manual") is the
controller's own protocol: the status bits (REQ, I/O, C/D, BUSY), the
six-byte command block, the command set (read, write, seek, verify,
format, request sense, initialize drive characteristics `$0C`, sector
buffer, diagnostics) and 17 sectors a track are the IBM XT / WD1002
family's, written from memory and **unverified**, as is the `$FF52`
jumper value (0). The images are raw (CHS order); until `$0C` the
geometry is 4 heads and the file's length. The XT-RTC's clock isn't
modelled (its registers aren't documented). Not run with B&B's driver.

```
expansion cart mpi ; expansion mpi1 burke ~/coco/os9hd.img ; expansion mpi4 fd502
```

(XRoar's `ide.c`/`idecart.c` model the Glenside IDE cartridge, a
different interface that mmsim doesn't have.)

The **Disto SC-II / SCSI** adapter is not modelled: no local reference
documents its registers.

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
SCS bit, as on the real machine), Burke & Burke (`burke`), the
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
them; the keyboard's EI1 is now re-read on each PIA0 write. Booting Super
Extended Color BASIC and OS-9 Level II (user-supplied ROMs and disks) is
still to be done.

## Not done

- The Speech/Sound Pak.
- The CoCo Max 3 style hi-res adapter (only the Tandy interface is
  modelled).
- CoCo 3: artifact colours in the CoCo 3 modes (only the VDG-compatible
  graphics are decoded); a PAL composite encoder.
- Storage: the Disto SC-II / SCSI adapter and the Glenside IDE cartridge;
  the CoCo SDC's flash programming. The SDC and Burke & Burke are not
  yet checked with SDC-DOS, HDB-DOS or NitrOS-9.

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
sense, the sector buffer; Burke & Burke's ROM socket; Burke & Burke and
the SDC in a Multi-Pak on a CoCo 3).
