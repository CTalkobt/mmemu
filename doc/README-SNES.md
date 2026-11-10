# mmemu-plugin-snes: Super Nintendo / Super Famicom

The SNES and Super Famicom (#15), after Mesen2 (GPL-3.0, Sour and the Mesen
contributors) and the fullsnes / Anomie documentation:
- **Timing and memory map:** `Core/SNES/SnesMemoryManager.cpp`, `InternalRegisters.cpp`.
- **DMA and HDMA:** `SnesDmaController.cpp`.
- **Cartridges:** `BaseCartridge.cpp`.
- **PPU:** `SnesPpu.cpp`.
- **Sound:** `Spc.cpp`, `SpcTimer.h`, `DSP/`.

## Profiles

| Profile | Machine |
|---------|---------|
| `snes` | NTSC SNES: master clock 21.477 MHz, 262 lines of 1364 clocks |
| `superfamicom` | The Super Famicom: the same machine here |

There is no ROM in the console: plug a cartridge in (`cart game.sfc` in the
CLI, File > Attach Cartridge in the GUI, MCP `attach_cartridge`). Inserting
one powers the console on; the machine's reset is the RESET button.

## Hardware

- **CPU**: the 5A22, the WDC 65C816 core (`W65C816`, cycle by cycle). Each
  bus cycle takes 6, 8 or 12 master clocks by region: FastROM through
  MEMSEL `$420D`, 12 for `$4000-$41FF`. Internal cycles take 6.
- **Memory map** (banks `$00-$3F` and `$80-$BF`):

| Address | |
|---------|---|
| `$0000-$1FFF` | the first 8 KB of work RAM |
| `$2100-$213F` | the PPU |
| `$2140-$217F` | the APU's four ports, mirrored |
| `$2180-$2183` | the work RAM port |
| `$4016-$4017` | the joypads' serial lines |
| `$4200-$421F` | interrupts, multiply/divide, timers, DMA enables, auto-read results |
| `$4300-$437F` | the eight DMA channels |
| `$6000-$7FFF` | HiROM battery RAM (banks `$20-$3F`) |
| `$8000-$FFFF` | the cartridge |

  Banks `$7E-$7F` hold all 128 KB of work RAM. Banks `$40-$7D` and
  `$C0-$FF` are the cartridge's.

- **Timing**: a line is 1364 master clocks (341 dots; dots 323 and 327
  take 6 clocks) and a frame 262 lines. Interlaced, every other frame has
  263 lines; otherwise line 240 of every other frame is 4 clocks short. The
  DRAM refresh steals 40 clocks a line at clock 538. HDMA runs at dot 276
  for the next line. Vertical blank starts at line 225 (240 with overscan):
  the NMI flag (`$4210`), the auto joypad read (`$4218-$421F`, 3 lines
  busy), and the OAM address reload. HDMA restarts at line 0.
- **Interrupts**: NMI (`$4200` bit 7) and the H/V timer IRQ (`HTIME`,
  `VTIME`, `$4211`).
- **Multiply/divide**: `$4202`/`$4203` and `$4204-$4206`, with results at
  `$4214-$4217`. They are given at once, not after 8/16 cycles.
- **DMA**: modes 0-7 in both directions, with A-bus increment, decrement or
  fixed. The CPU waits 8 clocks a byte.
- **HDMA**: direct and indirect tables, repeat and line counts.
- **Cartridges** (`.sfc`, `.smc`; a 512-byte copier header is skipped):
  - **Maps:** LoROM, HiROM and ExHiROM, chosen by scoring the internal
    header as bsnes and Mesen do.
  - **ROM sizes:** sizes that aren't a power of two mirror as in bsnes.
  - **Battery RAM:** LoROM `$70-$7D:$0000-$7FFF`, HiROM `$20-$3F:$6000-$7FFF`.
    A cartridge with a battery (chipset low nibble 2, 5, 6, 9 or A, as
    Mesen) keeps its RAM, or the Super FX's, in `<rom>.srm` next to the ROM
    (Mesen's and snes9x's format). The file is loaded when the cartridge goes
    in, written within a second of a change, and written again when the
    cartridge is removed.
  - **Coprocessors:** the DSP-1, 1B, 2, 3 and 4, the Super FX, the SA-1 and the Cx4 (see below).

### DSP-1 to DSP-4

