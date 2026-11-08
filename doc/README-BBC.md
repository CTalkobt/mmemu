# mmemu-plugin-bbc: BBC Micro

The Acorn BBC Micro (#13), after b-em (GPL-2.0-or-later):
- the memory map and the 1 MHz bus (`6502.c`);
- the video: the 6845, the video ULA and the SAA5050 (`video.c`);
- the VIAs (`via.c`, `sysvia.c`);
- the keyboard matrix (`keyboard.c`);
- the SN76489 register handling (`sn76489.c`).

## Profiles

| Profile | Machine |
|---------|---------|
| `bbcb` | BBC Micro Model B: OS 1.20 and BASIC 2, starting at BASIC's `>` prompt in Mode 7 |

The CPU is an NMOS 6502 at 2 MHz with 32 KB of RAM.

ROMs, in `roms/bbc/`, are from b-em's `roms/`. b-em distributes them in good faith; they are not under its GPL.
- `os12.rom`: OS 1.20, 16 KB at `&C000`.
- `basic2.rom`: BASIC 2, sideways slot 15.
- `saa5050.fnt`: b-em's teletext font, a capture of the SAA5050's output, character rounding included (from BeebEm).

## Memory map

| Address | Contents |
|---------|----------|
| `&0000-&7FFF` | RAM; the screen is at the top, from `&3000` (Modes 0-2) to `&7C00` (Mode 7) |
| `&8000-&BFFF` | Sideways ROM: the slot chosen by ROMSEL (`&FE30`); empty slots read 0 |
| `&C000-&FBFF` | OS ROM |
| `&FC00-&FDFF` | FRED, JIM (1 MHz bus): nothing fitted, read `&FF` |
| `&FE00-&FE07` | 6845 CRTC (address, data) |
| `&FE08-&FE0F` | 6850 ACIA: nothing attached (status: transmit empty; it interrupts only while its transmit interrupt is on) |
| `&FE20-&FE2F` | Video ULA: control (`&FE20`), palette (`&FE21`); write only |
| `&FE30-&FE3F` | ROMSEL (write only) |
| `&FE40-&FE5F` | System VIA |
| `&FE60-&FE7F` | User VIA |
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

BREAK resets only the 6502. The OS tells a power-on from a BREAK by the System VIA's IER, which a BREAK leaves alone, so a program survives BREAK and `OLD` brings it back.

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

## Tests

`src/plugins/machines/bbc/test/test_bbc_machine.cpp`:
- `bbc_boots_basic`: the banner, BASIC's `>`, and `PRINT 6*7` giving 42.
- `bbc_break_keeps_program`: BREAK, then `OLD`, then `LIST`.
- `bbc_mode1_graphics`: a Mode 1 filled rectangle in the picture.
- `bbc_typed_text_case`: typed text keeps its case.
- `bbc_sound_tone`: `SOUND` gives 449.6 Hz on the audio output.

## Not yet

- **Storage:** cassette (the ACIA and serial ULA), and disc (the 8271 or 1770 with DFS).
- **Other hardware:** the ADC (analogue joysticks), the Tube, Econet.
- **Other models:** the Model A, the B+, the Master, and the Electron.
