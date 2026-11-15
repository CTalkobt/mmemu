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
| PB0-1 | stepper phase: the head follows it a half track at a time (half-tracks 0-83) |
| PB2 | motor |
| PB3 | LED |
| PB4 | write protect (0 = protected: the image file isn't writable) |
| PB5-6 | speed zone (0-3) |
| PB7 | SYNC (0 = under the head) |
| PA | the byte read, or the byte to write |
| CA1 | byte ready |
| CA2 | byte ready to the CPU's SO pin (V flag) |
| CB2 | 1 = read, 0 = write |

**The head** works on bits. The disk turns at 300 rpm, once every 200,000 drive cycles, and a track's 8 × size bit cells pass under the head in that time. So a turn takes the same time on every track, and in every speed zone (VICE testprogs `drive/rpm`).
- **Reading:** each cell is a bit. Ten 1 bits in a row are a SYNC (PB7 low), and restart the byte. After a SYNC, every 8 bits are a byte: PA latches it, CA1 pulses, and if CA2 enables it the CPU's V flag is set, which the DOS's `BVC *` loops wait for.
- **Writing:** the byte on PA is shifted out into the cells, a bit per cell, and the next byte is taken (a byte ready) every 8 bits.

**The stepper** has inertia (`drive/inertia`): the head moves a half track toward the coil only after the coil's phase has held for 1,000 cycles, and each further half track takes as long again. A phase held for less, such as stepping out and back, or round all four phases, leaves the head where it was. The DOS steps every 14,000 cycles or so. When the head moves, it keeps its angle: its cell is scaled by the two tracks' sizes, as VICE's.

**Images:**
- **D64** (35 to 42 tracks, with or without the error bytes) is encoded with VICE's layout (`gcr.cpp`):
  - each sector is a SYNC, the header (`$08`, checksum, sector, track, ID2, ID1, `$0F`, `$0F`), 9 bytes of `$55`, a SYNC, the data block (`$07`, 256 bytes, checksum) and the zone's gap (8, 17, 12 or 9 bytes), and `$55` fills the rest of the track;
  - each track is turned on from the last by VICE's skew: the sectors' length, and the time a step takes (`drive/skew`);
  - the error bytes break the sector as VICE does: 2 a header ID of `$FF` (error 20), 3 no SYNCs on the track (21), 4 a data block ID of `$00` (22, but the data still reads), 5 a bad data checksum (23), 9 a bad header checksum (27), 11 ID1 changed (29);
  - tracks past the image's are unformatted (`$55`), and can be written.
- **G64** tracks are taken as stored, half-tracks included.
- **P64** (flux pulses, 3,200,000 positions a turn, range-coded) is read with BeRo's P64 reference decoder from VICE (`p64.cpp`, zlib licence). Each half-track's pulses become bit cells at the track's usual density, the cell clock restarting at each pulse, as VICE's `P64PulseStreamConvertToGCR` does. Pulse strengths and side 1 are ignored.
- Half-tracks with nothing on them have no flux (zeros).

Writes change the GCR tracks; only a D64's are saved (a G64's or P64's last until it's ejected). A D64's track is decoded back into its sectors, and written into the file (`diskfile::patch`, so gzip and zip images too) when the head leaves the track or the disk is ejected.

**The DOS's SEAK** (`$F3B1`) takes a block whose first GCR byte is `$52` for a header. A data block with the ID `$00` (error 22) starts with `$52` too. So which errors a program reads depends on where the sectors are when the head lands on a track, which is why the D64 layout and skew follow VICE's (`drive/scanner/scanner*e`).

## Not yet

- The speed zone sets neither the read clock nor the write clock. The head reads and writes the track's cells at the track's own density, so a track read in the wrong zone reads right, where a real drive would read garbage.
- The read circuit's clock recovery, and weak or random bits: unformatted parts of a disk read as no flux, not noise.
- The VIAs' shift register doesn't shift (its modes set T2's 8-bit mode). Their timers are VICE's (README-VIA, `drive/viavarious` all pass).
- The 1541-II, 1571 and 1581, a second drive, parallel cables, and a GUI choice between this and `IecDrive`.

## Tests

- `gcr_track_round_trip`: encoding and decoding GCR tracks.
- `p64_pulses_to_gcr_round_trip`: pulses at a GCR track's cell centres come back as its bits; files that aren't P64s are refused.
- `drive1541_error_map_job_codes`: a D64 with an error map; the DOS's read jobs return each error's code, and errors 22 and 23 still read the data.
- `via_t1_latch_reads_back`: the DOS loads T1 from its latch.
- `c64_true_drive_kernal_load`: a KERNAL `LOAD` from a D64 through the drive's DOS.
- `via_t1_counts_through_ffff_and_reloads`, `via_t2_eight_bit_mode`: the VIA timers.

VICE testprogs that pass: all of `drive/` and `vdrive/` in the list, and all of `general/fuxxortest`:
- `drive/viavarious` (19);
- `drive/1541-testsuite` (both disks), `defaults`, `diskid`, `format`, `iecdelay`, `inertia`;
- `drive/interrupts/timera`, `openbus`, `openbus2`, `rpm1`, `rpm2`, `rpm3`, `selftest`, `skew1`;
- `drive/scanner` 35, 40 and 42, from D64s (with and without error maps) and G64s;
- `drive/rpm` 1-3 from D64, G64 and P64, `drive/skew/skew2` (G64), `drive/hls-protection/hlstest` (G64);
- `vdrive/bustakeover`, `vdrive/sa-check`;
- `general/fuxxortest`, `test-fuxxored` included.

`drive/inertia` (Krill's drive-emu-check) is one that VICE fails ("EMU"): it steps out and back faster than the head can follow.

The VICE runner (README-C64 §2.15) uses the true drive for the list's `drive/`, `vdrive/` and `general/fuxxortest` programs. With `MMEMU_VICE_TRUEDRIVE=1` it uses it for every program. The list's G64 and P64 entries (`mountg64:`, `mountp64:`) run too: `hls-protection/hlstest`, `rpm1`-`rpm3` from both, `scanner35`/`40`/`42` and `skew2` (#364).
