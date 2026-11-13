# mmemu-plugin-vic2: MOS 6567/6569 VIC-II Implementation

The MOS 6567 (NTSC) and MOS 6569 (PAL) Video Interface Chip II is the graphics processor for the Commodore 64. It is implemented as an `IOHandler` and `IVideoOutput` plugin within `mmemu-plugin-vic2.so`.

---

## 1. Intent

The VIC-II generates composite video output by fetching character data, bitmap data, and sprite shapes from memory via a dedicated DMA bus. It also drives the C64's primary raster interrupt, enabling cycle-accurate timing of display effects.

---

## 2. Functionality

### 2.1 Video Modes

Five video modes are selected by the ECM, BMM, and MCM control bits:

| Mode | ECM | BMM | MCM | Description |
|------|-----|-----|-----|-------------|
| Standard Text | 0 | 0 | 0 | 40×25 chars, 16 foreground colors per cell |
| Multicolor Text | 0 | 0 | 1 | 40×25 chars, 4 colors per cell (2-pixel wide) |
| Standard Bitmap | 0 | 1 | 0 | 320×200 pixels, 2 colors per 8×8 cell |
| Multicolor Bitmap | 0 | 1 | 1 | 160×200 pixels, 4 colors per 4×8 cell |
| Extended Background Color | 1 | 0 | 0 | Text with 4 selectable background colors |

### 2.2 Display Geometry

| Constant | Value | Description |
|----------|-------|-------------|
| `FRAME_W` | 384 | Total frame width including borders (pixels) |
| `FRAME_H` | 272 | Total frame height including borders (lines) |
| `DISPLAY_X` | 32 | Left border width (pixels) |
| `DISPLAY_Y` | 36 | Top border height (lines) |
| `DISPLAY_W` | 320 | Active display width: 40 columns × 8 pixels |
| `DISPLAY_H` | 200 | Active display height: 25 rows × 8 pixels |
| `CYCLES_PER_LINE` | 65 | Phi2 cycles per scanline (NTSC 6567) |
| `LINES_PER_FRAME` | 263 | Total scanlines per frame (NTSC) |

### 2.3 Sprite Engine

8 hardware sprites (MOBs), each 24×21 pixels:

- Independent X/Y position registers with MSB (bit 8) in `$D010`
- Per-sprite enable (`$D015`), multicolor (`$D01C`), X/Y expansion (`$D01D`/`$D017`)
- Priority control (`$D01B`): sprite-behind-background per sprite
- Registers repeat every 64 bytes through the window the `decode` option gives (`setOption("decode", "0x3FF")`; the C64 in `machines/c64.json`; VICE `GEO-RAM/newtest-auto` reads `$D0FE` as `$FF`)
- **Phi1 bus value** (`IBaSource::phi1Value`): what the VIC-II fetched in the first half of the current cycle, from VICE's cycle tables (sprite pointers, sprite data, refresh, graphics, else the idle fetch); a 6510 write to `$00`/`$01` stores it in RAM (`REU/cpuport`)
- Sprite-sprite collision register (`$D01E`): cleared on read
- Sprite-background collision register (`$D01F`): cleared on read
- Mono sprites: 1-bit pixels, one color per sprite
- Multicolor sprites: 2-bit pixels, shared `$D025`/`$D026` plus per-sprite color

#### 2.3.1 Collisions
Collisions come from a port of VICE's sprite sequencer (`viciisc/vicii-draw-cycle.c`, `draw_sprites8`), run pixel by pixel over a line (`VIC2::evalCollisions`). The picture the GUI shows is still drawn once a frame; the sequencer is only used for the collision registers.
- **Pixel time:** 8 pixels a cycle. Each cycle puts out the X from VICE's cycle table, rounded down to 8. On PAL, cycle 1 puts out X `$190`-`$197`, cycle 13 `$1F0`-`$1F7` and cycle 14 `$000`-`$007`, so a sprite at X `$1F8`-`$1FF` never starts.
- **Sequencer:** a sprite starts where the X put out matches its X register as it was a cycle earlier. It shifts out one bit a pixel (two pixels a bit when X-expanded, in pairs when multicolour). While its own pointer and data are fetched it pauses, still putting out the same pixel (the "sprite gap", VICE `VICII/spritegap`). Its shift register is loaded in its second DMA cycle, and display starts from cycle 58 if its DMA is on.
- **Graphics:** a foreground pixel (hires 1, multicolour `10`/`11`) is at X 24 + XSCROLL + 8 × column + bit, in the 40 columns of g-accesses. Text rows start on line `$30` + YSCROLL. The side border hides graphics but doesn't stop their collisions; the vertical border flip-flop does (VICE `draw_graphics8`). It is set on the bottom line for RSEL and cleared on the top one when DEN is set, so moving RSEL past the bottom line opens the border. Outside the display rows the graphics are the idle fetch (`$3FFF`, `$39FF` with ECM), which shows and collides in an opened border (VICE `REU/bonzai/spritetiming`).
- **Fetches as they happen:** with sprites on, the VIC-II ticks cycle by cycle and keeps each g-access and each sprite's data as fetched; the sequencer uses those, so memory changed during a line (an REU streaming into a character) counts from the cycle it changed (`REU/bonzai/checkchar`).
- **Registers:** two sprites' pixels meeting set `$D01E`, border included; a sprite pixel on foreground graphics sets `$D01F`. Each register raises its IRQ (`$D019` bit 2 / bit 1) when it goes from 0.
- **Timing:** the sequencer runs lazily. A read of `$D01E`/`$D01F` runs it up to the cycle before the read, a write to a sprite register runs it up to the write first, and the end of each line runs the rest. So a read sees the pixels of the cycles before it (VICE `VICII/spritecollisions` `*-collision-cycle`).
- **Not done:** the sprite DMA and display flags (`mc`, `mcbase`, the Y-expansion flop) are simplified, so a sprite restarted on its last line (VICE `VICII/spriterestart`) isn't right. Neither are the 6569R1's and 8565's own colour latencies; the 6569's is used.

