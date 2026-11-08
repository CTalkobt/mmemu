# Sinclair ZX Spectrum

The ZX Spectrum (#242), in `mmemu-plugin-spectrum.so`, after Fuse (the Free Unix Spectrum Emulator, GPL-2.0-or-later) and libspectrum's timings:
- `machines/spec48.c`, `spec128.c`, `machine.c`, `spectrum.c`: memory maps, the contention pattern, the floating bus;
- `peripherals/ula.c`, `periph.c`: port `$FE` and the I/O contention;
- `display*.c`: the beam position, for the border and screen;
- `keyboard.c`, `peripherals/joystick.c`: the keyboard and Kempston.

| Profile | Model | ROMs (`roms/spectrum/`) | Boots to |
|---------|-------|------|----------|
| `spectrum48` | 48K | `48.rom` | "© 1982 Sinclair Research Ltd" |
| `spectrum128` | 128K | `128-0.rom` (the 128 editor and menu), `128-1.rom` (48 BASIC) | the menu |
| `spectrumplus2` | +2 (Amstrad) | `plus2-0.rom`, `plus2-1.rom` | the menu |

The ROMs come from Fuse's `roms/`; Amstrad allows their distribution (see `roms/spectrum/README.md`).

## Timing

The Z80 runs at 3.5 MHz (48K) or 3.5469 MHz (128K, +2). Its clock advances per bus cycle in Fuse's order (see `doc/README-Z80.md`), so the ULA sees each access at its T-state:

| | 48K | 128K, +2 |
|---|---|---|
| T-states per line | 224 | 228 |
| Lines per frame | 312 | 311 |
| T-states per frame | 69888 | 70908 |
| /INT held for | 32 T-states | 36 T-states |
| First screen pixel | T-state 14336 | T-state 14362 |

**Contended memory.**
- **What is contended:** `$4000-$7FFF` on the 48K, and the odd RAM banks (1, 3, 5, 7) on the 128K.
- **The delay:** in the 192 screen lines, a memory or internal cycle that starts in the 128 T-states the ULA fetches waits by the 6, 5, 4, 3, 2, 1, 0, 0 pattern. On the 48K the pattern starts at T-state 14335.
- **I/O:** contended as Fuse does it, by whether the port's high byte addresses contended memory and whether the port is even (the ULA's own).

**Floating bus.** A port nothing answers returns what the ULA is reading at that moment:
- bitmap, attribute, bitmap, attribute, then four T-states of `$FF`, in each 8;
- `$FF` in the borders.

## Video

The picture is 320 × 240: the 256 × 192 screen with 32 pixels of border at each side and 24 lines above and below, as Fuse shows it.
- **The beam:** the border and screen are drawn as the beam passes. A write to the screen or a border change first draws the cells the beam has already reached, so effects that change colours mid-frame show.
- **Attributes:** ink, paper, BRIGHT, and FLASH, which swaps ink and paper every 16 frames.
- **Colours:** Fuse's palette (`$C0` levels, `$FF` when BRIGHT).

## Ports

| Port | Read | Write |
|------|------|-------|
| `$FFFD` (128K) | The selected AY register | Select an AY register |
| `$BFFD` (128K) | | Write the AY register |
| `$FE` (any even port) | Keyboard half-rows (address lines A8-A15 low select, bits 0-4), EAR in (bit 6) | Border (bits 0-2), MIC (bit 3), EAR/beeper (bit 4) |
| `$1F` (A5-A7 low) | Kempston joystick: right, left, down, up, fire (bits 0-4, high when pressed) | |
| `$7FFD` (128K: A15 and A1 low) | | RAM bank at `$C000` (bits 0-2), screen in bank 5 or 7 (bit 3), ROM (bit 4), lock (bit 5) |

Bit 6 of a `$FE` read follows the last EAR output, as on an Issue 3 machine. The 128K always acts as an Issue 3.

## Sound

The beeper is port `$FE`'s EAR bit, with a little MIC, at Fuse's levels, sampled at 44.1 kHz. `BEEP 1,0` plays middle C (261.6 Hz).

## The AY-3-8912 (128K, +2)

The AY is after Fuse's `sound/ay_engine.c` and `peripherals/sound/ay.c`:
- **Clock:** the CPU clock / 2 (1.7734 MHz), stepped every 32 T-states.
- **Channels:** three tone channels; the noise generator (a 17-bit LFSR); the envelope (16 steps, the shapes in register 13); the mixer (register 7).
- **Levels:** Matthew Westcott's measured output levels. The channels are summed with the beeper.
- **Ports:** `$FFFD` (A15, A14 and A1 decoded) selects a register and reads it back; `$BFFD` writes it. Unused bits read as 0, and register 14 reads `$BF` (the serial input).
- **Snapshots:** `.z80` (version 2 and 3) snapshots restore the AY's registers.

128 BASIC silences the AY when a command ends, so a sound written by `OUT` from a direct command stops at once. Use `PLAY`, or a program that keeps running.

## Keyboard

