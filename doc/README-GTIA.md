# mmemu-plugin-gtia: Atari GTIA Implementation

This document describes the GTIA (Graphics Television Interface Adaptor) implementation for the mmemu platform.

---

## 1. Intent
GTIA is the secondary video chip in the Atari 8-bit family. It receives digital video data from ANTIC and converts it into a color signal. It also handles player-missile graphics (PMG), collision detection, and console switches.

---

## 2. Functionality

### 2.1 Colour generation

The NTSC palette (`getARGB`) has 256 entries: hue in bits 4-7, luminance in bits 0-3. It is generated with atari800's palette model (#160). Colour registers ignore bit 0 of the luminance.

### 2.2 Scanline composition

`renderLine(pf, out)` takes ANTIC's playfield classes for one line (384 pixels, colour clocks 32-223) and draws the players and missiles over them:

- **Players:** 8 bits from GRAFP0-3 (from DMA or the CPU), placed at HPOSP0-3. SIZEP sets 1, 2 or 4 colour clocks per bit.
- **Missiles:** 2 bits each from GRAFM, placed at HPOSM0-3, with the sizes in SIZEM. Each uses its player's colour. With PRIOR bit 4 (fifth player), missiles take PF3's colour.
- **Priority (PRIOR bits 0-3):** players over playfield (1), P0-1 over playfield over P2-3 (2), playfield over players (4), PF0-1 over players over PF2-3 (8). 0 behaves as 1.
- **High-resolution pixels (modes 2, 3, F):** PF2's hue with PF1's luminance. The luminance also shows through a player in front.

### 2.3 Collision detection

Each colour clock, GTIA checks overlaps and updates the collision registers:

- Missiles against the playfield (M0PF-M3PF) and against players (M0PL-M3PL).
- Players against the playfield (P0PF-P3PF) and against other players (P0PL-P3PL).

On high-resolution lines, only lit pixels collide, as PF2. HITCLR clears the registers.

### 2.4 Console keys and triggers

- **CONSOL bits 0-2:** START, SELECT and OPTION, active low (`setConsoleKey`).
- **TRIG0-3:** joystick buttons (`setTrigger`). GRACTL bit 2 latches presses.
- **PAL register:** `$0F` on NTSC.

### 2.5 Not modelled

GTIA modes (PRIOR bits 6-7), multicolour players (PRIOR bit 5), VDELAY, and the console speaker.

## 3. Register Map ($D000–$D01F)

GTIA uses overlapping addresses for read and write operations.

### Write-Only Registers

| Offset | Symbol | Description |
|--------|--------|-------------|
| $00-$03 | HPOSP0-3 | Horizontal Position Players 0-3 |
| $04-$07 | HPOSM0-3 | Horizontal Position Missiles 0-3 |
| $08-$0B | SIZEP0-3 | Size Players 0-3 |
| $0C | SIZEM | Size Missiles |
| $0D-$10 | GRAFP0-3 | Graphics Data Players 0-3 |
| $11 | GRAFM | Graphics Data Missiles |
| $12-$15 | COLPM0-3 | Color Players 0-3 |
| $16-$19 | COLPF0-3 | Color Playfields 0-3 |
| $1A | COLBK | Color Background |
| $1B | PRIOR | Priority Select |
| $1C | VDELAY | Vertical Delay |
| $1D | GRACTL | Graphics Control |
| $1E | HITCLR | Clear Collisions |
| $1F | CONSOL | Console Speaker / Switches |

### Read-Only Registers

| Offset | Symbol | Description |
|--------|--------|-------------|
| $00-$03 | M0-3PF | Missile to Playfield Collisions |
| $04-$07 | P0-3PF | Player to Playfield Collisions |
| $08-$0B | M0-3PL | Missile to Player Collisions |
| $0C-$0F | P0-3PL | Player to Player Collisions |
| $10-$13 | TRIG0-3 | Joystick Triggers |
| $14 | PAL | PAL/NTSC Status |
| $1F | CONSOL | Console Switches |

---

## 4. Dependencies
- **Depends on**: Core mmemu interfaces.
- **Required by**: Atari Machine Plugins (Phase 26.4).

---

## 5. Implementation Details
- **Source Location**: `src/plugins/devices/gtia/`
- **Plugin**: `lib/mmemu-plugin-gtia.so`
- **Class**: `GTIA`
- **Registration Name**: `"GTIA"`