### 2.4 Raster IRQ

The raster counter advances once per scanline. When it matches the value in `$D012` (plus bit 8 from `$D011` bit 7), the VIC-II sets IRQ bit 0 in `$D019` and asserts the IRQ signal line. The host clears the flag by writing `1` to `$D019` bit 0.

```cpp
vic2->setIrqLine(cpuIrqLine);
```

### 2.5 VIC-II Banking

The VIC-II has its own 16 KB address space, selected by CIA #2 Port A bits 0–1 (inverted). The machine factory calls `setBankBase()` whenever those bits change:

```cpp
vic2->setBankBase(0x0000);  // VIC bank 0 ($0000–$3FFF)
vic2->setBankBase(0x4000);  // VIC bank 1 ($4000–$7FFF)
vic2->setBankBase(0x8000);  // VIC bank 2 ($8000–$BFFF)
vic2->setBankBase(0xC000);  // VIC bank 3 ($C000–$FFFF)
```

Within any bank, the character ROM is always visible at offsets `$1000–$1FFF` and `$9000–$9FFF` regardless of CPU banking:

```cpp
vic2->setCharRom(charRomPtr, 4096);
```

### 2.6 Memory Pointer Register ($D018 / VMEM)

| Bits | Field | Description |
|------|-------|-------------|
| 7–4 | VM[14:11] | Screen RAM start: offset = `((bits>>4)&0x0F) * 0x0400` |
| 3–1 | CB[13:11] | Char/bitmap base: offset = `((bits>>1)&0x07) * 0x0800` |

### 2.7 Color Palette

Standard C64 16-color RGBA8888 palette (Pepto's measurements):

| Index | Name | RGBA |
|-------|------|------|
| 0 | Black | $000000FF |
| 1 | White | $FFFFFFFF |
| 2 | Red | $68372BFF |
| 3 | Cyan | $70A4B2FF |
| 4 | Purple | $6F3D86FF |
| 5 | Green | $588D43FF |
| 6 | Blue | $352879FF |
| 7 | Yellow | $B8C76FFF |
| 8 | Orange | $6F4F25FF |
| 9 | Brown | $433900FF |
| 10 | Light Red | $9A6759FF |
| 11 | Dark Grey | $444444FF |
| 12 | Grey | $6C6C6CFF |
| 13 | Light Green | $9AD284FF |
| 14 | Light Blue | $6C5EB5FF |
| 15 | Light Grey | $959595FF |

---

## 3. Register Map (partial)

| Offset | Symbol | Description |
|--------|--------|-------------|
| $00–$0F | SP0X–SP7Y | Sprite 0–7 X/Y positions |
| $10 | MSIGX | Sprite X MSBs (bit 8) |
| $11 | SCR1 | Control 1: scroll-Y, row count, DEN, BMM, ECM, raster bit 8 |
| $12 | RASTER | Raster compare / raster counter read |
| $15 | SPENA | Sprite enable bits |
| $16 | SCR2 | Control 2: scroll-X, column count, MCM |
| $17 | YXPAND | Sprite Y expansion |
| $18 | VMEM | Memory pointers (screen RAM + char/bitmap base) |
| $19 | IRQ | IRQ status (write 1 to clear) |
| $1A | IRQEN | IRQ enable mask |
| $1B | SPBGPR | Sprite-background priority |
| $1C | SPMC | Sprite multicolor enable |
| $1D | XXPAND | Sprite X expansion |
| $1E | SSCOL | Sprite-sprite collision (cleared on read) |
| $1F | SBCOL | Sprite-BG collision (cleared on read) |
| $20 | EXTCOL | Border color |
| $21–$24 | BGCOL0–3 | Background colors 0–3 |
| $25–$26 | SPMC0–1 | Sprite shared multicolors |
| $27–$2E | SP0COL–SP7COL | Per-sprite colors |

Registers $2F–$3F are unused and read as $FF. The 64-byte window mirrors across $D000–$D3FF.

---

## 4. Dependencies

- **Depends on**: `IBus` (DMA bus), `ISignalLine` (raster IRQ), `IVideoOutput` (from `libdevices`)
- **Required by**: C64 Machine Plugin (Phase 11.6)

---

## 5. Implementation Details

- **Source Location**: `src/plugins/devices/vic2/main/vic2.h/cpp`
- **Plugin**: `lib/mmemu-plugin-vic2.so`
- **Class**: `VIC2 : public IOHandler, public IVideoOutput`
- **Registration Name**: `"6567"` (NTSC; PAL variant is `"6569"` in future)
- **Base Address**: `0xD000`
- **Address Mask**: `0x003F` (64-byte window; 47 registers used)
- **Frame format**: `FRAME_W × FRAME_H` pixels, RGBA8888