The 40 keys are on 8 half-rows. Host keys map by position:
- **Letters, digits, Enter and Space:** as on the Spectrum.
- **Caps Shift:** Shift.
- **Symbol Shift:** Ctrl, Alt or the right Shift.
- **Caps Shift combinations:** Backspace is DELETE (Caps 0), the arrow keys are Caps 5-8, and Escape is BREAK (Caps Space). The screen button BREAK does the same.
- **Symbol Shift combinations:** host punctuation (`, . ; - / = + * '`) gives the matching Symbol Shift key.

**Typed text** (CLI `type`, `--type`, the GUI's Type Text, MCP) goes to the ROM's key buffer: `LAST_K` (23560), with bit 5 of `FLAGS` (23611) set. A character is handed over only when the ROM is about to read a key: at the `BIT 5,(IY+1)` in 48 BASIC's `KEY-INPUT`, or in the 128 editor's key loop. Each ROM's point is found by its code, so nothing is lost while BASIC is busy.
- **48 BASIC:** the keywords are entered as tokens, as K mode would (`PRINT` is `$F5`; `GO TO` and `GOTO` both give `$EC`; `<=`, `>=` and `<>` are tokens too). The spaces around keywords are dropped, since the ROM adds its own when listing. Strings and `REM` text stay as typed.
- **128 BASIC:** the editor tokenises lines itself, so text goes as typed.
- **The 128K menu:** its cursor keys are read from the keyboard rather than the key buffer, so press keys (for example `down`, then `return`).

## Joystick

The host's first joystick or gamepad is the Kempston at port `$1F`.

## Tape

`.tap` and `.tzx` files, plain, gzipped or in a zip, after Fuse's `tape.c`, `tape_traps.c` and `loader.c` and libspectrum's tape readers. They go in through CLI `tape mount|play|stop|rewind`, the GUI tape pane, or `load` (see below).
- **Pulses:** the tape plays as edges on EAR (bit 6 of port `$FE`), in TZX's 3.5 MHz T-states. On the 128K they are scaled to its clock, as Fuse does.
- **TZX blocks:**
  - Played: standard (`0x10`) and turbo (`0x11`) data, pure tone (`0x12`), pulse sequence (`0x13`), pure data (`0x14`) and direct recording (`0x15`).
  - Tape control: pause, and stop the tape (`0x20`); stop if 48K (`0x2A`); set level (`0x2B`); loops (`0x24`/`0x25`).
  - Skipped: text, archive and other information blocks; the CSW and generalized-data blocks (`0x18`, `0x19`) are skipped too.
- **`.tap`:** each block is a standard block with a 1-second pause.
- **The ROM traps:** these act when the 48 BASIC ROM is paged, which Fuse's `trap_check_rom` checks by its bytes.
  - **LD-BYTES:** at the `RET NZ` at `$056B`, a standard block at the tape's position is loaded, or verified, straight into memory. The registers and flags are left as the ROM leaves them (Fuse `tape_trap_load_block`), and it returns through `$05E2`.
  - **SA-BYTES:** at the `EX AF,AF'` at `$04D0`, the block being saved (flag, data, parity) is kept, and it returns through `$053E`. `tape save <file>.tap` writes the blocks. `tape record` starts a new set.
  - **Not trapped:** a non-standard block, or a tape already playing, is read by the ROM from the pulses.
- **Loader detection** (Fuse `loader.c`): reads of port `$FE` in a tight loop, less than 500 T-states apart with B counting by one, start the tape. Ten reads that look like something else stop it. Turbo loaders therefore load without pressing play.

## Snapshots and `load`

CLI `load`, `--mount` and MCP take these:

| File | What happens |
|------|--------------|
| `.tap`, `.tzx` | Mounted. The machine is reset and loading starts: `LOAD ""` on the 48K, the menu's Tape Loader on the 128K. |
| `.sna` | 48K (49179 bytes): the registers, RAM and border, with PC taken from the stack. 128K (131103 or 147487 bytes): also `$7FFD` and every bank. |
| `.z80` | Versions 1 (48K, RLE-compressed), 2 and 3: 48K and 128K/+2 pages, `$7FFD`, the border. The +3 and Timex models are refused. |

A 48K snapshot runs on the 128K in 48K mode: `$7FFD` = `$30` (ROM 1, locked). A 128K snapshot is refused on the 48K.

## Tests

`bin/mmemu-test "spectrum*"`:
- `spectrum48_boots`
- `spectrum48_typed_basic` (PRINT, BORDER, POKE, CODE)
- `spectrum48_beep` (the pitch of middle C)
- `spectrum48_contention` (NOP timings across a screen line)
- `spectrum48_floating_bus`
- `spectrum128_boots_and_pages` (the menu, then 128 BASIC through the menu, then RAM banks at `$C000` through `$7FFD`)
- `spectrum48_tape_trap_save_load` (SAVE, then LOAD "" through the traps)
- `spectrum48_tape_pulses_load` (the same tape read by the ROM from the pulses)
- `spectrum_tzx_blocks`
- `spectrum48_sna_snapshot`
- `spectrum_z80_snapshots` (version 1 compressed, version 3 128K)
- `spectrum128_ay_tone` (tone A at 261 Hz from 128 BASIC; a register read back)

`z80_fuse_core_tests` checks the Z80's per-cycle timing against Fuse's core tests.

## Not yet

- **Saving:** snapshots, and recording a tape's pulses from MIC. A custom saver's blocks are not kept; the ROM's SAVE goes through the trap.
