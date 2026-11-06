# mmemu-plugin-atari2600: Atari 2600 (VCS)

The Atari 2600 (#11), after Stella (GPL-2.0-or-later): the TIA's video,
sound and inputs (`src/emucore/tia/`), the RIOT 6532 (`M6532.cxx`), the
cartridge bank switching and its detection (`Cart*.cxx`,
`CartDetector.cxx`) and the console switches (`Switches.cxx`).

## Profiles

| Profile | Machine |
|---------|---------|
| `atari2600` | NTSC: colour clock 3579545 Hz, CPU 1193182 Hz, 262 lines |
| `atari2600-pal` | PAL: colour clock 3546894 Hz, CPU 1182298 Hz, 312 lines |

There is no ROM in the console: plug a cartridge in (`cart game.a26` in the
CLI, File > Attach Cartridge in the GUI, MCP `attach_cartridge`). Without
one the CPU runs into nothing.

## Hardware

- **CPU**: MOS 6507, a 6502 with 13 address lines (no interrupts), at a
  third of the colour clock.
- **Memory map** (the 6507's 8 KB, mirrored through 64 KB):

| Address | Chip |
|---------|------|
| `A12 = 1` (`$1000-$1FFF`, `$F000-$FFFF`...) | the cartridge |
| `A12 = 0, A7 = 0` (`$00-$7F`) | the TIA: writes `$00-$3F`, reads `$00-$0F` |
| `A12 = 0, A7 = 1, A9 = 0` (`$80-$FF`) | the RIOT's 128 bytes of RAM |
| `A12 = 0, A7 = 1, A9 = 1` (`$280-$29F`) | the RIOT's ports and timer |

- **TIA** (`Tia`): see below.
- **RIOT 6532** (`Riot6532`): 128 bytes of RAM; port A (SWCHA) the two
  joysticks, port B (SWCHB) the console switches, through their data
  direction registers; the interval timer (TIM1T, TIM8T, TIM64T, T1024T:
  counts down every interval, then every cycle with its flag set after it
  wraps; INTIM read clears the flag) and the PA7 edge flag.

### TIA

A port of Stella's TIA, clocked every colour clock (228 a line, 68 of them
horizontal blank):

- **Objects**: the playfield (normal, reflected, score and priority modes),
  the two players (with NUSIZ copies and stretch, reflection, vertical
  delay), the two missiles (lock to player), the ball (vertical delay), with
  Stella's delay queue for register writes, RESPx/RESMPx, HMOVE and its
  extended blank ("HMOVE bars"), and HMCLR.
- **Collisions**: the 15 latches (CXM0P..CXPPMM), CXCLR.
- **Inputs**: INPT4/INPT5 fire buttons with the VBLANK bit 6 latch, INPT0-3
  paddle charge and dump (VBLANK bit 7).
- **Sound**: the two AUDC/AUDF/AUDV channels (the 4- and 5-bit polynomial counters, pure tones and their
  combinations), two samples a line, resampled to 44100 Hz.
- **Video**: the NTSC, PAL and SECAM palettes; the frame follows VSYNC as in
  Stella's frame manager (a pulse counts after 2 lines; a frame ends when it
  ends, or after 50 lines of waiting with VBLANK on). The screen shows 228
  lines (NTSC) or 274 (PAL), starting 23 / 32 lines after VSYNC, 160 pixels
  doubled to 320.

Left out: Stella's debugger features (object toggles, fixed debug colours),
TIA revision quirks and random undriven pins, jitter, phosphor and colour
loss, and its automatic frame-layout detection.

### Timing

The 6502 core runs an instruction at a time, so the TIA and RIOT are
clocked lazily: before a read of either, they are brought up to the cycle
of that access; writes to them are held and applied when the instruction
ends, on its last cycle(s), where the 6502 does its writes (so `STA RESP0`,
`STA HMOVE`, `STA WSYNC` land on the right colour clock in every addressing
mode). WSYNC then holds the CPU (RDY) until the TIA's line ends. The core
does no dummy reads, so a read of the TIA or RIOT with an indexed address
can be one cycle early.

## Cartridges

`.a26` and `.bin` images; the type is detected as Stella does:

| Type | Size | Switching |
|------|------|-----------|
| 2K | 2 KB (mirrored) | none |
| 4K | 4 KB | none |
| F8 / F6 / F4 | 8 / 16 / 32 KB | an access to `$1FF8-$1FF9` / `$1FF6-$1FF9` / `$1FF4-$1FFB` picks the 4 KB bank; starts in the last |
| F8SC / F6SC / F4SC | the same + 128 bytes of RAM (Superchip) | RAM written at `$1000-$107F`, read at `$1080-$10FF` |
| E0 | 8 KB (Parker Brothers) | 1 KB slices: `$1FE0-$1FE7` / `$1FE8-$1FEF` / `$1FF0-$1FF7` pick the slice at `$1000` / `$1400` / `$1800`; `$1C00` is always the last |
| 3F | 8-512 KB (Tigervision) | a write to `$00-$3F` picks the 2 KB bank at `$1000`; `$1800` is always the last |
| FA | 12 KB (CBS RAM Plus) | `$1FF8-$1FFA` pick the bank; 256 bytes of RAM at `$1000` / `$1100` |

## Controls

- **Joysticks**: port 0 (P0, the left: SWCHA bits 7-4, fire on INPT4) and
  port 1 (SWCHA bits 3-0, INPT5).
- **Console switches** (as in Stella): F1 SELECT, F2 RESET (held while the
  key is down), F3 colour / F4 black and white, F5 / F6 the left difficulty
  A / B, F7 / F8 the right. The GUI's screen buttons: RESET, SELECT.

## Not yet emulated

- Other bank-switching types (E7, FE, UA, DPC, the ARM-based ones, the
  Supercharger).
- Paddles, keypads, driving controllers and other controllers in the
  frontends (the TIA's INPT0-3 readout is there, `Tia::setPaddle`).
- SECAM as a profile (the TIA has its palette).
