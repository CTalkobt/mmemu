# mmemu-plugin-ted: MOS 7360/8360 TED

The TED (Text Editing Device) is the video, sound, timer and keyboard-latch chip of the Commodore Plus/4, C16 and C116. mmemu's TED is ported from VICE (`src/plus4/ted*.c`, the palette from `ted-color.c` and `src/video/video-color.c`), GPL-2.0-or-later; see NOTICE. VICE's `raster_t` framework is not ported, only the behaviour it implements, one line at a time.

Source: `src/plugins/devices/ted/main/` (`ted.h`, `ted.cpp` registers/timers/raster, `ted_video.cpp` drawing and palette, `ted_sound.cpp`). Device type `ted` (plugin `mmemu-plugin-ted.so`, PAL). Tests: `src/plugins/devices/ted/test/test_ted.cpp`.

---

## 1. Clock

`tick(cycles)` counts TED double-clock cycles: PAL 1,773,447 Hz, NTSC 1,789,772 Hz (`Ted::clockHz(Standard)`), 114 cycles a line in both (VICE `plus4.h`), 312 lines (PAL) or 262 (NTSC).

The CPU runs at the double clock, or at the single clock (half speed) when TED needs the bus. The machine's scheduler asks before each CPU cycle:

| Call | Meaning |
|------|---------|
| `cpuSingleClock()` | Charge 2 ticks per CPU cycle now: `$FF13` bit 1 set, or cycles 4-100 of lines 0-`$CB` while the display is on (TED fetches every other cycle), or cycles 91-100 of any line (RAM refresh). As VICE's `ted_delay_clk`. |
| `cpuBlocked()` / `consumeBlockedCycle()` | TED holds the bus for (40 + 3) * 2 = 86 double-clock cycles from cycle 4 of each character-pointer (bad) line and each attribute line. While blocked, the scheduler calls `consumeBlockedCycle()` and `tick(1)` without running the CPU (as for ANTIC). |
| `totalCycles()` | Double-clock cycles since reset, for real-time pacing. |

## 2. Machine hooks

| Hook | Use |
|------|-----|
| `setFetch(FetchFn)` | `uint8_t(uint16_t addr, bool fromRom)`: the byte TED sees at `addr`. `fromRom` is true for character/bitmap fetches while `$FF12` bit 2 is set (the machine reads its current ROM banking there); for screen/attribute fetches it is true only at `$8000` and up while ROM is selected. A character/bitmap fetch from "ROM" below `$8000` is VICE's open space (0) and does not call the hook. The idle-state byte is read from `$FFFF` (RAM). |
| `setKeyboardRead(KeyboardFn)` | `uint8_t(uint8_t ff08Written)`: called when `$FF08` is written, with that value (bit 2 low selects joystick 1, bit 1 low joystick 2). Return the keyboard column bits for the rows the machine latched at `$FD30`, ANDed with the selected joysticks (0 = pressed). TED latches the result; reads of `$FF08` return it. |
| `setRomSelect(RomSelectFn)` | `void(bool romOn)`: a write to `$FF3E` (true) or `$FF3F` (false). Also called with true by `reset()`. `romSelected()` is the state (`$FF13` bit 0). |
| `setIrqLine(ISignalLine*)` | `set(true)` while TED asserts IRQ, `set(false)` when it stops (only on a change). |
| `setStandard(PAL/NTSC)` | Lines per frame, frame size, vertical sync line, palette. |

## 3. Registers ($FF00-$FF3F)

