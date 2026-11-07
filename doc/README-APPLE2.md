# mmemu-plugin-apple2: Apple II, II+ and IIe

The Apple II family (#12), after AppleWin (GPL-2.0-or-later): the memory
map and soft switches (`Memory.cpp`), the language card
(`LanguageCard.cpp`), the keyboard (`Keyboard.cpp`), the speaker
(`Speaker.cpp`), paddles and buttons (`Joystick.cpp`) and the video (a port
of `NTSC.cpp`, see below) and the Disk II (`Disk.cpp`).

## Profiles

| Profile | Machine |
|---------|---------|
| `apple2` | Apple ][: Integer BASIC and the original monitor (starts at the `*` prompt) |
| `apple2plus` | Apple ][+: Applesoft and the autostart monitor (starts at the `]` prompt) |
| `apple2e` | Apple //e: 128 KB, 80 columns, NMOS 6502 |
| `apple2e-enhanced` | Apple //e enhanced: 65C02, MouseText, the enhanced ROMs |

The II and II+ have 48 KB of RAM and a 16 KB language card in slot 0; the
IIe has 64 KB (the language card built in) and an extended 80-column card
with 64 KB more in the aux slot. The CPU is an
NMOS 6502 at 1.020484 MHz (14.318181 MHz / 14 with a long cycle every 65).

ROMs (AppleWin's resource images, in `roms/apple2/`): `Apple2.rom`,
`Apple2_Plus.rom` (12 KB at `$D000-$FFFF`), `Apple2_Video.rom` (the
character generator); `Apple2e.rom`, `Apple2e_Enhanced.rom` (16 KB at
`$C000-$FFFF`: the internal `$C100-$CFFF` ROM, then `$D000`),
`Apple2e_Enhanced_Video.rom` (the IIe's 4 KB character ROM with the
alternate set; the original IIe has no MouseText).

## Memory map

| Address | |
|---------|--|
| `$0000-$BFFF` | RAM (text page 1 `$0400`, page 2 `$0800`; hi-res page 1 `$2000`, page 2 `$4000`) |
| `$C000-$C0FF` | soft switches and slot I/O |
| `$C100-$CFFF` | slot ROMs (empty slots: the floating bus) |
| `$D000-$FFFF` | ROM, or the language card's RAM |

## Soft switches

| Address | |
|---------|--|
| `$C000` | keyboard: the last key's ASCII code, bit 7 set until `$C010` |
| `$C010` | clear the keyboard strobe |
| `$C030` | toggle the speaker |
| `$C050` / `$C051` | graphics / text |
| `$C052` / `$C053` | full screen / mixed (4 lines of text) |
| `$C054` / `$C055` | page 1 / page 2 |
| `$C056` / `$C057` | lo-res / hi-res |
| `$C058-$C05F` | annunciators 0-3 off / on |
| `$C061-$C063` | buttons 0-2 (bit 7) |
| `$C064-$C067` | paddles 0-3: bit 7 set until the paddle's timer runs out |
| `$C070` | start the paddle timers (a position takes 2816/255 = 11.04 cycles a step) |
| `$C080-$C08F` | the language card |

Reads of switches that have no data, and of empty slots, return the
**floating bus**: the byte the video is fetching at that cycle (AppleWin's
`MemReadFloatingBus`). Accesses happen on their own cycle within an
instruction (the bus counts them), so the video and the speaker see them
where they occur.

### Language card

As AppleWin (`LanguageCardUnit::IO`, after *Understanding the Apple IIe*,
5-23): bit 3 picks bank 1 (`$C088-$C08F`) or bank 2 (`$C080-$C087`) of the
4 KB at `$D000`; reads come from RAM when address bits 0 and 1 are equal
(`$C080`, `$C083`, ...); an odd address read twice in a row (or written by
a read-modify-write instruction after its read) write-enables the RAM, an
even one write-protects it. At power-on: ROM read, bank 2, write enabled.
The RESET key leaves it alone (on a II/II+ the card does not see RESET).

## IIe

After AppleWin's `Memory.cpp` (`MemSetPaging`, `UpdatePaging`, `IO_Cxxx`)
and *Understanding the Apple IIe*:

| Write | |
|-------|--|
| `$C000` / `$C001` | 80STORE off / on: PAGE2 picks main or aux for `$0400-$07FF` (and `$2000-$3FFF` with HIRES) |
| `$C002` / `$C003` | RAMRD: read `$0200-$BFFF` from main / aux |
| `$C004` / `$C005` | RAMWRT: write `$0200-$BFFF` to main / aux |
| `$C006` / `$C007` | INTCXROM: slot ROMs / the internal ROM at `$C100-$CFFF` |
| `$C008` / `$C009` | ALTZP: main / aux zero page, stack and language card |
| `$C00A` / `$C00B` | SLOTC3ROM: internal / slot ROM at `$C300` |
| `$C00C` / `$C00D` | 80 columns off / on |
| `$C00E` / `$C00F` | the alternate character set off / on |
| `$C05E` / `$C05F` | double hi-res on / off (and annunciator 3) |

| Read | bit 7 |
|------|-------|
| `$C010` | any key down (and clears the strobe) |
| `$C011` / `$C012` | language card bank 2 / reading RAM |
| `$C013` - `$C018` | RAMRD, RAMWRT, INTCXROM, ALTZP, SLOTC3ROM, 80STORE |
| `$C019` | not vertical blank |
| `$C01A` - `$C01F` | TEXT, MIXED, PAGE2, HIRES, ALTCHARSET, 80COL |
| `$C07F` | double hi-res (enhanced IIe) |

The status reads give the key code in bits 0-6. With SLOTC3ROM off, an
access to `$C3xx` switches the internal `$C800-$CFFF` ROM in until `$CFFF`
is touched (the 80-column firmware). The language card's aux banks follow
AppleWin: bank 2 at aux `$D000`, bank 1 at aux `$C000`.

Control-RESET on a IIe resets the memory switches, the language card (ROM
read, bank 2, write enabled) and the video modes (AppleWin `CtrlReset`).
The built-in self-test (hold Open Apple and Closed Apple, then
Control-RESET) passes: "System OK" on the enhanced IIe, "KERNEL OK" on the
original once the keys are let go.

## Keyboard

The II and II+ keyboards give upper case only. The IIe has lower case and
Caps Lock (`caps_lock`; on at power-on, as AppleWin): letters are upper
case with Caps Lock or Shift. Open Apple (`alt`) and Closed Apple
(`right_alt`) are buttons 0 and 1. Typed text keeps its case on a IIe. Keys come from the frontends
by name (US layout; Shift and Control are tracked: `Ctrl`+letter gives the
control code), or as typed text (CLI `type`, MCP). Return is `$0D`, the
left arrow and Backspace `$08`, the right arrow `$15`, Esc `$1B`. The
**RESET** screen button (`key reset`) restarts the CPU.

## Speaker

Each access to `$C030` flips the cone; the level is averaged over each
44.1 kHz sample and run through a DC-blocking filter.

## Paddles and buttons

Joystick port 0 drives paddles 0 (left/right) and 1 (up/down) and button
0; port 1 paddles 2/3 and button 1: the paddle reads 0 or 255 with the
stick pushed, 127 when centred.

## Video

`Apple2Video` (`src/plugins/devices/apple2_video/`) is a port of AppleWin's
NTSC renderer (`NTSC.cpp`, `NTSC_CharSet.cpp`; William S Simms, Michael
Pohoreski, Tom Charlesworth): AppleWin's default monitor, "Color (TV)" with
50% scan lines. Its chroma, luma and signal filters and colour tables match
AppleWin's (checked entry by entry). The beam runs 65 cycles a line, 262
lines a frame (17030 cycles, 60 Hz), and every line is drawn as the beam
passes it, so a mode switch in mid-frame or mid-line shows where it
happened.

- Text (40 columns; flashing characters every 16 frames, inverse), lo-res
  (16 colours), hi-res (280 x 192 with the composite colours and fringes,
  the high bit's half-dot delay), mixed mode (text from line 160), page 2.
  The IIe modes (80 columns, double lo-res and hi-res, the alternate set
  with MouseText) are in the port for the IIe stage.
- The picture is 560 x 384: 40 columns of 14 dots, 192 lines each drawn
  twice, the second at half brightness (AppleWin's 50% scan lines); no
  border.
- The floating bus is the byte the video fetches one cycle behind the beam
  (AppleWin's `NTSC_VideoGetScannerAddress`), from main RAM; the II/II+
  scanner adds `$1000` during horizontal blanking as the hardware does.

Left out: the other monitor types (idealised colour, RGB, monochrome),
50 Hz, the clones' character sets.

## Disk II

The Disk II interface card in slot 6 with two drives, after AppleWin's
`Disk.cpp` (its "authentic" path for `.dsk` and `.nib` images):

| Address | |
|---------|--|
| `$C600-$C6FF` | the boot ROM (P5, 16 sectors: `roms/apple2/DISK2.rom`; 13 sectors, `DISK2-13sector.rom`, for a WOZ that says it boots DOS 3.2) |
| `$C0E0-$C0E7` | stepper phases 0-3 off / on |
| `$C0E8` / `$C0E9` | motor off / on (it runs on for a second after off) |
| `$C0EA` / `$C0EB` | drive 1 / 2 |
| `$C0EC` | read or write a nibble (the data latch) |
| `$C0ED` | load the latch (write mode); sense write protect (`$FF` protected) |
| `$C0EE` / `$C0EF` | read / write mode |

Even addresses read the latch; odd ones the floating bus. The head moves
toward an adjacent magnet that is on, a half track at a time, 10 cycles
after the change (two adjacent magnets turned off within that time leave it
where it is). The disk passes a nibble every 32 cycles; a read within 6
cycles of the last sees part of the nibble; an empty drive reads noise.

The II+ autostart ROM boots slot 6 at power-on; with no disk in drive 1 it
waits, as the real machine does: RESET gives Applesoft. The `apple2`
(original monitor) does not boot by itself: `C600G` or `PR#6`.

Images (AppleWin `DiskImageHelper.cpp`):

- `.dsk`, `.do` (DOS 3.3 sector order), `.po` (ProDOS order); a `.dsk`'s
  order is found from its catalog. 35 tracks of 16 × 256 bytes (up to 40);
  each track is nibblized as DOS 3.3 formats it (volume 254, 6-and-2
  encoding, each track starting 768 nibbles on) and written back into the
  file when the head leaves a written track or the drive stops.
- `.nib`: 6656 nibbles a track, as recorded.
- `.woz` (WOZ1, WOZ2): the bit stream of each quarter track, see below.

### WOZ

After AppleWin's WOZ path (`Disk.cpp` `DataLatchReadWriteWOZ`,
`DataLatchReadWOZ`, `DataLoadWriteWOZ`, `DataShiftWriteWOZ`,
`FindTrackSeamWOZ`; `DiskImageHelper.cpp` `CWOZ1Image`, `CWOZ2Image`):

- The INFO chunk (5.25" disks only; write protect; WOZ2's boot sector
  format and optimal bit timing), TMAP (quarter tracks to tracks), TRKS
  (WOZ1: 6656-byte tracks; WOZ2: 512-byte blocks and a length in bits).
  WRIT, META and WOZ 2.1's FLUX chunks are skipped (FLUX tracks are not
  emulated, as in AppleWin).
- The disk turns a bit cell every optimal bit timing (32 = 4 us; Border
  Zone's 28 = 3.5 us), the remainder carried over. Each access runs the
  cells since the last one through the sequencer (after a gap of more than
  100, the rest are skipped so it re-syncs): the MC3470's head window makes
  up bits (~30% ones) after three zero cells (weak bits, fake bits); the
  shift register latches a nibble when bit 7 sets, and holds it 7 us, longer
  while zeroes follow. `$C08D` resets the sequencer (offset nibble streams).
- With two adjacent magnets on the head stops between them: quarter tracks
  (half tracks for `.dsk`/`.nib`, as AppleWin). Changing track keeps the
  head's place in proportion to the two tracks' lengths (cross-track sync).
- AppleWin's two jitters for copy protections: a slipped bit cell after
  the boot ROM reads track 0 sector 0's address (GH#930), and at the track
  seam from track 33 on (GH#1125).
- Writes shift the latch out a bit cell at a time. A written track goes
  back into the file with a new header CRC; a write to an unrecorded track
  adds one at the end of TRKS (seen from the quarter tracks on either
  side), so a blank WOZ can be formatted. Unrecorded tracks read as noise.

Checked with the WOZ reference test images (applesaucefdc.com): the DOS 3.3
and 3.2 System Masters (WOZ2) boot; Blazing Paddles and Take 1 (cross-track
sync) reach their menus; Hard Hat Mack, The Bilestoad (half tracks), Dino
Eggs, Crisis Mountain, Miner 2049er II, The Print Shop Companion (fake
bits), First Math Adventures (latch lifespan), Stickybear Town Builder
(offset streams), Bouncing Kamungas, Commando, Planetfall, Rescue Raiders,
Sammy Lightfoot, Stargate on a II+; Wings of Fury, The Apple at Play and
Border Zone on the enhanced IIe. INIT formats a blank WOZ2 that then
boots. A WOZ1 has no boot sector format, so its DOS 3.2 master needs the
13-sector ROM chosen by hand (not yet offered).

Disks go in units 1 and 2: CLI `disk mount 1 dos33.dsk`, the GUI drive pane,
MCP `mount_disk`. A read-only file is a write-protected disk.

## Not yet emulated

- WOZ 2.1 FLUX tracks; 13-sector `.dsk` images (`.d13`); a way to pick the
  13-sector ROM for a WOZ1; `.zip`/`.gz` images (#233).
- The IIc, the IIgs; RamWorks memory; the IIe without its 80-column card.
- Cassette input and output.
