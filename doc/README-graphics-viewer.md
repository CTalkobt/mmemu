# Graphics Viewers (VIC and ANTIC/GTIA)

Issue #28. Decodes sprite and character graphics from memory. It is available
in three places, which all use the same code (`src/libtoolchain/main/gfx_decode.*`
for decoding, `src/libdebug/main/gfx_view.*` for following the video chip):

- **GUI**: View > Graphics (VIC) (pane id `graphics-vic`) or View > Graphics (ANTIC/GTIA) (`graphics-antic`, Atari 8-bit), side panes
- **CLI**: `gfx`
- **MCP**: `render_graphics`

## Formats

| id          | Format                                 | Size   | Bytes per item |
|-------------|----------------------------------------|--------|----------------|
| `sprite`    | VIC-II sprite, hires                   | 24x21  | 64             |
| `sprite-mc` | VIC-II sprite, multicolour             | 12x21 (double-wide pixels) | 64 |
| `sprite64`  | VIC-IV 64-wide sprite (SPRX64EN)       | 64x21  | 192            |
| `sprite16`  | VIC-IV 16-colour sprite with SPRX64EN  | 16x21  | 192            |
| `sprite16-6` | VIC-IV 16-colour sprite without SPRX64EN (3 bytes per row) | 6x21 | 64 |
| `char`      | Character, hires                       | 8x8    | 8              |
| `char-mc`   | Character, multicolour                 | 4x8 (double-wide pixels) | 8 |
| `char-fcm`  | VIC-IV full-colour character           | 8x8, 1 byte per pixel | 64 |
| `char-ncm`  | VIC-IV nybl-colour character           | 16x8, 4 bits per pixel | 64 |
| `char16`    | VIC-I double-height character          | 8x16   | 16             |
| `char16-mc` | VIC-I double-height character, multicolour | 4x16 (double-wide pixels) | 16 |
| `antic2`    | ANTIC character, modes 2/3 (and 6/7: characters 0-63) | 8x8 | 8 |
| `antic4`    | ANTIC character, modes 4/5, 4 colours  | 4x8 (double-wide pixels) | 8 |
| `player`    | ANTIC/GTIA player or missiles, single-line resolution | 8x256 | 256 |
| `player-2line` | ANTIC/GTIA player or missiles, double-line resolution | 8x128 | 128 |

The missiles share one byte per row, 2 bits each (missile 3 in bits 7-6 to
missile 0 in bits 1-0), so `player` at the missile address shows all four.

### Formats per machine

It is a viewer for the VIC family. Each machine lists the formats its video chip
has (`"ui": {"graphicsFormats": [...]}` in the machine JSON,
`MachineDescriptor::graphicsFormats`), and the GUI, `gfx formats` and
`render_graphics` offer only those:

| Machine | Video chip | Formats |
|---------|------------|---------|
| VIC-20  | VIC-I      | `char`, `char-mc`, `char16`, `char16-mc` (no sprites) |
| C64     | VIC-II     | `sprite`, `sprite-mc`, `char`, `char-mc` |
| MEGA65  | VIC-IV     | every format (it declares no list) |
| Atari 8-bit | ANTIC + GTIA | `antic2`, `antic4`, `player`, `player-2line` (in the Graphics (ANTIC/GTIA) pane) |
| PET, X16 | CRTC / VERA | none: no viewer pane. A VERA viewer (sprites, tiles) is planned as its own pane. |

The stride (bytes from one item to the next) defaults to the size above and can
be changed, e.g. to view sprites packed at 63-byte intervals.

4-bit formats: NCM characters show the low nibble as the left pixel and
16-colour sprites the high nibble (as mmsim's VIC-IV renderer draws them).
"Swap nibbles" reverses this.

## Following the video chip

On machines whose video chip supports inspection (VIC-II on the C64, VIC-IV on
the MEGA65), the viewer can follow:

- **a sprite**: data address from the sprite pointer (the VIC-IV's relocated
  and 16-bit pointers included), its mode (multicolour, 64-wide, 16-colour,
  extended height) and its colours;
- **the character set**: the current character/bitmap base, multicolour or
  full-colour mode, and the background colours.

On the Atari (ANTIC/GTIA), the same sources are:

- **Player 0-3** and **Missiles**: from PMBASE, at the resolution DMACTL selects
  (single-line: missiles at +$300, players from +$400, 256 bytes each;
  double-line: +$180 and +$200, 128 bytes each), in the player's COLPMn colour
  (the missiles in COLPM0);
- **Character set (CHBASE)**: 128 characters in the text mode the display
  list uses most: modes 2/3 in COLPF2 with COLPF1's luminance, modes 4/5 in
  COLBK/COLPF0/COLPF1/COLPF2, modes 6/7 (64 characters, CHBASE on a 512-byte
  boundary) in COLPF0 on COLBK. Colours come from GTIA's NTSC or PAL palette.

**Video chip view** reads memory the way the video chip fetches it, so the C64
character ROM appears at $1000-$1FFF and $9000-$9FFF of VIC banks 0 and 2.
**CPU view** reads through the CPU's memory map.

## GUI

Controls: source (address, VIC sprite 0-7 / Atari player 0-3 and missiles, the
character set), address (any expression), format, count, columns, stride, zoom,
CPU/video-chip view, palette offset for 16-colour formats and swap nibbles (VIC
pane only), live update, Export PNG. Each pane lists only the machine's formats
and appears only on machines that have its chip.

Hover an item to see its address in the pane's status line. Double-click opens
it in the memory pane; right-click to copy its address or start the view there.
With **Live** checked the sheet redraws (at most 10 times a second) while the
machine runs.

## CLI

```
gfx formats
gfx <format> <addr> [count] [columns] [vic] [swap] [file.png]
gfx chip-sprite <n> [file.png]      VIC sprite 0-7; Atari player 0-3, 4 = missiles
gfx chip-charset [count] [columns] [file.png]
(vic-sprite / vic-charset are the same commands)
```

Without a file the items are printed as ASCII art (at most 16): `.` for 0,
`#` for set pixels in 1-bit formats, and the colour value for the others.

```
> gfx char $1008 1 vic
Character (hires) at $1008, 1 item, stride 8, video chip view
#0 $1008
...##...
..####..
.##..##.
.######.
.##..##.
.##..##.
.##..##.
........
```

## MCP

`render_graphics` takes `machine_id`, `source` (`address`, `sprite`,
`charset`), `address`, `format`, `sprite`, `count`, `columns`, `stride`,
`video_view`, `swap_nibbles` and `path`. With `path` it writes a PNG sheet;
otherwise it returns ASCII art.
