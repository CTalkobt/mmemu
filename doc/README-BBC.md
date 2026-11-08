# mmemu-plugin-bbc: BBC Micro

The Acorn BBC Micro (#13), after b-em (GPL-2.0-or-later):
- the memory map and the 1 MHz bus (`6502.c`);
- the video: the 6845, the video ULA and the SAA5050 (`video.c`);
- the VIAs (`via.c`, `sysvia.c`);
- the keyboard matrix (`keyboard.c`);
- the SN76489 register handling (`sn76489.c`);
- the 8271 disc controller (`i8271.c`, with the sector-image timing of `disc.c` and `sdf-acc.c`);
- the cassette: the 6850 ACIA, the serial ULA's motor, and UEF tapes (`acia.c`, `serial.c`, `tape.c`, `uef.c`); the UEF player (`UefTape`) is shared with the Electron.

## Profiles

| Profile | Machine |
|---------|---------|
| `bbca` | BBC Micro Model A: 16 KB, OS 1.20 and BASIC 2; no User VIA, ADC or disc |
| `bbcb` | BBC Micro Model B: OS 1.20, Acorn DFS 0.90 and BASIC 2, starting at BASIC's `>` prompt in Mode 7 |
| `bbcb-6502` | A Model B with the 6502 Second Processor on the Tube; DNFS 1.00 instead of DFS 0.90 |
| `bbcb-z80` | A Model B with the Z80 Second Processor on the Tube (CP/M); DNFS 1.00 |

The CPU is an NMOS 6502 at 2 MHz with 32 KB of RAM; the Model A's 16 KB is seen at both `&0000` and `&4000`, as b-em maps it (A14 is not decoded; the screen is at the top).

ROMs, in `roms/bbc/`, are from b-em's `roms/`. b-em distributes them in good faith; they are not under its GPL.
- `os12.rom`: OS 1.20, 16 KB at `&C000`.
- `basic2.rom`: BASIC 2, sideways slot 15.
- `dfs09.rom`: Acorn DFS 0.90 for the 8271, sideways slot 14. It is 8 KB and appears twice in its 16 KB slot. DFS raises PAGE to `&1900`.
- `dnfs100.rom`: DNFS 1.00 (DFS 1.00 and NFS 3.40; b-em's `usdnfs.rom`), slot 14 on `bbcb-6502`. Unlike DFS 0.90, it has the Tube host code.
- `6502tube.rom`: the 6502 Second Processor's 2 KB boot ROM (b-em `roms/tube`).
- `z80tube121.rom`: the Z80 Second Processor's 4 KB client ROM, version 1.21 (b-em `roms/tube/Z80_121.rom`).
- `saa5050.fnt`: b-em's teletext font, a capture of the SAA5050's output, character rounding included (from BeebEm).

## Memory map

| Address | Contents |
|---------|----------|
| `&0000-&7FFF` | RAM; the screen is at the top, from `&3000` (Modes 0-2) to `&7C00` (Mode 7) |
| `&8000-&BFFF` | Sideways ROM: the slot chosen by ROMSEL (`&FE30`); empty slots read 0 |
| `&C000-&FBFF` | OS ROM |
| `&FC00-&FDFF` | FRED, JIM (1 MHz bus): nothing fitted, read `&FF` |
| `&FE00-&FE07` | 6845 CRTC (address, data) |
| `&FE08-&FE0F` | 6850 ACIA: the cassette (RS423 has nothing attached) |
| `&FE10-&FE17` | Serial ULA: motor relay (bit 7), RS423 or cassette (bit 6); write only (a read writes `&FE`) |
| `&FE20-&FE2F` | Video ULA: control (`&FE20`), palette (`&FE21`); write only |
| `&FE30-&FE3F` | ROMSEL (write only) |
| `&FE40-&FE5F` | System VIA |
| `&FE60-&FE7F` | User VIA |
| `&FE80-&FE9F` | 8271 disc controller (status/command, result/parameter, reset, data) |
| `&FEC0-&FEDF` | uPD7002 ADC (the analogue port) |
| `&FEE0-&FEFF` | The Tube, host side (`bbcb-6502`, `bbcb-z80`) |
| `&FF00-&FFFF` | OS ROM |

Other SHEILA addresses read `&FE`, as in b-em.

Accesses to FRED, JIM and the slow parts of SHEILA wait for the 1 MHz clock, which adds 1 or 2 cycles (b-em's `FEslowdown`). The slow parts are the CRTC and ACIA (`&FE00`), the System VIA (`&FE40`), the User VIA (`&FE60`) and the ADC (`&FEC0`). Each instruction's cycles include these waits.

## System VIA and the slow data bus

The System VIA's port A is the slow data bus, shared by the keyboard and the SN76489. Port B writes IC32, the 74LS259 addressable latch:

| IC32 bit | Function |
|----------|----------|
| 0 | Sound write enable: the SN76489 takes the bus when it goes low |
| 3 | Keyboard: autoscan when high, one key at a time when low |
| 4-5 | Screen size, for the hardware-scroll wrap (`&4000`, `&5000`, `&2000`, `&2800` bytes) |
| 6-7 | The Caps Lock and Shift Lock LEDs (on when low) |

Port B bits 4 and 5 are the joystick fire buttons, low when pressed.

## Joysticks and the ADC

The analogue port's four channels go through the uPD7002 ADC (b-em `adc.c`):
- **Channels:** joystick 1's X and Y are channels 0 and 1, and joystick 2's are 2 and 3. A channel reads high when its stick is to the left or up.
- **Conversions:** writing `&FEC0` starts converting the channel in bits 0-1. A 10-bit conversion (bit 3) takes 10 ms, otherwise 4 ms.
- **Status:** busy (bit 7), not busy (bit 6), and the result's top two bits. End of conversion is the System VIA's CB1.

The frontends' digital joysticks (ports 0 and 1) drive the channels to their ends and work the fire buttons, so `ADVAL(1)` reads 65520 with the stick left and 0 with it right. The Model A has no analogue port.

CA1 is the 6845's vertical sync and CA2 the keyboard. CB2 latches the light pen on a rising edge.

The VIAs count in 2 MHz half-cycles, as b-em's do, so timer reads, reloads and interrupts fall where they do on a BBC.

## Keyboard

The matrix is 10 columns of 8 rows. A key's code is `row << 4 | column`. Row 0 holds Shift, Ctrl and the start-up links; all links read open, which gives Mode 7. In autoscan, CA2 is high while any key in rows 1-7 is down. Otherwise the OS writes a key's code to the slow data bus and reads bit 7.

Named keys (`onKey`, the GUI, CLI `key`) go to the key in the same place on a BBC keyboard, following b-em's physical map:

| Name | BBC key |
|------|---------|
| `escape` / `run_stop` | ESCAPE |
| `f1`-`f9` | f1-f9 |
| `f10` | f0 |
| `f12` / `break` | BREAK |
| `delete` | DELETE |
| `home` / `end` | COPY |
| `caps_lock` | CAPS LOCK |
| `=` key (`kpeq`) | `^` |
| `'` key | `:` |
| `` ` `` key | `_` |

BREAK restarts the 6502, the video, the disc controller and a second processor (b-em `main_key_break`); the VIAs keep their state. The OS tells a power-on from a BREAK by the System VIA's IER, which a BREAK leaves alone, so a program survives BREAK and `OLD` brings it back.

Typed text (CLI `type`, MCP) presses one key at a time, holding it for 40 ms and releasing it for 30 ms. The OS scans every 10 ms and misses keys, including a repeated key, at 25/15 ms. Letters keep their case. With Caps Lock on, which it is at power-on, capitals need no Shift. On a BBC, Shift doesn't reverse Caps Lock unless SHIFT+CAPS LOCK has set that mode, so for lower case, typing presses CAPS LOCK first.

## Video

`BbcVideo` (`src/plugins/devices/bbc_video`) is b-em's `video_poll`, run a 2 MHz cycle at a time:
- **6845:** the registers with b-em's masks, the character row and scan-line counters, vertical adjust, the cursor (start and end lines, blink rates, the ULA's cursor width bits) and interlace-sync-and-video addressing (R8 bits 0-1 = 3).
- **Video ULA:**
  - the fast clock (Modes 0-3, a character every cycle) and the slow clock (Modes 4-7, every other cycle);
  - 1, 2 and 4 bits a pixel through the 16-entry palette, with logical to physical colours inverted as the hardware has them;
  - the flashing colours 8-15, following the control register's flash bit (the OS toggles it).
- **Bitmap addressing:** `MA << 3 | RA`, wrapping by the screen size from IC32. Modes 3 and 6 have blank lines between the text rows.
- **SAA5050 (Mode 7):**
  - the screen at `&7C00` (MA bit 13), each character shown two cells after it is fetched;
  - the colour and graphics codes, contiguous and separated graphics, hold and release, conceal;
  - double height, flash, and new and black backgrounds.

The picture is 832 x 624 and every scan line is drawn twice. In Mode 7 the two lines are the SAA5050's two fields, each with its own half of the rounded 16 x 20 character, as an interlaced TV shows them. Not done: the NuLA extensions, and interlace as separate fields.

## Sound

The SN76489 runs at 4 MHz divided by 16:
- three tones at 125 kHz / N, where N = 0 counts as 1024;
- noise from the BBC's 15-bit shift register: bits 0 and 1 fed back for white noise, bit 0 for periodic;
- the noise clock is 250 kHz / 16, 32 or 64, or tone 3's rate;
- volume in 2 dB steps;
- the output is mixed to 44.1 kHz.

A byte with bit 7 set latches a register. As in b-em, a byte without it sets the high bits of the last tone written, or the noise control if noise was latched.

OS 1.20's pitch table is slightly sharp: `SOUND 1,-15,89,…`, an A, writes N = 278, which is 449.6 Hz.

## Discs

**The 8271 controller.** It follows b-em:
- **Commands:** the ones DFS uses. Read and write, of one 128-byte sector or several sectors of any size, with or without deleted data. Verify, read ID, format, seek, specify, read drive status, and read and write special register.
- **Data transfer:** data moves by NMI, as on a BBC without DMA. The 8271 raises its interrupt for each byte, and the 6502's NMI line follows it.
- **Timing:** a byte every 272 cycles (b-em's sector-image rate). Seeks take the step and settle times DFS specifies. The motor stops the specified number of revolutions after the last command, and not-found errors come after 500 byte times.

**Drives and images.** Drives 0 and 1 are units 0 and 1 in CLI `disk mount <unit> <path>`, MCP `mount_disk` and the GUI drive pane ("Drv 0").
- **Sides:** the drive control output's side select picks the side. DFS drives 2 and 3 are the second sides of drives 0 and 1.
- **Image formats:**
  - `.ssd`: single-sided, one track after another.
  - `.dsd`: double-sided, with the two sides' tracks alternating.
  - Both have 10 sectors of 256 bytes on 40 or 80 tracks, the count taken from the size. Short images are padded.
- **Writes:** writes go back to the file when a sector completes. A read-only file mounts write-protected, and the 8271 then returns "write protected".

SHIFT+BREAK boots a disc's `!BOOT`. Checked with b-em's demo disc, whose `!BOOT` runs `*BASIC` and `CHAIN"LOAD"`; the program loads and reports that it wants a Master 128, which is right for that disc.

## Cassette

**Tapes.** UEF tapes, gzipped or not, mount with CLI `tape mount <path>` or from the GUI tape pane. Mounting presses PLAY.
- **Motion:** the tape moves while the serial ULA's motor relay is on and PLAY is down.
- **Byte time:** one byte time per step (b-em's `uef_poll`). At 1200 baud that is 120 bytes a second; the baud-rate chunk changes it.
- **Chunks:**
  - carrier tone (`&110`, and `&111` with its dummy byte) raises the ACIA's DCD, which interrupts;
  - data (`&100`, and `&104` with 7 or 8 bits) clears DCD and delivers its bytes to the receive register;
  - gaps (`&112`, `&116`) are silence;
  - other chunks are skipped.
- **The end:** the tape stops at its end.
- **Controls:** `tape stop`, `tape play` and `tape rewind`, and the pane's buttons.

**The ACIA.** It follows b-em, with one change. In cassette mode it takes a byte time to send: 10 bits at 1200 baud, or 300 when the control register divides by 64. The OS times its output by the transmit register. b-em empties the register at once, and the OS then writes each byte several times.

**Recording.** `tape record`, then `tape save <path>` (or the pane's Record and Save) records what the ACIA sends while the motor runs in cassette mode. The idle time between bytes becomes carrier tone, and the result is saved as a UEF.

**Checked:**
- `*TAPE`, `SAVE` onto a recording, then `CHAIN ""` on a fresh machine loads it and runs it.
- b-em's `Welcome_B.uef` (gzipped) loads with `*TAPE` and `CHAIN ""`, and runs through to "Welcome to the BBC Microcomputer".

## The Tube and the 6502 Second Processor

`bbcb-6502` adds the 6502 Second Processor (b-em `tube.c`, `6502tube.c`).

**The Tube ULA.** It has four FIFOs between the BBC (`&FEE0-&FEE7`) and the second processor (`&FEF8-&FEFF` on its side):
- register 1 holds 24 bytes from the second processor to the BBC;
- register 3 holds one or two bytes (flag V).

The control register sets or clears these flags:

| Flag | Effect |
|------|--------|
| Q | The BBC's IRQ from register 4 |
| I, J | The second processor's IRQ from registers 1 and 4 |
| M | Its NMI from register 3 |
| V | Two-byte register 3 |
| P | Holds the second processor in reset |
| T | Clears the FIFOs |

**The second processor** is a 65C02 at 3 MHz, running 1.5 cycles for each of the BBC's, with 64 KB of RAM. Its 2 KB boot ROM covers `&F000-&FFFF` until it first reads the Tube's status register.

**What runs where.** DNFS's Tube host code finds it and the banner reads "Acorn TUBE 6502 64K". BASIC is copied across and runs on the second processor: `PAGE` is `&800` and `HIMEM` is `&8000`. The BBC handles the screen, keyboard, sound and discs. The debugger sees the second processor as CPU `tube` on bus `tube`.

## The Z80 Second Processor

`bbcb-z80` adds the Z80 Second Processor (b-em `z80.c`).
- **The CPU:** a Z80 at 6 MHz, three cycles for each of the BBC's, with 64 KB of RAM. The debugger sees it as CPU `tube`, on bus `tube` for memory and `tube-io` for I/O.
- **The client ROM:** 4 KB, version 1.21. It covers `$0000-$0FFF` until the Z80 first runs code above `$7FFF`, and pages back in whenever an NMI is taken, since its NMI handler is at `$0066`.
- **The Tube:** on every Z80 I/O port, by the port's low three bits.
- **Interrupts:**
  - The client runs in IM 2 with I = `$FF`. The data bus reads `$FE` when an interrupt is acknowledged, so the vector is at `$FFFE`, as in b-em.
  - The Tube's IRQ and NMI go to the Z80's /INT and /NMI. The NMI moves register 3's data, one byte per NMI.

**Booting.**
- **Without a disc:** the banner reads "Acorn TUBE Z80 64K 1.21". BASIC (a 6502 language) is copied across to `$8000`, then the client asks "Insert CP/M System disc in drive A".
- **With Acorn's CP/M system disc in drive 0:** for example b-em's `discs/cpm1.dsd`, an 80-track double-sided `.dsd`. Mount it, then power on or press BREAK. The client loads CP/M through OSWORD `&7F`, and "Acorn CP/M 2.2 - Bios 1.20" and `A>` appear in an 80-column mode. `DIR` lists the disc.

The CP/M discs are not in this repository.

## Tests

`src/plugins/machines/bbc/test/test_bbc_machine.cpp`:
- `bbc_boots_basic`: the banner, BASIC's `>`, and `PRINT 6*7` giving 42.
- `bbc_break_keeps_program`: BREAK, then `OLD`, then `LIST`.
- `bbc_mode1_graphics`: a Mode 1 filled rectangle in the picture.
- `bbc_typed_text_case`: typed text keeps its case.
- `bbc_sound_tone`: `SOUND` gives 449.6 Hz on the audio output.
- `bbc_joystick_adc`: `ADVAL` reads the stick positions and the fire buttons.
- `bbc_model_a`: the 16K banner, no DFS, and `PAGE`/`HIMEM` (`&E00`, `&3C00`).
- `bbc_tube_6502`:
  - the Tube banner;
  - BASIC on the second processor (`PAGE`, `HIMEM`, a sum), with the program in its memory;
  - BREAK.
- `bbc_tube_z80_cpm`: BASIC copied to the Z80's memory. With b-em's `discs/cpm1.dsd` beside this tree, CP/M 2.2's BIOS loads from the disc.

`test_bbc_disc.cpp`:
- `bbc_dfs_image_layout`: where `.ssd` and `.dsd` sectors are in the file.
- `bbc_dfs_save_and_chain`: on a blank disc, DFS's banner, `*CAT`, `SAVE` writing the catalogue and file to the image, then `CHAIN` loading and running it.

`test_bbc_tape.cpp`:
- `bbc_uef_playback`: a plain and a gzipped UEF. Carrier raises DCD, then the data bytes arrive while the motor runs, and nothing moves while it is off.
- `bbc_tape_save_and_chain`: the cassette filing system both ways.

## Not yet

- **Storage:** CSW tapes, the RS423 port, the 1770 controller (B+, Master), ADFS, and other disc image formats (`.adf`, `.hfe`, `.fsd`).
- **Other hardware:** other second processors (Z80, 6809, ARM, 32016), Econet, the printer port.
- **Other models:** the B+ and the Master. (The Electron is in [README-ELECTRON.md](README-ELECTRON.md).)
