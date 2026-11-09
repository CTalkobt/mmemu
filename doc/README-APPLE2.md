# mmemu-plugin-apple2: Apple II, II+, IIe, IIc and IIgs

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
| `apple2c` | Apple //c: the enhanced IIe in one box, with its IOU, two serial ports and a built-in drive (ROM FF; the ROM is not included, see below) |

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

## IIc

`apple2c` follows MAME's `apple2e.cpp`: an enhanced IIe (65C02, 128 KB, 80 columns, MouseText) with the IIc's differences.
- **The internal ROM:** it always answers at `$C100-$CFFF`, holding the firmware of the built-in serial ports (slots 1 and 2), the 80-column display (slot 3), the mouse (slot 4) and the drive (slot 6). INTCXROM and SLOTC3ROM read back, but switch nothing.
- **The IOU's interrupts:**
  - IOUDIS is set by writing `$C078/A/C/E` and cleared by writing `$C079/B/D/F`. Reading `$C078/A/C/E` returns it in bit 7.
  - With IOUDIS clear, `$C058-$C05F` are the mouse X/Y and VBL interrupt switches: DisXY, EnbXY, DisVBL, EnVBL, and the X0/Y0 edges. With it set, they are the IIe's annunciators and double hi-res.
  - At vertical blanking with VBL enabled, VBLINT (`$C019` bit 7) rises and /IRQ is held. Reading `$C019` doesn't clear it; any `$C07x` access does.
  - `$C040-$C043` read the XY mask, the VBL mask and the X0/Y0 edges. `$C015`/`$C017` read and clear the mouse interrupt flags, and `$C048` clears them too.
- **The serial ports:** two 6551 ACIAs, port 1 at `$C098-$C09B` and port 2 at `$C0A8-$C0AB`. Their interrupts go to /IRQ. They're the devices "ACIA (port 1)" and "ACIA (port 2)", and `serial attach` connects them.
- **The drive:** the IIc's internal 5.25" drive is the Disk II-compatible slot 6, drive 1; a second drive is unit 2. With no disk the firmware shows "Check Disk Drive.". DOS 3.3 and ProDOS boot.
- **Other switches:**
  - `$C060` reads the 40/80 switch as 40 columns.
  - `$C063` reads the mouse button, up, inverted.
  - `$C02x` swaps a 32 KB ROM's halves.

**The ROM.** Apple's IIc ROMs aren't in this repository; `.gitignore` keeps them out. Download ROM FF, "Apple IIc ROM FF - 342-0272-A.bin" (16 KB), from the Apple II Documentation Project mirror:
<https://mirrors.apple2.org.za/Apple%20II%20Documentation%20Project/Computers/Apple%20II/Apple%20IIc/ROM%20Images/>. Save it as `roms/apple2/apple2c-ff.rom`. The video ROM is the enhanced IIe's, which is included.

**Not yet:**
- **The later 32 KB ROMs** (00, 03, 04): ROM 00 needs the IWM's 3.5" drive (UniDisk / SmartPort), and 03/04 need the memory expansion (`$C0C0-$C0C3`). A 32 KB ROM is refused for now.
- **The mouse:** its movement and button; the switches are there.

## IIgs

`apple2gs` follows KEGS by Kent Dickey (GPL-3.0, in the GSplus tree): `moremem.c`, `adb.c`, `clock.c`, `doc.c` and `sim65816.c`. It's a ROM 01 machine with the WDC 65C816 (`doc/README-65816.md`). Stage 1 boots to the start-up screen and to Applesoft; stage 2 adds Super Hi-Res.

- **Speed:** the CPU runs at 2.8 MHz (CYAREG `$C036` bit 7) or 1 MHz. The Mega II side (banks `$E0`/`$E1`, `$C0xx` and shadowed writes) always takes 1 MHz cycles. Time is counted in 14.318 MHz ticks: 5 per fast cycle, 14 per slow one. The video, speaker and devices run on the 1 MHz clock, and the machine is paced by it.
- **Memory:**
  - 4 MB of fast RAM in banks `$00-$3F`, the Mega II's 128 KB in `$E0`/`$E1`, and the ROM in `$FE`/`$FF`.
  - Banks `$00`/`$01` and `$E0`/`$E1` take the Apple II paging. STATEREG (`$C068`) reads and writes ALTZP, PAGE2, RAMRD, RAMWRT, RDROM, LCBANK2, ROMBANK and INTCXROM together, alongside the IIe's switches and the language card.
  - `$C02D` picks internal ROM or a slot's card per slot (no cards are fitted yet), and `$C3xx` switches the `$C800` ROM in.
