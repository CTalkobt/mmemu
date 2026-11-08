# Acorn Electron

The Acorn Electron (#13), in the BBC plugin (`mmemu-plugin-bbc.so`, profile `electron`), after Elkulator (GPL-2.0-or-later):
- `ula.c`: the ULA's video, interrupts, sound, cassette and RAM contention;
- `mem.c`: the memory map, paged ROM and the keyboard.

It starts at BASIC's `>` prompt in Mode 6. After "Acorn Electron" the OS draws Acorn's logo, a small acorn from a glyph in its ROM at `&C42B`.

ROMs (from the Elkulator 1.0 release, in `roms/electron/`):
- `os100.rom`: OS 1.00, 16 KB at `&C000`.
- `basic2.rom`: BASIC 2, the same as the BBC Micro's.

## Memory map

| Address | Contents |
|---------|----------|
| `&0000-&7FFF` | 32 KB of RAM, shared with the ULA |
| `&8000-&BFFF` | Paged ROM: BASIC (pages 10 and 11), the keyboard (pages 8 and 9), external ROMs (none fitted) |
| `&C000-&FBFF` | OS ROM |
| `&FC00-&FDFF` | FRED, JIM: nothing fitted |
| `&FE00-&FEFF` | The ULA (16 registers, repeated) |
| `&FF00-&FFFF` | OS ROM |

**RAM contention.** The CPU runs at 2 MHz from ROM, but RAM is 4 bits wide and the ULA shares it (Elkulator `waitforramsync`):
- in Modes 0-3 the CPU waits out each visible line;
- otherwise RAM accesses take the next 1 MHz cycle.

## The ULA

| Register | Write | Read |
|----------|-------|------|
| `&FE00` | Interrupt enable | Interrupt status: master (bit 0), power-on reset (1, cleared by reading), display end (2), RTC (3), receive full (4), transmit empty (5), high tone (6) |
| `&FE02-&FE03` | Screen start address (in 64-byte units) | |
| `&FE04` | Cassette data out (clears transmit empty) | Cassette data in (clears receive full) |
| `&FE05` | ROM page; clears the display-end (bit 4), RTC (bit 5) and high-tone (bit 6) interrupts | |
| `&FE06` | Sound counter | |
| `&FE07` | Sound/cassette mode (bits 1-2), screen mode (3-5), cassette motor (6), Caps Lock LED (7) | |
| `&FE08-&FE0F` | The palette: each logical colour's red, green and blue, as bits that are clear when on | |

**ROM paging.** Pages 12-15 select external ROMs, and pages 8-11 internal ones. Pages 0-7 take effect only when an external page is already selected, the ULA's quirk. At power-on the keyboard is paged in.

**Video.**
- 128 cycles a line and 312 or 313 lines a frame.
- 640 x 256 pictures, drawn line-doubled to 640 x 512.
- Modes 0-6: Modes 0-3 fetch a byte each cycle; Modes 4-6 fetch every other cycle and draw at half width. Modes 3 and 6 leave two blank lines in each ten-line row.
- The display-end interrupt comes at line 256 (250 in Modes 3 and 6), and the RTC interrupt at line 99.

**Sound.** A square wave from a 1 MHz / 16 counter, flipping every `S`+1 counts, where `S` is the value in `&FE06`. It sounds when `&FE07` selects sound, and is mixed to 44.1 kHz. `SOUND 1,-15,89,…` plays about 453 Hz.

**Cassette.** The same UEF tapes as the BBC Micro (gzipped or not; CLI `tape mount`/`play`/`stop`/`rewind`/`record`/`save`, the GUI tape pane). The tape runs while the motor bit is on:
- carrier keeps raising the high-tone interrupt;
- each data byte lands in the data register with the receive-full interrupt;
- in output mode, a byte written to `&FE04` is recorded, and the transmit-empty interrupt comes back a byte time (10 bits at 1200 baud) later.

I worked at the byte level rather than Elkulator's bit-level shift register, and left out its shortened carrier tones.

## Keyboard

The 14 columns are on address lines A0-A13, selected low, and the 4 rows on data bits 0-3. With the keyboard paged in, a read of `&8000-&BFFF` returns the rows of every selected column.

Named keys go to the key in the same place:

| Name | Electron key |
|------|--------------|
| `escape` / `run_stop` | ESCAPE |
| `caps_lock`, `alt`, `tab` | CAPS LK/FUNC |
| `home` / `end` | COPY |
| `delete` | DELETE |
| `'` key | `:` |
| `f1`-`f9`, `f10` | FUNC with 1-9 and 0 |
| `break` / `f12` | BREAK (resets only the 6502) |

**Typed text** (CLI `type`, MCP) keeps its case. On the Electron, CAPS LK alone doesn't toggle Caps Lock; SHIFT+CAPS LK does. So for lower case, typing presses SHIFT+CAPS LK when the Caps Lock LED is on.

## Tests

`src/plugins/machines/bbc/test/test_electron.cpp`:
- `electron_boots_basic`: the picture shows the banner, and BASIC's PAGE is `&E00`.
- `electron_typed_basic`: mixed-case text and a calculation.
- `electron_mode2_graphics`: a filled Mode 2 rectangle.
- `electron_sound_tone`: `SOUND` pitch on the audio output.
- `electron_tape_save_and_chain`: `SAVE` to a recording, then `CHAIN ""` on a fresh machine.

## Not yet

- **Expansions:** the Plus 1 (cartridges, ADC, printer) and the Plus 3 (disc).
- **Video:** the enhanced ULA modes.
