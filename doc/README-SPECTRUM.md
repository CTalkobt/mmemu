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
| `$FE` (any even port) | Keyboard half-rows (address lines A8-A15 low select, bits 0-4), EAR in (bit 6) | Border (bits 0-2), MIC (bit 3), EAR/beeper (bit 4) |
| `$1F` (A5-A7 low) | Kempston joystick: right, left, down, up, fire (bits 0-4, high when pressed) | |
| `$7FFD` (128K: A15 and A1 low) | | RAM bank at `$C000` (bits 0-2), screen in bank 5 or 7 (bit 3), ROM (bit 4), lock (bit 5) |

Bit 6 of a `$FE` read follows the last EAR output, as on an Issue 3 machine. The 128K always acts as an Issue 3.

## Sound

The beeper is port `$FE`'s EAR bit, with a little MIC, at Fuse's levels, sampled at 44.1 kHz. `BEEP 1,0` plays middle C (261.6 Hz).

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

## Tests

`bin/mmemu-test "spectrum*"`:
- `spectrum48_boots`
- `spectrum48_typed_basic` (PRINT, BORDER, POKE, CODE)
- `spectrum48_beep` (the pitch of middle C)
- `spectrum48_contention` (NOP timings across a screen line)
- `spectrum48_floating_bus`
- `spectrum128_boots_and_pages` (the menu, then 128 BASIC through the menu, then RAM banks at `$C000` through `$7FFD`)

`z80_fuse_core_tests` checks the Z80's per-cycle timing against Fuse's core tests.

## Not yet

- **Tape:** `.tap`/`.tzx` loading and saving.
- **Snapshots:** `.sna`, `.z80`.
- **Sound:** the 128K's AY-3-8912.

These follow in #242.
