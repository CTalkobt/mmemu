# mmemu-plugin-nes: Nintendo Entertainment System / Famicom

The NES and Famicom (#14), after Mesen2 (GPL-3.0, Sour and the Mesen
contributors): the PPU (`Core/NES/NesPpu.cpp`, `BaseNesPpu.cpp`,
`DefaultNesPpu.h`), the APU (`Core/NES/APU/`), the mappers
(`Core/NES/Mappers/Nintendo/`, `A12Watcher.h`), the header
(`NesHeader.cpp`) and the console (`NesConsole.cpp`, `NesCpu.cpp`,
`NesMemoryManager.cpp`), with the nesdev wiki for the details.

## Profiles

| Profile | Machine |
|---------|---------|
| `nes` | NTSC NES: CPU 1789773 Hz, PPU 3 dots a CPU cycle, 262 lines |
| `famicom` | The Famicom: the same machine here |

There is no ROM in the console: plug a cartridge in (`cart game.nes` in the
CLI, File > Attach Cartridge in the GUI, MCP `attach_cartridge`). Inserting
a cartridge powers the console on; the machine's reset is the RESET button.

## Hardware

- **CPU**: the Ricoh 2A03, an NMOS 6502 (`MOS6502` with
  `setDecimalDisabled`) without decimal mode: D sets and clears, ADC/SBC
  stay binary. The unstable LXA/XAA use the magic constant `$FF`. IRQ
  polling has the 6502's latency after CLI/SEI/PLP (`setIrqPollLatency`).
- **Memory map**:

| Address | |
|---------|---|
| `$0000-$1FFF` | 2 KB of RAM, mirrored |
| `$2000-$3FFF` | the PPU's 8 registers, mirrored |
| `$4000-$4013`, `$4015`, `$4017` | the APU (`$4017` write: the frame counter) |
| `$4014` | OAM DMA |
| `$4016` / `$4017` | controller strobe (write), controllers 1 / 2 (read) |
| `$4020-$FFFF` | the cartridge: work RAM at `$6000`, PRG ROM at `$8000` |

- **Timing**: as for the Atari 2600, the PPU and APU are clocked lazily.
  Reads bring them up to the cycle of the access; writes to them and to the
  cartridge are held and applied on the instruction's last cycles. NMI and
  IRQ are sampled before an instruction's last cycle, as the 6502 polls them.
  The reset sequence's 7 cycles run before the first instruction.
- **OAM DMA**: 256 read/write pairs through `$2004`, stalling the CPU 513
  cycles (514 on an odd cycle).
- **Controllers**: two standard pads (a 4021 shift register each: A, B,
  Select, Start, Up, Down, Left, Right, then 1s).
- **Reset**: the RESET button keeps RAM, the cartridge and the CPU's A/X/Y.
  S drops by 3 and I is set. The PPU clears `$2000`/`$2001`. The APU clears
  `$4015` (the triangle's length counter stays) and rewrites `$4017`'s mode.

### PPU (2C02)

Dot by dot, 341 dots a line, 262 lines:

- **Background**: the nametable/attribute/pattern fetches every 8 dots into
  the 16-bit shift registers, fine X, the loopy `v`/`t`/`x`/`w` scroll
  registers and their increments and copies (dots 256, 257, 280-304).
- **Sprites**: up to 8 a line, 8x8 or 8x16. Secondary OAM is evaluated for
  the next line, with the overflow flag's diagonal-scan bug, and fetched at
  dots 257-320 (unused slots fetch tile `$FF`, which MMC3 counts on). Sprite
  0 hit (not at x = 255, not under the left-8 masks), priority, and the left
  8-pixel masks.
- **Registers**: `$2002`'s vblank race (a read on the dot before suppresses
  the flag and NMI), `$2004` reads during rendering, the `$2007` read buffer
  (palette reads immediate), `$2006`'s 3-dot delay to `v`, the `$2007`
  increment a dot later (during rendering: coarse X and Y), OAM writes
  during rendering ignored, open bus with per-bit decay after 3 frames.
