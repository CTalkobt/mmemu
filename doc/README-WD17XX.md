# mmemu Plugin: Western Digital WD17xx / WD279x Floppy Disk Controller

**Plugin file:** `lib/mmemu-plugin-wd17xx.so`
**Device IDs:** `"fd1771"`, `"fd1791"`, `"fd1793"`, `"fd1795"`, `"fd1797"`, `"wd2791"`, `"wd2793"`, `"wd2795"`, `"wd2797"`, `"wd1773"`
**Source:** `src/plugins/devices/wd17xx/`

## Overview

The WD17xx family is the floppy controller of the TRS-80 Color Computer's
FD-500/501 cartridge (WD1793 / WD2793, #331) and of the TRS-80 Model I
Expansion Interface (FD1771) and Model III/4 (FD1793, #330). One class,
`Wd17xx`, covers every variant; a track-level image layer, `FloppyDisk`,
loads and saves JVC, VDK, DMK, JV1 and JV3 images.

The machine owns the controller: it maps the four registers, drives
drive-select / side / motor / density from its own latch, and wires INTRQ
and DRQ to its NMI, IRQ or HALT logic (the CoCo's DRQ→HALT is the
cartridge's job, not the controller's).

## Variants

| Variant | Data bus | Side | Density | Type II bit 3 / bit 1 |
|---------|----------|------|---------|-----------------------|
| FD1771 | true | — | FM only | b (IBM lengths / 16 x N) / a1 (data mark) |
| FD1791, WD2791 | inverted | compare | FM / MFM | S (side) / C (compare) |
| FD1793, WD2793, WD1773 | true | compare | FM / MFM | S / C |
| FD1795, WD2795 | inverted | SSO output | FM / MFM | L (length table) / U (SSO) |
| FD1797, WD2797 | true | SSO output | FM / MFM | L / U |

On the inverted-bus parts every register reads and writes complemented, as on
the bus. On SSO parts the U bit picks the side; on the others `setSide()` does.

## Registers

| Address & 3 | Read | Write |
|-------------|------|-------|
| 0 | Status (clears INTRQ unless `$D8` is in force) | Command |
| 1 | Track | Track |
| 2 | Sector | Sector |
| 3 | Data (clears DRQ) | Data (clears DRQ) |

## Commands

| Type | Command | Code | Notes |
|------|---------|------|-------|
| I | Restore | `$0x` | TR = $FF, DR = 0, steps out to track 0; seek error after 255 steps without track 0 |
| I | Seek | `$1x` | steps to the data register's track |
| I | Step / Step In / Step Out | `$2x` `$4x` `$6x` (+`$10` = u) | u updates the track register |
| II | Read Sector | `$8x` (+`$10` = m) | DAM within 30 (FM) / 43 (MFM) bytes |
| II | Write Sector | `$Ax` (+`$10` = m) | DRQ 2 bytes into gap 2; first byte needed 11 (FM) / 22 (MFM) bytes after the ID |
| III | Read Address | `$Cx` | 6 bytes; the ID's track goes to the sector register |
| III | Read Track | `$Ex` | every byte from index to index |
| III | Write Track | `$Fx` | from index to index; first byte within 3 byte times |
| IV | Force Interrupt | `$Dx` | I0 not-ready→ready, I1 ready→not-ready, I2 each index, I3 immediate |

Type I bits: h (3) load head, V (2) verify, r1 r0 step rate: 6/12/20/30 ms
(179x, 1773) or 12/12/20/40 ms (1771) at a 1 MHz controller clock, half at
2 MHz (`setFdcClockHz`). Verify and E wait 30 ms (1771: 20 ms) for the head
to settle. m (multiple) counts the sector register up until record not
found; a data CRC error ends it.

Write Track special bytes: MFM `$F5` = `$A1` sync with the CRC preset, `$F6`
= `$C2`, `$F7` = the two CRC bytes, `$FE` = an ID address mark (recorded in
the track's IDAM table); FM `$F7` = CRC, `$F8`-`$FB` data marks and `$FE`
an ID mark, each presetting the CRC.

## Status

| Bit | After Type I | After Type II / III |
|-----|--------------|---------------------|
| 7 | not ready | not ready |
| 6 | write protect | write protect (1771 read: record type high bit) |
| 5 | head loaded | record type: 179x `$F8`/`$F9` deleted; 1771 bits 6-5 = 0 `$FB`, 1 `$FA`, 2 `$F9`, 3 `$F8` |
| 4 | seek error | record not found |
| 3 | CRC error | CRC error |
| 2 | track 0 | lost data |
| 1 | index pulse | DRQ |
| 0 | busy | busy |

A Force Interrupt with no command running switches the status back to Type I.

## Timing

Everything counts the machine's cycles (`setClockHz`). The disk turns at
300 RPM (`setRpm`); the index pulse is the first ~5 ms of each turn. A track
of L byte slots passes in one turn, so a 6272-slot 5.25" track gives a byte
every ~32 us in MFM and every ~64 us in FM (FM bytes take two slots).
Searches fail after five index pulses. A byte read while the last one is
still unread sets lost data and replaces it; a byte to write not supplied in
time is written as `$00` with lost data. The head unloads after 15 idle
turns. Master reset (`reset()`) loads sector 1 and runs a Restore (`$03`),
which ends with INTRQ.

## Drives

Four drives, each 80 cylinders and two sides unless `setDriveGeometry` says
otherwise, with an image, a write-protect switch and a head position.
READY is "the selected drive has a disk and the motor is on"
(`setReadyMode(DiskPresent)` or `Always` for other wiring); index pulses and
data need a disk and the motor on. Type II/III commands on a drive that
isn't ready end at once with INTRQ and not ready.

## Disk images (`FloppyDisk`)

Each track is held raw, as in DMK: one slot per MFM byte time, FM bytes
stored twice, plus the slots of its ID marks (bit 15 = MFM). Sector formats
are formatted into tracks when loaded (IBM gaps, shortened if the sectors
need it) and read back out of the tracks when saved.

| Format | Detection | Notes |
|--------|-----------|-------|
| JVC (`.dsk`, `.jvc`, `.os9`) | default | header = file size mod 256: sectors/track, sides, size code, first sector, attribute flag. Default 18 x 256 MFM from 1, one side; ≥ 88 tracks one-sided means two sides; headerless OS-9 geometry from LSN0 |
| VDK (`.vdk`) | `dk` signature | cylinders, heads, write protect; 18 x 256 MFM; compressed images refused |
| DMK (`.dmk`) | header sanity + size | flag `$10` one side, `$40` FM bytes stored once; byte 0 `$FF` = write protected |
| JV1 (`.jv1`) | size a multiple of 2560 (not of 4608) | 10 x 256 FM sectors 0-9, one side; track 17 (directory) data mark `$FA` |
| JV3 (`.jv3`) | valid header + data size | 2901 entries (track, sector, flags: `$80` MFM, `$60` data mark, `$10` side, `$08` CRC error, `$03` size), write-protect byte; a second block after a full first one |

Writes mark the image dirty; the controller saves it to its file half a
second (emulated) after the last write, on eject, `flush()` and destruction.
A file that can't be written keeps the changes in memory.

## Using it from a machine

```cpp
#include "plugins/devices/wd17xx/main/wd17xx.h"

auto fdc = std::make_unique<Wd17xx>(Wd17xx::Variant::WD2793, 0xFF48);
fdc->setClockHz(894886);                      // the CPU clock tick() counts
fdc->onIntrq([&](bool on) { /* NMI / end of HALT */ });
fdc->onDrq([&](bool on) { /* release HALT */ });
fdc->mountDisk(0, "disk.dsk");                // or insertDisk(0, FloppyDisk::load(...))

// The FD-500 latch at $FF40
fdc->selectDrive(drive);                      // -1: none
fdc->setSide(side);
fdc->setMotor(motorOn);
fdc->setDoubleDensity(!(latch & 0x20));

// Registers at $FF48-$FF4B: ioRead/ioWrite, or readRegister/writeRegister(addr & 3)
// Each CPU step: fdc->tick(cycles);
```

INTRQ is also available as `setIrqLine(ISignalLine*)` and both outputs as
`setSignalLine("intrq"/"drq", line)`, or polled with `intrq()` / `drq()`.

Options (`setOption`): `variant`, `base`, `rpm`, `fdcclock`, `ready`
(`motor`, `disk`, `always`), `drive0`-`drive3` (an image path).

## Not emulated

- Raw flux, weak bits, write precompensation, data separator behaviour; FM
  clock patterns (address marks are found from the IDAM table, data marks
  by value).
- WD1770/1772 motor-on and spin-up; the 1773's 8 MHz quirks beyond fixed
  step/settle times.
- The datasheet's exact head-load timing (HLT) and the delay before a
  forced interrupt takes effect.
- DMK's "ignore density" flag (`$80`) beyond loading the bytes as stored.

## Tests

`src/plugins/devices/wd17xx/test/test_wd17xx.cpp` (controller) and
`test_floppy_images.cpp` (formats): `bin/mmemu-test "*wd17xx*" "*floppy*"`.

## References

FD179X-02 and FD1771 data sheets; XRoar `wd279x.c`, `vdisk.c`, `rsdos.c`;
MAME `wd_fdc.cpp`; Tim Mann's TRS-80 disk format notes (JV1, JV3, DMK).