- **Shadowing (`$C035`):** writes to the text pages, the hi-res pages and (in bank `$01`) Super Hi-Res also go to `$E0`/`$E1`. Bit 6 turns the I/O space and language card of banks `$00`/`$01` into RAM. CYAREG bit 4 shadows the odd banks too.
- **Vectors:** interrupt and reset vectors come from the ROM (the 65C816's /VP), whatever the language card maps. `$C071-$C07F` read the ROM.
- **Interrupts:**
  - the VBL interrupt (`$C041` bit 3, `$C046`, cleared by `$C047`);
  - the quarter-second interrupt (`$C041` bit 4);
  - the one-second interrupt (`$C023`, `$C032`);
  - ADB data.
  - `$C019` bit 7 is set during VBL, the opposite of the IIe. `$C02E`/`$C02F` are the video counters.
- **ADB:** the microcontroller is emulated at command level, as in KEGS.
  - `$C026` takes the modes, configuration, sync, memory read/write, version 5, and talk/listen to the keyboard. `$C027` is the status and interrupt enables.
  - The keyboard is autopolled through `$C000`/`$C010`, with auto-repeat. `$C025` holds the modifiers: Shift, Control, Caps Lock, Option (Right Alt) and Command (Alt).
  - Control-Command-Escape raises the Desk Manager interrupt.
- **The mouse** (ADB, after KEGS `mouse_read_c024`): moves and button changes queue up, and a button change starts a new entry so a quick click isn't lost.
  - **`$C024`:** gives the X delta, then the Y delta. Each is 7 bits signed, at most 63 a read. Bit 7 of the Y read is the button (0 = down).
  - **`$C027`:** bit 7 means mouse data is waiting, and bit 1 means Y is next. Bit 6 enables the mouse interrupt.
  - **When it's off:** ADB mode bit 1, or the mouse moved off its address.
  - **Scaling:** the front ends' moves arrive in picture pixels. Y is halved (Super Hi-Res lines are doubled), and X is halved in 320 mode, as KEGS scales the host pointer.
  - In the GUI, move and click over the screen; in the CLI, `mouse <dx> <dy> [buttons]`. GS/OS's Finder opens its menus with it (`apple2gs_boots_gsos`).
- **The clock:** `$C033`/`$C034` give the time (local, seconds since 1904) and 256 bytes of battery RAM. The battery RAM starts at zero, so the ROM falls back to its defaults; it isn't saved yet. `$C034` bits 0-3 are the border colour.
- **Sound:** the Ensoniq ES5503 DOC, after MAME `es5503.cpp` (v2.4), behind the sound GLU, mixed with the speaker (the device "Sound").
  - **Oscillators:** 32 of them, reading 8-bit samples from 64 KB of sound RAM. Each has a frequency, a volume, a wave table pointer and size (256 bytes to 32 KB) and a resolution. The modes are free-run, one-shot, sync/AM and swap.
  - **Halting and interrupts:** a zero sample halts an oscillator, and a halt can raise its interrupt. Register `$E0` names the interrupting oscillator and clears it.
  - **The highest oscillator:** the highest enabled one sounds three times over, as the silicon does.
  - **Timing:** the DOC runs at 7.16 MHz, producing a sample every 8 x (oscillators + 2) clocks: 26.3 kHz with all 32. It is caught up before each register access and after each instruction.
  - **The GLU:** `$C03C` is control (bit 6 RAM or registers, bit 5 auto-increment, bits 0-3 the volume), `$C03D` is data (reads one access behind), `$C03E/F` the address.
  - **Output:** mono at 44.1 kHz; each output sample averages the DOC samples within it. It is scaled by the GLU volume and mixed with the speaker.
  - **Heard:** the ROM's start-up beep and Block Out's title sound.
- **Other devices:**
- **The serial ports:** a Zilog 8530 SCC (the device "SCC"), after KEGS `scc.c`. Channel A is port 1, the printer port (`$C039` command, `$C03B` data); channel B is port 2, the modem port (`$C038`, `$C03A`).
  - **Registers:** a pointer selects them. WR0's commands, WR9's resets and master interrupt enable, RR0 (Rx available, Tx empty, zero count, DCD, CTS), RR2B's status-modified vector and RR3's pending interrupts are emulated.
  - **Timing:** the baud rate generator runs from the 3.6864 MHz crystal or PCLK (WR12/13/14), with the clock mode, multiplier and character format (WR4/5/11). Together they give each character's time.
  - **Interrupts:** Rx (a 3-deep FIFO), Tx empty and zero count.
  - **Host link:** `serial loopback` or `serial tcp:PORT` (a listener on 127.0.0.1) attaches port 1, then port 2; `serial none` detaches both.
  - **Firmware:** the ROM's firmware drives it: from Applesoft, `PR#1` and `PRINT` send to port 1.
  - The IWM (`$C0E0-$C0EF`) has its switches, mode, status and handshake registers. `$C031` bit 6 picks the 5.25" or the 3.5" drives. With no startup device the ROM shows "Check startup device!".
- **5.25" drives** (slot 6, the device "Disk II"): the IIgs ROM's slot 6 firmware drives them through the IWM, using the Apple II's Disk II emulation (`.dsk`, `.do`, `.po`, `.nib`, `.woz`).
  - The IWM's mode and status registers sit in front.
  - While the motor runs with CYAREG bit 2 set, the CPU drops to 1 MHz, as the IIgs does.
  - DOS 3.3 and ProDOS boot.
- **3.5" drives** (slot 5, the device `3.5" Drive`, units 1 and 2), after KEGS `iwm.c`:
  - **Images:** 800 KB ProDOS-order images (`.po`, `.hdv`), `.2mg`, DiskCopy 4.2, and 3.5" WOZ2 (INFO disk type 2, with TMAP indexed by track × 2 + side).
  - **Track format:** 80 tracks a side, with 12, 11, 10, 9 or 8 sectors by zone and 2:1 interleave. Tracks are nibblised as the ROM writes them (a port of its code at `$FF:6320`), with 10-bit syncs.
  - **Bit streams:** each track side is a bit stream, either generated or the WOZ's own. The IWM latches a nibble when its shift register's top bit is set, so 10-bit syncs fall into step. Bits pass at 2 us each, and each nibble is read once (latch mode).
  - **Weak bits:** after three zero bits, the read amplifier passes noise: such a bit reads as one about 30% of the time (KEGS `iwm_read_data`). Tracks with such runs read differently on each revolution, which the copy protection on originals such as Qix checks.
  - **Drive registers:** CA0-CA2 (phases 0-2) with SEL (`$C031` bit 7) pick a drive register. The status register reads it in bit 7 (disk in place, write protect, track 0, motor, sides, tachometer, disk switched). Phase 3 rising (LSTRB) runs the commands (step direction, step, motor on/off, eject).
  - **Writing:** each byte written puts 8 bits at the head, keeping the track's length, and the handshake register paces them (ready after 8 bit times, underrun after 16). When write mode ends, the track goes back into the image: decoded into its sectors and patched into the file, or as bits into the WOZ.
  - The ROM's SmartPort firmware boots and reads and writes these disks.
  - GS/OS System 6.0.1 (Apple's freely distributed System Disk, an 800 KB `.2mg`) boots to the Finder. The test `apple2gs_boots_gsos` boots it from `../refs/apple2gs/sys601-2.2mg` when that file is present.
  - `disk mount` tries the 3.5" drives first, then the 5.25" drives, then the SmartPort card. The 3.5" drives take only 800 KB images and 3.5" WOZs; the 5.25" drives take 140 KB images and 5.25" WOZs; everything else goes to the SmartPort card.
  - Copy-protected originals from the woz-a-day collection boot: Qix (its key disk check) and Block Out. The test `apple2gs_boots_woz_originals` boots them from `../refs/apple2gs/woz` when they are present.
- **The SmartPort card** (slot 7, the device "SmartPort", units 1-4), after KEGS (`config.c` `g_rom_c700`, `smartport.c`): images of any size, for hard disks and 1.44 MB disks.
  - **Why a virtual card:** a real IIgs would need a SCSI card or the SuperDrive card for these. Like KEGS, this card serves them through the firmware's block interface instead.
  - **Images:** raw ProDOS-order files (`.po`, `.hdv`, `.img`) and `.2mg` (its data offset and length). Writes go straight back to the file.
  - **The ROM:** the card's ROM has the SmartPort signature, the ProDOS entry at `$C70A` and the SmartPort entry at `$C70D`. Both end at `$C70F`, where an intercept serves the call. The flags say which call it is: V set for boot, C clear for ProDOS, C set for SmartPort.
  - **Boot:** block 0 is loaded to `$0800` and entered at `$0801` with X = `$70`. With no disk, slot 5 boots instead.
  - **ProDOS calls:** status, read, write, format.
  - **SmartPort calls,** standard and extended: Status (the driver, a unit, the DIB), Read Block, Write Block, Format and Control.
  - **When it answers:** the card's ROM appears at `$C700` when `$C02D` picks slot 7's card (the Control Panel's default, "Your Card") and a disk is mounted.
  - GS/OS System 6 boots from a 5 MB hard disk image to the Finder (`apple2gs_boots_hard_disk`).
- **Video:** the Apple II modes, through the IIe video, with the enhanced IIe's character set (MouseText). Text uses `$C022`'s colours, and the picture has the `$C034` border.
- **Super Hi-Res** (stage 2, KEGS `video.c`): `$C029` bit 7 turns it on. Bank `$E1` holds it:
  - **Layout:** 200 lines of 160 bytes at `$2000`. The scan-line control bytes are at `$9D00`: bit 7 selects 640 pixels, bit 6 the interrupt, bit 5 fill mode, and bits 0-3 the palette. The 16 palettes of 16 colours are at `$9E00` (`$0RGB`, low byte first).
  - **320 mode:** each byte holds two pixels. In fill mode, colour 0 repeats the pixel before it.
  - **640 mode:** each byte holds four 2-bit pixels, taking colours 8-11, 12-15, 0-3 and 4-7 of the palette in turn.
  - **Drawing:** each line is drawn when the beam reaches it, so palette and SCB changes between lines show. Lines are doubled to 640 x 400 inside the border.
  - **The scan-line interrupt:** as a line starts, if its SCB has bit 6 and `$C023` bit 5 is clear, `$C023` gets bits 7 and 5, and /IRQ is raised when `$C023` bit 1 enables it. Writing `$C032` with bit 5 clear clears it.
- **Keys:**
  - The screen buttons send Control-Reset (BASIC) and Control-Command-Reset (a cold start).
  - Control-Reset clears STATEREG, the shadow register, the slot ROM selection, the interrupt enables and the video switches, then resets the CPU (KEGS `do_reset`).

**ROM 03** (profile `apple2gs-rom03`): 256 KB in banks `$FC-$FF`. KEGS lists what differs from ROM 01:
- **ADB:** version 6, an 8-byte sync, commands `$12`/`$13`, the inverted repeat rate, and the special-keys byte at ADB RAM `$0C`.
- **`$C036`:** bit 6 is a power-on flag.
- **SmartPort bus:** ROM 03 polls the disk port for SmartPort devices (phases 1 and 3, "ENABLE2"). With none fitted, the IWM answers as KEGS's does: sense 1, data `$FF`, and the handshake `$C0` three times, then `$80`.

GS/OS 6.0.1 boots on it to the Finder (`apple2gs_rom03_boots`).

**The battery RAM** (the Control Panel's settings) is kept between runs in `~/.local/share/mmsim/apple2gs-bram-rom01.bin` (or `-rom03.bin`), or under `$MMEMU_DATA_DIR`. The test binary points that variable at a scratch folder.

**The ROMs.** Apple's IIgs ROMs aren't in this repository; `.gitignore` keeps them out. Download ROM 01 (128 KB) from the Apple II Documentation Project mirror:
<https://mirrors.apple2.org.za/Apple%20II%20Documentation%20Project/Computers/Apple%20II/Apple%20IIGS/ROM%20Images/>. Save it as `roms/apple2/apple2gs-rom01.rom`. Bank `$FE` comes first; a dump with bank `$FF` first is put in order when it loads. For ROM 03, download "Apple IIGS ROM 3 Banks FC-FD - 341-0728.bin" and "... Banks FF-FE - 341-0748.bin" from the same folder, then join them as banks `$FC-$FF`: `cat 341-0728.bin <(tail -c 65536 341-0748.bin) <(head -c 65536 341-0748.bin) > roms/apple2/apple2gs-rom03.rom` (MAME loads them this way). The tests skip without them.

**Not yet:** SmartPort devices on the disk port (the UniDisk 3.5), and ROM 00.

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
- The IIc's 32 KB ROMs (the 3.5" drive, the memory expansion) and mouse; the
  IIgs's later stages (see above); RamWorks memory; the IIe without its 80-column card.
- Cassette input and output.
