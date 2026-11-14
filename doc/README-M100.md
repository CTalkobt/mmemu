# Tandy Model 100 / 102 / 200

Issue #334. The machines themselves (`model100`, `model102`, `model200`) are
added separately; this page describes the peripheral chips they are built
from. Each is a standalone C++ class under `src/plugins/devices/`, compiled
into the machine plugin that uses it (as the CoCo plugin does with the
MC6821 and MC6847), with unit tests under its `test/` directory.

References: VirtualT (Stephen Hurd, Ken Pettit; BSD licence) for how the
Model 100/102/200 use the chips, MAME (`kyocera.cpp`, `kyocera_v.cpp` and
the device files named below), the chips' data sheets, the Bitchin100 wiki
and Brian K. White's `pdd.sh` for the TPDD protocols. The code is mmsim's
own; each file names what it follows.

## Devices

### HD44102 column driver and the Model 100 LCD (`hd44102/`)

`Hd44102`: one 50 x 32-dot column driver with 4 pages x 50 bytes of display
RAM (bit 0 the top dot of a byte). Offset 0 is the instruction register
(write) and status (read: bit 7 busy, 6 count up, 5 display off, 4 /RST
held); offset 1 is display data. Instructions: `$38`/`$39` display off/on,
`$3A`/`$3B` count down/up, `pp111110` display start page, `ppyyyyyy` set
page and column (0-49; other values ignored). Data accesses step the
column, wrapping 49 -> 0 (up) or 0 -> 49 (down). Reads come through an
output latch, so the first read after setting the address is a dummy, as
the data sheet says and VirtualT's "fresh pointer" logic produces. A busy
time per access (in the caller's units, `setBusyTime` + `tick`) is
optional; it defaults to 0 since neither VirtualT nor MAME model it.

`M100Lcd` composes the 240 x 64 panel from ten of them. The geometry
(MAME `set_screen_offsets((i % 5) * 50, (i / 5) * 32)`, VirtualT
`SetByte`):

| chips | panel rows | panel columns |
|-------|------------|---------------|
| 0-4   | 0-31       | 0-49, 50-99, 100-149, 150-199, 200-239 |
| 5-9   | 32-63      | the same      |

Chips 4 and 9 have only their columns 0-39 wired (4 x 50 + 40 = 240). The
ten active-high chip selects come from the 81C55: port A bits 0-7 select
chips 0-7, port B bits 0-1 chips 8-9 (`setSelectFromPorts`); the same
lines strobe the keyboard columns. An I/O access at `$F0-$FF` goes to every
selected chip (A0 picks instruction/status or data); reads OR the selected
chips. The HD44103 common driver only scans the rows and has no state.

### HD61830 LCD controller (`hd61830/`)

`Hd61830`: the Model 200's 240 x 128 controller. Offset 1 is the
instruction register / status (bit 7 busy), offset 0 data (the Model 200's
`$F1` / `$F0`). Instructions `$0` mode, `$1` character pitch, `$2` number
of characters, `$3` time divisions, `$4` cursor line, `$8`/`$9` display
start, `$A`/`$B` cursor address (with the low-byte carry quirk), `$C`
write, `$D` read (dummy-read latch), `$E`/`$F` clear/set bit. The display
RAM is external (the Model 200's 8 KB; `setRamSize`). `render(w, h)` draws
graphic mode (the Model 200's: 40 bytes of 6 dots, 64 + 64 rows) and
character mode with cursor, cursor blink and character blink (`frame()`
toggles the blink every 32 frames, as MAME).

The HD61830A's internal character ROM is not reproduced; no freely
licensed dump exists. `FONT_5X7` ($20-$7F) is generated from mmsim's own
MC6847 glyphs; katakana and the 5x11 characters render blank. An external
CG can be attached (`externalCg`). The Model 200 uses graphic mode only.

MAME compares the cursor line with `cursor position + 1`, which never
matches the 8th line of an 8-line cell; mmsim uses the register value as
the line, so `$4` = 7 underlines.

### 8155 / MSM81C55 RAM-I/O-timer (`i8155/`)

`I8155`: 256 bytes of RAM (`readRam`/`writeRam`) and eight I/O registers
(`readIo`/`writeIo`, address & 7): command/status, ports A, B, C, timer
low/high. Port C modes ALT1-ALT4 including the strobed handshakes
(`setStrobeA/B`, INTR/BF/INTE in the status). The 14-bit timer counts
`tick(clocks)` with its four output modes (single/continuous square wave,
single/continuous pulse), start/stop/stop-after-TC, a new count taking
effect at the next TC, and the TIMER status flag (cleared by a status read).
`tick` returns the number of TIMER OUT rising edges, so a UART clocked by
TIMER OUT can be advanced by that many clocks. Callbacks: `inPa/inPb/inPc`,
`outPa/outPb/outPc`, `onTimerOut`, `onIntrA/B`. The count readback follows
MAME (twice the count left in the half, bit 0 set in the first half).

On the Model 100 the timer runs at 2.4576 MHz: TIMER OUT is both the
speaker tone (selected by port B bit 2) and the IM6402's 16x clock, so a
count of 8 is 19200 baud.

### IM6402 UART (`im6402/`)

`Im6402`: control word (`writeControl`, bits as the Model 100 wires port
`$D0`: 0 SBS, 1 EPE, 2 PI, 3 CLS1, 4 CLS2), `writeTbr`, `readRbr`,
`dataReceivedReset` (`readData` does both, as `IN $C0` does), status flags
DR, TBRE, TRE, OE, FE, PE, the TRO pin, master reset. The transmitter and
receiver are clocked at 16x the bit rate (`tickTx`, `tickRx`, or `tick16x`
for both); a received character lands at the centre of its first stop bit.
`onDr` is the Model 100's RST 6.5. The far end is a `SerialEndpoint`
(`setEndpoint`, or `attachSerial(spec)`).

### Serial endpoints (`serial_endpoint/`)

`SerialEndpoint` is the byte-level far end of a UART, with modem lines.
`makeSerialEndpoint(spec)` understands `none`, `loopback`, `file:PATH`,
`tcp:PORT` (a listening socket on localhost, as the TRS-80's RS-232 port
offers) and any scheme registered with `registerSerialScheme`
(`Tpdd::registerSchemes()` adds `tpdd:` and `tpdd2:`).

### uPD1990AC calendar clock (`upd1990a/`)

`Upd1990a`: the Model 100/102's RTC, with pins `setC` (C0-C2), `setDataIn`,
`setClk`, `setStb`, `setCs`, `setOe` (and `setPins` for the Model 100's port
A update), outputs `dataOut()` and `tp()`/`onTp` (RST 7.5). Commands:
register hold, shift, time set, time read, TP 64/256/2048 Hz, test mode.
The 40-bit register is seconds, minutes, hours, day (BCD), weekday and
month. The `Upd4990a` variant adds the year byte, serial command mode
(C0-C2 high), TP 4096 Hz and the 1/10/30/60-second interval modes (those
are uPD4990A commands; the uPD1990AC has only three command pins).

Time runs on emulated time (`tick(cycles)` at `setInputHz`, default
2.4576 MHz) and starts from host local time plus an offset (`syncToHost`,
`setOffsetSeconds`); a time set moves the offset. Time set holds the
counter until hold/shift/read, and the next second then comes 31/32 s
later (MAME's divider analysis).

### RP5C01 calendar clock (`rp5c01/`)

The Model 200 uses a Ricoh RP5C01 at `$90-$9F` (MAME `tandy200`, VirtualT
`rp5c01_read/write`), not the uPD1990AC. `Rp5c01`: sixteen 4-bit
registers, the mode register's block select (time counter, alarm, two RAM
nibble blocks), timer and alarm enables, 12/24-hour mode, the leap-year
counter, the reset register (alarm reset, divider reset, 16 Hz / 1 Hz
off) and the /ALARM pin (`alarmPin`, `onAlarm`). The same host-time-plus-
offset scheme as the uPD1990AC.

### TPDD / TPDD2 (`tpdd/`)

`Tpdd` is a `SerialEndpoint`: attach it to the UART (`serial tpdd:DIR`,
`serial tpdd2:DIR`, or an image path). Operation mode (both drives):
directory reference (reference, first, next, and on the TPDD2 previous and
end), open (write new, append, read), close, read, write, delete, format,
drive status, condition, rename, with the drive's error codes. The TPDD2
adds bank 1 (type + `$40`), version (`$23`), system information (`$33`)
and the sector cache commands (`$30` load/commit, `$31` write, `$32`
read). The TS-DOS extensions VirtualT implements are there too:
sub-directories as `NAME.<>` entries (open to enter, `PARENT.<>` to leave,
open-for-write to create) and the "DME" request (`M1\r` then type `$08`).

Storage: operation mode works on a host directory; files appear in 6.2
form (`HELLO .BA`), longer host names as-is. Sector-level access (the
TPDD1's FDC mode: `M`, `D`, `F/G`, `A`, `B/C`, `R`, `W/X`, `S`, answered
with 8 hex characters; the TPDD2's sector cache) works on a disk image:
pdd.sh's `.pdd1`/`.pdd2` (per sector: size code, 12 ID bytes, 1280 data
bytes) or a raw 100 KB / 200 KB dump, written back on every change.

Limits: the drive's on-disk file system is not interpreted, so operation
mode on an image (rather than a directory) reports "no disk" for file
commands; both TPDD2 banks see the same directory; format leaves a host
directory alone; mechanical timing is not modelled (answers are queued
at once and paced by the UART).