- **Frame**: vblank at line 241 dot 1, cleared at the pre-render line, odd
  frames one dot shorter with rendering on, 256x240 double-buffered.
- **Colour**: the 2C02 palette (Mesen's default), grayscale, emphasis bits.

### APU

Clocked every CPU cycle: two pulse channels (duty, envelope, sweep with
pulse 1's ones' complement negate), the triangle (linear counter), noise
(both LFSR modes), the DMC (sample fetches through the CPU's bus, a 4-cycle
stall each, loop and IRQ), length counters, and the frame counter in 4- and
5-step modes with its IRQ and the `$4017` write delay. Mixed with the nesdev
nonlinear formulas, averaged to 44100 Hz and DC-blocked.

### Cartridges

iNES and NES 2.0 images (`.nes`). Trainers load at `$7000`. Battery RAM is
not saved yet.

| Mapper | Boards | |
|--------|--------|---|
| 0 | NROM | 16/32 KB PRG, 8 KB CHR |
| 1 | MMC1 (SxROM) | serial port (writes on consecutive cycles ignored), PRG modes, 4/8 KB CHR, mirroring, SUROM's 512 KB PRG |
| 2 | UxROM | 16 KB switchable + last fixed |
| 3 | CNROM | 8 KB CHR switching |
| 4 | MMC3 (TxROM) | 8 KB PRG / 1-2 KB CHR banks, both modes, mirroring, work RAM protect, the scanline IRQ (A12 rises after A12 has been low more than 10 dots) |
| 7 | AxROM | 32 KB PRG, single-screen nametables |

## Input

- **Gamepad** (`onGamepad`, the CLI/MCP `gamepad`, host controllers):
  `PAD_A`/`PAD_X` press A, `PAD_B`/`PAD_Y` press B, plus Select, Start and
  the d-pad. Ports 0 and 1.
- **Joystick** (`onJoystick`): the d-pad, with fire as A.
- **Keyboard** (controller 1): arrows, `X` (A), `Z` (B), Return (Start),
  Tab or Shift (Select).

## Tests

- `src/plugins/machines/nes/test/test_nes_machine.cpp`: the header, RAM
  mirroring, binary ADC, vblank NMI, `$2006/$2007` and mirroring,
  background rendering, sprite 0 hit, OAM DMA's stall, the controller, APU
  length counters and frame IRQ, audio output, MMC1, UxROM, CNROM, AxROM,
  and MMC3's scanline IRQ.
- `test_nes_roms.cpp`: a survey of the
  [nes-test-roms](https://github.com/christopherpow/nes-test-roms)
  collection (blargg's `$6000` protocol). Set `MMEMU_NES_TEST_ROMS` (default
  `../refs/nes-test-roms`) and `MMEMU_NES_SURVEY=1`, or a path substring to
  pick ROMs.

They pass `instr_test-v3` and `-v5` (all), `instr_timing`, `instr_misc`
1-2, `apu_test` (all), `apu_reset` (4 of 6), `cpu_reset`, `ppu_vbl_nmi`
01-04 and 09-10, `ppu_open_bus`, `oam_read`, `oam_stress`, `mmc3_test_2`
1-3 and 5, `cpu_interrupts_v2` 1, and `cpu_exec_space_apu`.

## Not yet

- **6502 bus cycles**: the core does not make the 6502's dummy reads
  (indexed page crossings) or its read-modify-write double writes.
  `instr_misc` 3-4, `cpu_dummy_writes`, `cpu_dummy_reads` and MMC1 games
  that reset with `INC` (Bill & Ted) need them.
- **NMI/IRQ timing to the cycle**: `ppu_vbl_nmi` 05-08 and
  `cpu_interrupts_v2` 2-5 are off by about a cycle.
- **DMA interplay**: OAM DMA and DMC DMA together, and DMC DMA during
  reads of `$2007` and `$4016`.
- **MMC3**: `4-scanline_timing` is a dot or so out, and `6-MMC3_alt` tests
  the older revision's IRQ behaviour.
- **Other**: PAL and Dendy, more mappers (MMC2/4/5, VRC, FDS...),
  expansion audio, the Zapper and other controllers, battery saves.