| Reg | Function | Read |
|-----|----------|------|
| $00-$05 | Timers 1-3, low/high. Writing the low byte stops a timer and loads it; writing the high byte loads and starts it. They count at the single clock (once per 2 ticks). Timer 1 reloads from its latch on underflow (0 = 65536); timers 2 and 3 run on from `$FFFF`. | current count |
| $06 | Bit 7 test, 6 ECM, 5 BMM, 4 DEN, 3 RSEL (25 rows), 2-0 Y scroll | as written |
| $07 | Bit 7 256 characters (no hardware reverse), 6 NTSC, 5 freeze, 4 MCM, 3 CSEL (40 columns), 2-0 X scroll | as written |
| $08 | Keyboard latch (see above) | the latch |
| $09 | IRQ flags: 1 raster, 2 light pen, 3 timer 1, 4 timer 2, 6 timer 3, 7 any enabled. Write 1s to clear. | flags \| `$25` |
| $0A | IRQ enables (bits 1, 3, 4, 6), bit 0 = raster compare bit 8 | \| `$A0` |
| $0B | Raster compare bits 7-0 | |
| $0C/$0D | Cursor position (10 bits) | `$0C` \| `$FC` |
| $0E/$12 | Voice 1 frequency low / bits 0-1 high | |
| $0F/$10 | Voice 2 frequency low / bits 0-1 high | `$10` \| `$7C` |
| $11 | Bit 7 DA, 6 noise (voice 2), 5 voice 2, 4 voice 1, 3-0 volume | |
| $12 | Bits 5-3 bitmap base (8 KB), 2 characters/bitmap from ROM | \| `$C0` |
| $13 | Bits 7-2 character base (1 KB, or 2 KB in ECM/256-character mode), 1 single clock, 0 ROM selected (read-only) | |
| $14 | Bits 7-3 attribute base (2 KB); screen = base + `$400` | \| `$07` |
| $15-$18 | Background colours 0-3 | \| `$80` |
| $19 | Border | \| `$80` |
| $1A/$1B | Character position (10 bits) | `$1A` \| `$FC` |
| $1C/$1D | Raster line (9 bits), writable | `$1C` \| `$FE` |
| $1E | Horizontal position: `((cycle - 16) * 4 / 2) & $FE` (writes ignored, as VICE) | |
| $1F | Bits 6-3 flash counter, 2-0 character row counter (both writable) | \| `$80` |
| $3E/$3F | Write: ROM / RAM in the upper 32 KB | `$FF` with RAM selected |

`$FF20-$FF3D` are not TED: `ioRead`/`ioWrite` return false there so the bus reaches RAM/ROM; so do reads of `$FF3E/$FF3F` while ROM is selected (the ROM shows). `ioPeek` returns what a read returns; no TED read changes state.

`reset()` is VICE's `ted_reset()` + `ted_reset_registers()`: every register `$00-$3D` written with 0 (so all three timers run with 65536 periods and the raster line is 0), then `$3E`.

## 4. Video

- **Frame**: 384 x 288 (PAL, TV lines 19-306) or 384 x 242 (NTSC, 19-260), VICE's "normal" borders: 32 border pixels each side of the 320-pixel display. Raster line 4 (the first of 25 rows) is frame row 40 (PAL) / 18 (NTSC). `renderFrame` copies the last complete frame (0xFFRRGGBB).
- **Lines**: each line is drawn when it ends (cycle 114), with the registers as they are then, so changes between lines show; a change within a line applies to the whole line, except the X scroll and 38/40-column switch, which (as in VICE) wait for the next line once past cycles 18 / 17 / 56.
- **Sequencer** (ted.c / ted-fetch.c): attributes fetched on lines where `(line & 7) == Yscroll`, character pointers on the next line, the row counter and the character position counters as VICE keeps them; idle state after line `$CB`, display enable latched at line 0, 24/25-row and 38/40-column borders.
- **Modes**: standard text (128 characters with hardware reverse, or 256 with `$FF07` bit 7), flashing (attribute bit 7, 16 frames on, 16 off), the hardware cursor, multicolour text (attribute bit 3; colour `attr & $77`), extended colour text, hires bitmap (colours from screen nibbles + attribute luminances; the overscan behind the X scroll takes the last character's colour, as in VICE), multicolour bitmap, illegal modes black, idle state (background with black pixels from `$FFFF`).
- **Palette**: 128 entries, luminance (bits 6-4) x hue (bits 3-0), hue 0 black at every luminance, computed as VICE does by default (colodore angles and luminances, saturation 1.25, phase -9.5, PAL YUV or NTSC YIQ). `paletteColor(byte)`.

## 5. Sound

Two square-wave voices with 10-bit frequencies (period `$400 - freq`, frequency `$3FE` holds the output high), voice 2 can play noise (8-bit LFSR) instead, volume 0-8 (9-15 as 8), and DA mode (the enabled voices' levels as a constant). Generated as VICE's non-float path, one sample per host sample, at `nativeSampleRate()` (44100 Hz default, `setSampleRate()`), mono floats 0..0.61 through `pullSamples()`.

## 6. Not emulated

- VICE's per-pixel "raster changes" (mid-line colour/mode splits), the grey dot drawn when a colour register is written, the open side-border tricks, light pen.
- The read-modify-write special cases of `$FF09/$FF0A/$FF0B` in VICE (the CPU's own dummy write has the same effect when the core performs it).
- The raster IRQ's extra cycle on line 0 and VICE's two-cycle IRQ delay bookkeeping (the flag is set at the start of the compare line).
- `$FF07` bit 5 (freeze) and bit 6 (NTSC) have no effect beyond being stored; the standard is chosen with `setStandard()`.
