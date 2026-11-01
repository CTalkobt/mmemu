# Graphics Viewer (sprites and characters)

Issue #28. Decodes sprite and character graphics from memory. It is available
in three places, which all use the same code (`src/libtoolchain/main/gfx_decode.*`
for decoding, `src/libdebug/main/gfx_view.*` for following the video chip):

- **GUI**: View > Graphics (a side pane)
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

**Video chip view** reads memory the way the video chip fetches it, so the C64
character ROM appears at $1000-$1FFF and $9000-$9FFF of VIC banks 0 and 2.
**CPU view** reads through the CPU's memory map.

## GUI

Controls: source (address, VIC sprite 0-7, VIC character set), address
(any expression), format, count, columns, stride, zoom, CPU/video-chip view,
palette offset for 16-colour formats, swap nibbles, live update, Export PNG.

Hover an item to see its address in the pane's status line. Double-click opens
it in the memory pane; right-click to copy its address or start the view there.
With **Live** checked the sheet redraws (at most 10 times a second) while the
machine runs.

## CLI

```
gfx formats
gfx <format> <addr> [count] [columns] [vic] [swap] [file.png]
gfx vic-sprite <0-7> [file.png]
gfx vic-charset [count] [columns] [file.png]
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
