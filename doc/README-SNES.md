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

- **Timing**: a line is 1364 master clocks (341 dots) and a frame 262 lines.
  The DRAM refresh steals 40 clocks a line at clock 538. Each visible line
  (1-224) is drawn at the start of horizontal blank (dot 276), then HDMA
  runs for the next. Vertical blank starts at line 225: the NMI flag
  (`$4210`), the auto joypad read (`$4218-$421F`, 3 lines busy), and the
  OAM address reload. HDMA restarts at line 0.
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
  - **Coprocessors:** none yet (no DSP-1, Super FX or SA-1).

### PPU

Drawn a line at a time:

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
- **Registers**: VRAM with the address remaps and prefetch, CGRAM, the OAM
  write latch, `$2134` multiply, the H/V latches and `$213C-$213F`.

**Not yet**: hi-res modes 5/6 and pseudo-hires show only the main screen's
pixels (256 wide), there is no interlace and no 239-line overscan, and
register writes take effect for whole lines.

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
  - **Instruction tests:** the 65816 and SPC700 `CPUTest` ROMs, read back
    from the screen (`MMEMU_SNES_SURVEY=1`; 127 PASS, 0 FAIL).
  - **Sound:** the Twinkle music demo has to make sound.

Also seen running: krom's PPU demos (mode 7 rotate/zoom and perspective,
HDMA waves and gradients, windows, mosaic, 8bpp pictures, tile flips, the
mode 5 hi-res picture) and Tecmo's Monster Farm Jump (menus with colour
math, mode 7 stages with sprites).

## Not yet

- Cartridge coprocessors (DSP-1..4, Super FX, SA-1, S-DD1, Cx4, SPC7110...).
- True hi-res (512-pixel) output, interlace, overscan, mid-line raster effects.
- The multiply/divide delays, DMA/HDMA timing to the clock, open-bus
  details, and the SPC700's TEST register.
- PAL timing, battery RAM saving, the mouse, Super Scope and multitap.