The NEC uPD7725 (`snes::NecDsp`, after Mesen's `NecDsp.cpp`): a 16-bit
fixed-point DSP at 7.6 MHz running its own program (2048 24-bit words), with
a 1024-word data ROM, 256 words of RAM, a 16x16 multiplier working every
cycle, and two accumulators with their flags. Pilotwings, Super Mario Kart and
the other DSP-1 games use it for 3D maths.

- **Detection:** a chipset byte (`$FFD6`) of `$03`-`$0F` in the `$0x` range.
  The title picks the firmware as Mesen does: `PILOTWINGS` takes the
  DSP-1, `DUNGEON MASTER` the DSP-2, SD Gundam GX the DSP-3, Top Gear 3000
  the DSP-4, and everything else the DSP-1B.
- **Registers:**
  - **LoROM:** `$30-$3F:$8000-$FFFF` and `$60-$6F:$0000-$7FFF`, with DR
    below `$C000`/`$4000` and SR above.
  - **HiROM:** `$00-$1F:$6000-$7FFF`, with SR at `$7000`.
  - **Transfers:** DR goes through the byte latch, 16 or 8 bits.
- **Timing:** the DSP is brought up to the CPU's time whenever its registers
  are touched. While the firmware waits on RQM it sleeps until the next access.
- **Firmware:** the chip's ROM is not in the cartridge dump, and it is not
  part of mmsim (Nintendo/NEC copyright). Put it in `roms/snes/`
  (gitignored), or point `MMEMU_SNES_FIRMWARE` at another directory, under
  Mesen's names:
  - `dsp1.rom`, `dsp1b.rom`, `dsp2.rom`, `dsp3.rom` or `dsp4.rom`, 8192
    bytes each (the program, then the data);
  - or `<name>.program.rom` (6144 bytes) plus `<name>.data.rom` (2048 bytes).

  A ROM image with the firmware appended (8 KB past a 32 KB boundary) is
  used as it is. Without firmware, the cartridge is refused with a message
  naming the file it needs.

### Super FX

The GSU-1/GSU-2 (`snes::Gsu`, after Mesen's `Gsu.cpp`), used by Star Fox,
Yoshi's Island, Stunt Race FX, Doom and others. It is a 16-bit RISC CPU
running at the master clock (or half that), kept in step with the 65816
after every bus cycle.

- **Detection:** a chipset byte (`$FFD6`) of `$13`-`$1A`. Its RAM size
  comes from `$FFBD`, else 64 KB.
- **The chip:** sixteen registers and every instruction, with the ALT
  prefixes and FROM/TO/WITH. Also:
  - **Cache:** the 512-byte instruction cache.
  - **Buffered memory:** ROM reads through R14 and buffered RAM writes.
  - **Multiplies:** MULT/UMULT, FMULT/LMULT.
  - **PLOT/RPIX:** 2/4/8 bpp, with the 128/160/192-line and OBJ layouts,
    dither, transparency, and the two pixel caches.
- **Map:**
  - **Registers:** `$3000-$3FFF`, including the cache at `$3100`.
  - **RAM:** at `$6000-$7FFF` (its first 8 KB) and in `$70-$71`.
  - **ROM:** LoROM at `$8000`, and 64 KB a bank in `$40-$5F`.
  - **While the GSU runs:** when the GSU has the ROM or RAM (SCMR), the 65816
    sees the fixed interrupt-vector values from ROM and 0 from RAM. While it
    runs, only SFR and SCMR answer.
- **IRQ:** STOP raises the IRQ (unless CFGR masks it); reading `$3031`
  clears it.

### SA-1

The SA-1 (`snes::Sa1`, after Mesen's `Sa1.cpp`) is used by Super Mario RPG,
Kirby Super Star, Kirby's Dream Land 3 and others. It is a second 65C816 at
10.74 MHz (a second `W65C816` on its own bus), with 2 KB of I-RAM. It
shares the ROM and the battery RAM (BW-RAM) with the S-CPU and is kept in
step after every S-CPU bus cycle.

- **Detection:** a chipset byte of `$34`/`$35`. BW-RAM is the cartridge
  RAM from the header, saved like any battery RAM.
- **Memory:**
  - **ROM blocks:** the MMC's four 1 MB ROM blocks (`$2220-$2223`), LoROM in
    `$00-$3F`/`$80-$BF` and 64 KB banks in `$C0-$FF`.
  - **I-RAM:** at `$3000` (and `$0000` for the SA-1), with write protection.
  - **BW-RAM:** windows at `$6000-$7FFF` (`$2224`/`$2225`) and linear in
    `$40-$4F` (`$40-$5F` for the SA-1). The SA-1 also sees a 2- or 4-bit
    bitmap view in `$60-$6F` (and at `$6000` in bitmap mode). Writes are
    protected unless enabled.
- **CPU control:**
  - **Reset and wait:** the S-CPU starts, stops and holds the SA-1
    (`$2200`).
  - **Vectors:** the SA-1 has its own reset/NMI/IRQ vectors, and the S-CPU's
    NMI/IRQ vectors can be replaced.
  - **Signals:** messages and IRQs go both ways.
- **DMA:** between ROM, BW-RAM and I-RAM. The character conversion DMA turns
  bitmaps into SNES tiles: type 1 on the S-CPU's reads, type 2 through the
  bitmap registers.
- **Arithmetic:** signed multiply, divide and a 40-bit multiply-accumulate,
  with results after 5 or 6 cycles.
- **Variable-length bit reader:** `$2258-$225B`, `$230C/$230D`.
- **Not yet, as in Mesen:** the H/V timers, and the waits when both CPUs
  want the same memory. A BW-RAM access costs the SA-1 two cycles.

### Cx4

The Capcom Cx4 (`snes::Cx4`, after Mesen's `Cx4.cpp`) is used by Mega Man X2
and X3. It is a 24-bit DSP at 20 MHz and needs no firmware file: its built-in
table of maths (reciprocals, square roots, sines and tangents, 1024 words)
comes from Mesen's source.

- **Detection:** chipset `$Fx` with cartridge type `$10` (`$FFBF`).
- **The chip:**
  - **Program cache:** 256 16-bit instructions in two pages, loaded from ROM
    with the ROM/RAM wait states.
  - **Registers:** sixteen 24-bit registers, flags, and an 8-level call
    stack.
  - **Arithmetic:** a 48-bit multiplier.
  - **Memory:** 3 KB of data RAM, and buffered bus reads and writes.
  - **DMA:** between ROM, battery RAM and its data RAM. An invalid DMA
    locks the chip.
  - **Suspend.**
  - **IRQ:** raised on STOP.
- **SNES side:** the data RAM and registers are at `$6000-$7FFF` in banks
  `$00-$3F`/`$80-$BF`. ROM and battery RAM keep the LoROM map.

### PPU

Drawn as the beam goes: before each write to a PPU register (by the CPU,
DMA or HDMA) the line is drawn up to the current dot with the old values,
pixel x leaving at dot x + 22. Mid-line changes split the line as on the
hardware. A line's sprites are found and fetched in the previous line's
horizontal blank.

- **Backgrounds**:
  - **Modes 0-6:** 2/4/8 bpp, 8x8 and 16x16 tiles, 32/64-tile maps, direct
    colour, and offset-per-tile in modes 2, 4 and 6.
  - **Mode 7:** the matrix with `M7X/M7Y`, scroll, flips, the three
    screen-over modes, and EXTBG.
  - **Scroll registers:** the hardware's write latches.
- **Sprites**: 128 OAM entries, the 8 size pairs, 9-bit X, flips, priority
  rotation, and the 32-sprite / 34-sliver line limits (the `$213E` flags).
- **Screens**: the per-mode priority tables (mode 1's BG3 priority bit),
  main and sub screens, and windows 1 and 2 with their logic for each layer
  and for colour.
- **Colour math**: add or subtract, half, the fixed colour, subscreen
  backdrop rules, force-black and its windows.
- **Other effects**: mosaic and brightness. Forced blank shows black.
- **Hi-res**: modes 5 and 6 and pseudo-hires (`SETINI` bit 3) make
  512-pixel lines, the sub screen in the even pixels, with colour math as
  Mesen does it (each even pixel mixed with the main screen's previous one).
- **Interlace** (`SETINI` bit 0): 448 (or 478) rows, a field a frame. Modes
  5 and 6 then draw twice the BG rows, and OBJ interlace (bit 1) halves
  the sprites' height.
- **Overscan** (`SETINI` bit 2): 239 lines.
- **Picture size**: 256x224 normally, 512x448 for a frame with hi-res or
  interlace anywhere in it, and 239/478 lines with overscan.
- **Registers**: VRAM with the address remaps and prefetch, CGRAM, the OAM
  write latch, `$2134` multiply, the H/V latches (`$2137` and `WRIO`) and
  `$213C-$213F`.

**Not yet**: the PPU's per-dot fetches (a mid-line change to a BG's
scroll or tile base takes effect at the next pixel, not the next tile);
VRAM, OAM and CGRAM writes are allowed during the picture.

### Sound

- **SPC700** (`Spc700`, also a debugger CPU, `spc700`): every instruction
  and its cycle count. Checked against Tom Harte's SingleStepTests
  (all 254,000).
- **APU** (`snes::Apu`):
  - **Memory:** 64 KB of RAM and the 64-byte IPL boot ROM (embedded, as
    in Mesen and bsnes).
  - **Registers:** `$F0-$FF` — the ports both ways, the DSP address/data,
    and the three timers (8 kHz, 8 kHz, 64 kHz) with 4-bit counters.
  - **Clock:** runs at 1.024 MHz, brought up to the main CPU's time at
    every port access and every instruction.
- **S-DSP** (`snes::Dsp`):
  - **Pipeline:** the hardware's 32-step pipeline, one step per SPC700
    cycle (32 kHz stereo).
  - **Voices:** BRR decoding with its filters, ADSR and the four GAIN
    modes, Gaussian interpolation, pitch modulation, and noise.
  - **Echo:** the echo buffer with its 8-tap FIR, plus KON/KOFF/ENDX timing.
  - **Output:** mixed to mono and resampled to 44.1 kHz.

## Input

- **Gamepad** (`onGamepad`, the CLI/MCP `joy`, host controllers): all
  twelve buttons, ports 0 and 1.
- **Joystick** (`onJoystick`): the d-pad, with fire as B.
- **Keyboard** (pad 1): arrows, `X` (A), `Z` (B), `S` (X), `A` (Y), `Q`
  (L), `W` (R), Return (Start), Tab or Shift (Select).

## Tests

- `src/plugins/machines/snes/test/test_spc700.cpp`: the SPC700
  SingleStepTests (`../refs/ss-spc700/v1` or `MMEMU_SPC700_TESTS`; 300 per
  opcode, `MMEMU_SPC700_FULL=1` for all).
- `test_snes_machine.cpp`:
  - **Memory:** LoROM and work RAM, and HiROM with its battery RAM.
  - **Registers:** multiply/divide.
  - **Interrupts:** NMI once a frame.
  - **Sound:** the IPL handshake, a program uploaded through the IPL, and
    a DSP voice playing.
  - **Picture:** a BG1 tile on screen, and DMA to VRAM with HDMA changing
    colours mid-frame.
  - **Input:** the auto joypad read.
- `test_snes_roms.cpp`, with krom's ROMs
  ([PeterLemon/SNES](https://github.com/PeterLemon/SNES);
  `MMEMU_SNES_TEST_ROMS`, default `../refs/snes-krom`):
  - **Instruction tests:** the 65816 and SPC700 `CPUTest` ROMs and the
    Super FX's `CHIP/GSU/GSUTest` ROMs (31), read back from the screen
    (`MMEMU_SNES_SURVEY=1`; 585 PASS, 0 FAIL).
  - **Sound:** the Twinkle music demo has to make sound.
  - **Pictures:** `MMEMU_SNES_SHOTS=<dir>` runs every PPU demo and Super
    FX drawing demo for 3 seconds and saves a PNG of each, with the speed.
    24 of krom's 27 Super FX demo pictures match his reference PNGs pixel
    for pixel. The other three (PlotPixel at 192 lines) start 2 lines
    lower. Their HDMA table is the same as PlotLine's, which matches, so
    those three reference pictures look stale.
- `test_snes_cx4.cpp`: a hand-built Cx4 cartridge. A program is loaded
  through the cache, and the test reads back the registers, the data ROM,
  the data RAM, the multiplier and the STOP IRQ flag.
- `test_snes_sa1.cpp`: hand-built SA-1 cartridges. The S-CPU starts the
  SA-1, which multiplies and divides, writes BW-RAM (plain and as a
  bitmap), runs a DMA, reads a bit stream and messages back.
- `test_snes_gsu.cpp`: a hand-built Super FX cartridge. The 65816 starts a
  GSU program from work RAM and reads back its registers, its RAM, and the
  STOP IRQ.
- `test_snes_necdsp.cpp`: the uPD7725 with a small hand-assembled program
  (multiply, data ROM, RQM handshakes, ALU flags), on its own and through a
  LoROM cartridge driven by the 65816. It needs no Nintendo firmware, and it
  checks the error given when firmware is missing.
- `test_snes_machine.cpp` also checks the battery RAM's `.srm` (written,
  reloaded, saved on removal), and a mid-line write splitting a line,
  hi-res and interlace frame sizes and fields, and overscan.

Also seen running: all of krom's PPU demos (mode 7 rotate/zoom and
perspective, HDMA waves and gradients, windows, mosaic, 8bpp pictures, tile
flips, the H-IRQ palette pictures with 64/128 colours a tile row, pseudo-hires,
the six interlace pictures, mode 5 mosaic) and Tecmo's Monster Farm Jump
(menus with colour math, mode 7 stages with sprites).

## Not yet

- Cartridge coprocessors other than the DSP-1..4, Super FX, SA-1 and Cx4
  (S-DD1, SPC7110, ST010/011...).
- The multiply/divide delays, DMA/HDMA timing to the clock, open-bus
  details, and the SPC700's TEST register.
- PAL timing, the mouse, Super Scope and multitap.
