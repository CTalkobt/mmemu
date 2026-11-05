# mmemu-plugin-antic: Atari ANTIC Implementation

This document describes the ANTIC (Alpha-Numeric Television Interface Controller) implementation for the mmemu platform.

---

## 1. Intent
ANTIC is the primary DMA engine and display controller for the Atari 8-bit family (400/800/XL/XE). It interprets a "Display List" (a simple machine-code-like program for the screen) and generates timing and data for the GTIA chip.

---

## 2. Functionality

ANTIC is clocked once per CPU cycle: NTSC, 114 cycles a line and 262 lines a frame. It reads the display list, fetches screen data, character shapes and player/missile data, and hands each scan line to GTIA as playfield colour classes (background, PF0-PF3, high-resolution pixels). GTIA adds the players and missiles and colours the line (`src/plugins/devices/antic/main/antic.cpp`).

### 2.1 Display list

- **Where it runs:** on scan lines 8-247.
- **Instructions:**
  - Blank lines (1-8 lines).
  - `JMP`, and `JVB`, which blanks until the vertical blank.
  - Character modes 2-7 and map modes 8-F.
  - `LMS` loads the memory scan counter, which wraps within 4 KB. The display list counter wraps within 1 KB.
- **Character modes:**

  | Mode | Description |
  |---|---|
  | 2 | Hi-res text, 8 lines |
  | 3 | Hi-res text, 10 lines. `$60-$7F` characters drop by two lines (descenders). |
  | 4, 5 | Four-colour text, 8 and 16 lines. Bit 7 of a character selects PF3 for `11` pixels. |
  | 6, 7 | Five-colour text, 8 and 16 lines. The character's top two bits select PF0-PF3. |

  CHBASE is 1 KB aligned for modes 2-5 and 512 bytes for 6-7. CHACTL controls inverse video, blanking of inverse characters, and upside-down characters.
- **Map modes 8-F:** 1 or 2 bits a pixel, 1 to 8 scan lines. Mode F is high resolution.
- **Playfield width (DMACTL bits 0-1):** narrow, normal or wide.
- **Fine scrolling:**
  - **HSCROL:** the line is fetched one width wider and shifted right by HSCROL colour clocks, then clipped to the window.
  - **VSCROL:** the first scrolled mode line starts at row VSCROL. The line after the scrolled region ends at row VSCROL.

### 2.2 Player/missile DMA

When DMACTL bits 2-3 enable it, ANTIC fetches the missiles and the four players for each scan line from PMBASE: double-line at `+$180`/`+$200`, or single-line (DMACTL bit 4) at `+$300`/`+$400`. GTIA latches the data when GRACTL enables it.

### 2.3 Interrupts (NMI)

- **DLI:** fires at cycle 8 of the last scan line of a mode line whose instruction has bit 7 set.
- **VBI:** fires at the start of line 248.
- **RESET key:** NMIST bit 5 (the 400/800's RESET key).
- **NMIST:** setting the DLI or VBI bit clears the other. Writing NMIRES clears them. Unused bits read 1.
- **The NMI line:** held to the next line start, so the CPU sees the edge even while WSYNC stops it.

### 2.4 Taking cycles from the CPU

- **`cpuBlocked()`:** the machine's scheduler asks this before each CPU step. While it is true, the scheduler clocks the devices one cycle without the CPU.
- **WSYNC:** stops the CPU until cycle 104 of the line. The line is drawn at that point, so colour changes after a WSYNC take effect on the next line.
- **DMA:** each display line takes 9 refresh cycles, plus the display-list bytes, the playfield bytes (the first line of a mode line), the character shapes (each line of a text mode) and 5 cycles of player/missile data.

### 2.5 Video output

`IVideoOutput` gives a 384 × 240 frame: colour clocks 32-223 (a wide playfield), two pixels each, for scan lines 8-247.

### 2.6 Not modelled

- Exact per-cycle DMA positions and mid-line register changes: a line is drawn once, at cycle 104.
- GTIA modes 9/10/11 (PRIOR bits 6-7), VDELAY, the light pen, and PAL timing in the machines.

## 3. Register Map ($D400–$D40F)

| Offset | Symbol | Access | Description |
|--------|--------|--------|-------------|
| $00 | DMACTL | W | DMA Control |
| $01 | CHACTL | W | Character Control |
| $02 | DLISTL | W | Display List Pointer Low |
| $03 | DLISTH | W | Display List Pointer High |
| $04 | HSCROL | W | Horizontal Scroll |
| $05 | VSCROL | W | Vertical Scroll |
| $07 | PMBASE | W | Player-Missile Base Address |
| $09 | CHBASE | W | Character Base Address |
| $0A | WSYNC | W | Wait for Horizontal Sync |
| $0B | VCOUNT | R | Vertical Line Counter |
| $0E | NMIEN | W | NMI Enable |
| $0F | NMIST | R | NMI Status |
| $0F | NMIRES | W | NMI Reset |

---

## 4. Dependencies
- **Depends on**: `ICore` (for `HALT` signal), `ISignalLine` (for `NMI`).
- **Required by**: Atari Machine Plugins (Phase 26.4).

---

## 5. Implementation Details
- **Source Location**: `src/plugins/devices/antic/`
- **Plugin**: `lib/mmemu-plugin-antic.so`
- **Class**: `Antic`
- **Registration Name**: `"ANTIC"`

## Debugger inspection

ANTIC implements `IVideoInspect` for the Graphics (ANTIC/GTIA) pane, the CLI's
`gfx chip-sprite`/`gfx chip-charset` and MCP `render_graphics`: players 0-3 and
the missiles (sprite 4) from PMBASE at the DMACTL resolution, and the character
set from CHBASE in the display list's main text mode, coloured from GTIA's
registers. See [README-graphics-viewer.md](README-graphics-viewer.md).
