# Commodore 1541 (true drive)

`Drive1541` (`src/plugins/devices/drive1541/`, device type `drive1541`) is a 1541 at the chip level (#341). Its own 6502 runs the DOS ROM against two 6522 VIAs and a disk under a rotating head. The C64's usual drive 8, `IecDrive` (README-IEC), answers the serial protocol itself, which is enough for loading and saving. The true drive is for software that runs code in the drive, or reads the disk below the DOS: fast loaders, copy protection, VICE's `drive/` test programs.

## The drive

| Address | Device |
|---|---|
| `$0000-$07FF` | 2K RAM (again at `$2000`, `$4000`, `$6000`: A11 and A12 low) |
| `$1800-$1BFF` | VIA1: the serial bus (16 registers, mirrored; again at `$3800`, `$5800`, `$7800`) |
| `$1C00-$1FFF` | VIA2: the disk head (likewise) |
| `$8000-$FFFF` | the 16K DOS ROM (`$C000-$FFFF`, mirrored at `$8000`) |

Other addresses read the last byte on the data bus (it floats; VICE testprogs `drive/openbus`). The ROM is `roms/c64/dos1541.bin`: 325302-01 and 901229-05, CBM DOS V2.6, as VICE's default 1541.

The drive runs at 1 MHz against the host's clock (`setClockHz`, the C64's 985,248 Hz on PAL). Each of the drive CPU's accesses brings the VIAs and the head to its cycle first, through the 6502's access sync. The CPU's dummy reads of RAM go on the bus too (the open bus gets them).

**Timing between the two machines.** Fast loaders move two bits every 8 cycles with a margin of about a cycle, so the serial bus is timed exactly:
- Each side's line changes are stamped in a common time unit (a host cycle is 1,000,000 units, a drive cycle `hostHz`), and the other side sees them from that time.
- The drive runs a few cycles behind the host (`tick()`), and catches up to the host's time when the host reads or writes the port.
- The drive's VIA samples the port at the start of its cycle.

## The serial bus (VIA1)

| VIA1 port B | |
|---|---|
| PB0 | DATA in (1 = low) |
| PB1 | DATA out (1 pulls low, through a 7406) |
| PB2 | CLK in |
| PB3 | CLK out |
| PB4 | ATN acknowledge (ATNA) |
| PB5-6 | device number jumpers (0 = unit 8) |
| PB7 | ATN in (also CA1, the DOS's ATN interrupt) |

DATA is also pulled low while ATN and ATNA differ. This XOR gate answers an ATN before the DOS gets to it. A bit the DDR leaves as an input floats high and pulls its line, as a 1 does.

The computer's side is an `IPortDevice` for its CIA port, as `IecDrive`'s: bits 3-5 drive ATN, CLK and DATA, and bits 6-7 read CLK and DATA.

## The disk (VIA2)

| VIA2 | |
|---|---|
| PB0-1 | stepper phase: a step either way moves the head half a track (half-tracks 0-83) |
| PB2 | motor |
| PB3 | LED |
| PB4 | write protect (0 = protected: the image file isn't writable) |
| PB5-6 | speed zone: a byte every 32, 30, 28 or 26 cycles (zone 0-3) |
| PB7 | SYNC (0 = under the head) |
| PA | the byte read, or the byte to write |
| CA1 | byte ready |
| CA2 | byte ready to the CPU's SO pin (V flag) |
| CB2 | 1 = read, 0 = write |

The head moves on one byte each byte time. Two `$FF` bytes in a row are a SYNC, where no byte is ready. Otherwise every byte is ready: CA1 pulses, and if CA2 enables it the CPU's V flag is set, which the DOS's `BVC *` loops wait for. In write mode the byte on PA goes onto the track.

**Images:**
- **D64** (35 to 42 tracks, with or without the error bytes) is encoded as the DOS formats it: for each sector a SYNC, the header (`$08`, checksum, sector, track, ID2, ID1, `$0F`, `$0F`), a gap, a SYNC and the data block (`$07`, 256 bytes, checksum), with the gaps filling each track. The error bytes make the sector break as the code says (20 no header, 21 no SYNC, 22 no data block, 23 data checksum, 27 header checksum, 29 ID mismatch).
- **G64** tracks are taken as stored, half-tracks included.

Writes change the GCR tracks. A D64's track is decoded back into its sectors, and written into the file (`diskfile::patch`, so gzip and zip images too) when the head leaves the track or the disk is ejected.

## Not yet

- The head reads whole bytes, not bits. A SYNC is two `$FF` bytes, and a G64 track's length isn't a bit count. So the rotation speed (`drive/rpm`), track-to-track skew beyond whole bytes, and weak or odd bits (`drive/scanner`, `drive/inertia`) aren't right.
- The VIAs' shift register doesn't shift (its modes set T2's 8-bit mode). Their timers are VICE's (README-VIA, `drive/viavarious` all pass).
- The 1541-II, 1571 and 1581, a second drive, parallel cables, and a GUI choice between this and `IecDrive`.

## Tests

- `gcr_track_round_trip`: encoding and decoding GCR tracks.
- `via_t1_latch_reads_back`: the DOS loads T1 from its latch.
- `c64_true_drive_kernal_load`: a KERNAL `LOAD` from a D64 through the drive's DOS.
- `via_t1_counts_through_ffff_and_reloads`, `via_t2_eight_bit_mode`: the VIA timers.

VICE testprogs that pass:
- all of `drive/viavarious` (19);
- `drive/1541-testsuite` (both disks), `defaults`, `diskid`, `format`, `iecdelay`;
- `drive/interrupts/timera`, `openbus`, `openbus2`, `rpm1`, `rpm2`, `selftest`, `skew1`;
- `drive/scanner` 35, 40 and 42;
- `vdrive/bustakeover`, `vdrive/sa-check`;
- `general/fuxxortest/ef2-inst1`.

Not yet:
- `drive/rpm/rpm3` (times out);
- the error-map scanners: a data block whose ID isn't `$07` gives error 27 instead of 22;
- `general/fuxxortest/test-fuxxored`;
- `drive/inertia`, which VICE fails too: it needs the head's inertia.

The VICE runner (README-C64 §2.15) uses the true drive for the list's `drive/`, `vdrive/` and `general/fuxxortest` programs. With `MMEMU_VICE_TRUEDRIVE=1` it uses it for every program.
