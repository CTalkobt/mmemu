# mmemu-plugin-kbd-vic20: VIC-20 Keyboard Matrix Implementation

This document describes the Keyboard Matrix implementation for the VIC-20 machine on the mmemu platform.

---

## 1. Intent
Provides a standard 8x8 keyboard matrix simulation specifically for the Commodore VIC-20. It allows both physical keyboard mapping (in the GUI) and programmatic keystroke injection (via CLI and MCP).

---

## 2. Functionality

### 2.1 Matrix Simulation
- **8x8 Grid**: Tracks 64 keys across 8 columns and 8 rows.
- **Port Interfaces**: Provides two `IPortDevice` interfaces via `getPort(index)`:
    - **Column Port (Index 0)**: Wired to VIA2 Port B (`$9120`) — KERNAL drives columns low one at a time.
    - **Row Port (Index 1)**: Wired to VIA2 Port A (`$9121`) — KERNAL reads which rows are pulled low by pressed keys.
- **Active-low logic**: All lines are active-low; an unpressed key reads as 1, a pressed key as 0.

### 2.2 Host Interface (`IKeyboardMatrix`)
- **Key Injection**: `keyDown(col, row)` and `keyUp(col, row)` for direct matrix access using PB-bit (column) and PA-bit (row) indices.
- **Symbolic Mapping**: `pressKeyByName(name, down)` accepts the key names listed in Section 4 below.

---

## 3. Dependencies
- **Depends on**: None (Core mmemu interfaces only).
- **Required by**: [VIC-20 Machine Plugin](README-VIC20.md).

---

## 4. Implementation Details
- **Source Location**: `src/plugins/devices/kbd_vic20/`
- **Library**: `lib/internal/libdeviceKbdVic20.a`
- **Plugin**: `lib/mmemu-plugin-kbd-vic20.so`
- **Class**: `KbdVic20`
- **Registration Name**: `"kbd_vic20"`

---

## 5. Key Names and PC Keyboard Mapping

All key names are case-insensitive. Use these names with `pressKeyByName()` or in any host key-binding configuration.

### 5.1 Special / Non-Obvious Keys

These are the keys most likely to cause confusion because they have no obvious equivalent on a modern PC keyboard.

| VIC-20 Key | Key Name | Suggested PC Key | Notes |
|---|---|---|---|
| RUN/STOP | `run_stop` | `Escape` | Breaks running BASIC programs; SHIFT+RUN/STOP loads from tape |
| RESTORE | `restore` | `Page Up` | Not in the matrix: it pulls VIA1 CA1 low, and VIA1's interrupt output is the CPU's NMI. RUN/STOP+RESTORE warm-starts BASIC. Wired by the machine's `kbdWiring.restore`. |
| CBM (Commodore) | `cbm` | `Left Alt` | The Commodore logo key; used for graphics characters and SHIFT+CBM toggles character set |
| CTRL | `control` | `Tab` | Holds colour codes in BASIC; `CTRL+1`–`CTRL+8` set foreground colour |
| ← (arrow-left char) | `arrow_left` | `` ` `` (backtick) | Types the PETSCII `←` character (code `$5F`); **not** a cursor key |
| ↑ (arrow-up char) | `arrow_up` | `\` (backslash) | Types the PETSCII `↑` character (code `$5E`); **not** a cursor key. Its own key (PB6/PA6) |
| £ (pound sterling) | `pound` | `#` | Types the `£` symbol (PETSCII code `$5C`) |
| DEL / INST | `delete` | `Backspace` | Deletes character to the left; SHIFT+DEL inserts a space |
| HOME / CLR | `home` | `Home` | Moves cursor to top-left; SHIFT+HOME clears the screen |
| CRSR → / ← | `right` | `→` (right arrow) | Moves cursor right; hold SHIFT to move left. `left` presses SHIFT+`right` |
| CRSR ↓ / ↑ | `down` | `↓` (down arrow) | Moves cursor down; hold SHIFT to move up |
| Cursor up | `up` | `↑` (up arrow) | Presses SHIFT+`down` (the VIC-20 has no separate cursor-up key) |
| + | `plus` | `+` / `=` | Plus sign; not the same physical key as `=` |
| * | `asterisk` | `*` / `8` | Asterisk |
| @ | `at` | `@` / `2` | At sign |
| = | `equal` | `=` | Equals |
| - | `minus` | `-` | Minus / hyphen (PB7/PA5) |
| ; | `semicolon` | `;` | Semicolon |
| : | `colon` | `:` | Its own key (PB5/PA5); SHIFT+`:` is `[` (`bracket_left`), SHIFT+`;` is `]` (`bracket_right`) |
| / | `slash` | `/` | Forward slash |
| , | `comma` | `,` | Comma |
| . | `period` | `.` | Full stop / period |

### 5.2 Function Keys

The VIC-20 has four physical function keys. Each produces a second function when SHIFT is held; those are not separate matrix entries — inject SHIFT + the base key.

| VIC-20 Key | Key Name | Shifted function |
|---|---|---|
| F1 | `f1` | F2 (inject `left_shift` + `f1`) |
| F3 | `f3` | F4 |
| F5 | `f5` | F6 |
| F7 | `f7` | F8 |

### 5.3 Modifier Keys

| VIC-20 Key | Key Name | Notes |
|---|---|---|
| Left SHIFT | `left_shift` | |
| Right SHIFT | `right_shift` | Functionally identical to left shift for most purposes |
| CTRL | `control` | See Section 5.1 |
| CBM (Commodore) | `cbm` | See Section 5.1 |

### 5.4 Full Matrix Reference

As in VICE's `vic20via2.c`. Column PB7 is shared with the joystick's right switch (see README-VIC20.md); only PB bits set as outputs drive a column.

The hardware matrix uses VIA2 Port B to select columns (PB0–PB7 driven low in turn) and reads rows from VIA2 Port A (PA0–PA7). The table below lists every key by its matrix position.

```
              PB0         PB1          PB2       PB3         PB4          PB5     PB6        PB7
PA0 (bit 0):  1           arrow_left   control   run_stop    space        cbm     q          2
PA1 (bit 1):  3           w            a         left_shift  z            s       e          4
PA2 (bit 2):  5           r            d         x           c            f       t          6
PA3 (bit 3):  7           y            g         v           b            h       u          8
PA4 (bit 4):  9           i            j         n           m            k       o          0
PA5 (bit 5):  plus        p            l         comma       period       colon   at         minus
PA6 (bit 6):  pound       asterisk     semicolon slash       right_shift  equal   arrow_up   home
PA7 (bit 7):  delete      return       right     down        f1           f3      f5         f7
```

---

## 6. Injecting Key Combinations

To simulate a shifted key (e.g. `"` which is SHIFT+2 on a real VIC-20 keyboard), press both keys simultaneously:

```
pressKeyByName("left_shift", true)
pressKeyByName("2", true)
// ... hold for one frame or one scanline period ...
pressKeyByName("2", false)
pressKeyByName("left_shift", false)
```

The KERNAL's keyboard scanner (`SCNKEY`) runs once per video frame. Both keys must be held at the same time when the scanner reads the matrix.

To break a running program: inject `run_stop` (no shift needed). To clear the screen: inject `left_shift` + `home` simultaneously.
