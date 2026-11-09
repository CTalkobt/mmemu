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

- **CPU**: the Ricoh 2A03 (`nes::Cpu`, `nes_cpu.cpp`, after Mesen's
  `NesCpu`), an NMOS 6502 without decimal mode (D sets and clears, ADC/SBC
  stay binary), cycle by cycle:
  - **Bus cycles:** every cycle is a bus access, as on the chip: the dummy
    reads of indexed and implied addressing, the read-modify-write
    instructions' double writes, the stack's dummy reads.
  - **Interrupts:** NMI's edge and IRQ's level are sampled at the end of each
    cycle; an interrupt follows the instruction whose second-to-last cycle
    saw it. That gives CLI/SEI/PLP their latency, a taken branch's IRQ
    delay, and NMI hijacking BRK.
  - **Debugger:** registers, disassembly and the debugger come from the 6502
    core (`MOS6502`), which the 2A03 extends.
- **Memory map**:

| Address | |
|---------|---|
| `$0000-$1FFF` | 2 KB of RAM, mirrored |
| `$2000-$3FFF` | the PPU's 8 registers, mirrored |
| `$4000-$4013`, `$4015`, `$4017` | the APU (`$4017` write: the frame counter) |
| `$4014` | OAM DMA |
| `$4016` / `$4017` | controller strobe (write), controllers 1 / 2 (read) |
| `$4020-$FFFF` | the cartridge: work RAM at `$6000`, PRG ROM at `$8000` |

- **Timing**: a master clock of 12 a CPU cycle and 4 a PPU dot. Each access
  falls after two of the cycle's three dots (Mesen's alignment), and the APU
  is clocked at the start of each cycle. Power-on and RESET run 8 cycles
  before the first instruction.
- **DMA**: both halt the CPU on its next read cycle and take its cycles
  there.
  - **OAM DMA:** 256 read/write pairs through `$2004`, 513 or 514 cycles.
  - **The DMC's fetches:** a halt cycle, a dummy cycle, then the read on a
    "get" cycle. They share cycles with OAM DMA, and enabling or disabling
    the channel takes effect after Mesen's delays.
  - **Side effects:** the halted CPU's repeated reads, which double-clock
    `$2007` and the pads. The DMA unit's reads also reach `$4015`-`$4017`
    when the CPU was halted on one.
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
  the next line, with the overflow flag's diagonal-scan bug and set on the
  dot the hardware's evaluation reaches the 9th sprite, and fetched at
  dots 257-320 (unused slots fetch tile `$FF`, which MMC3 counts on). Sprite
  0 hit (not at x = 255, not under the left-8 masks), priority, and the left
  8-pixel masks.
- **Registers**: `$2002`'s vblank race (a read on the dot before suppresses
  the flag and NMI), `$2004` reads during rendering, the `$2007` read buffer
  (palette reads immediate; a second read within 6 dots is ignored),
  `$2006`'s 3-dot delay to `v`, the `$2007`
  increment a dot later (during rendering: coarse X and Y), OAM writes
  during rendering ignored, open bus with per-bit decay after 3 frames.
- **Frame**: vblank at line 241 dot 1, cleared at the pre-render line, odd
  frames one dot shorter with rendering on, 256x240 double-buffered.
- **Colour**: the 2C02 palette (Mesen's default), grayscale, emphasis bits.

### APU

Clocked every CPU cycle: two pulse channels (duty, envelope, sweep with
pulse 1's ones' complement negate), the triangle (linear counter), noise
(both LFSR modes), the DMC (sample fetches by the CPU's DMA, loop and IRQ),
length counters (halt and reload writes take effect after the cycle's frame
counter step), and the frame counter in 4- and 5-step modes with its IRQ and
the `$4017` write delay (3 or 4 cycles by the CPU cycle's parity). Mixed with the nesdev
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
  background rendering, sprite 0 hit, OAM DMA's cycles, the controller, APU
  length counters and frame IRQ, audio output, MMC1, UxROM, CNROM, AxROM,
  and MMC3's scanline IRQ.
- `test_nes_roms.cpp`: a survey of the
  [nes-test-roms](https://github.com/christopherpow/nes-test-roms)
  collection. Set `MMEMU_NES_TEST_ROMS` (default `../refs/nes-test-roms`)
  and `MMEMU_NES_SURVEY=1`, or a path substring to pick ROMs; about 10
  minutes for all. It reads results in three ways:
  - **blargg's `$6000` protocol:** pressing RESET when a test asks.
  - **The screen:** the first nametable as text ("Passed", "Failed", the
    2005 tests' `$01`, `cpu_test5`'s failing opcodes).
  - **CRCs:** the values the source lists for the tests that print only one.

All 158 ROMs with a verdict pass:
- **CPU:** `instr_test-v3`/`-v5`, `instr_timing`, `instr_misc`,
  `nes_instr_test`, `blargg_nes_cpu_test5`, `cpu_timing_test6`,
  `branch_timing_tests`, `cpu_interrupts_v2`, `cpu_dummy_reads`/`_writes`,
  `cpu_exec_space`, `cpu_reset`.
- **APU:** `apu_test`, `apu_reset`, `blargg_apu_2005.07.30`.
- **PPU:** `ppu_vbl_nmi`, `vbl_nmi_timing`, `ppu_open_bus`,
  `ppu_read_buffer`, `blargg_ppu_tests_2005.09.15b`, `oam_read`,
  `oam_stress`, `sprite_hit_tests_2005.10.05`, `sprite_overflow_tests`.
- **DMA:** `sprdma_and_dmc_dma`, `dmc_dma_during_read4`.
- **MMC3:** `mmc3_test`, `mmc3_test_2`, `mmc3_irq_tests`.

The three MMC3 tests of other chip revisions (`6-MMC3_alt`, `6-MMC6`,
`5.MMC3_rev_A`) fail, as they do on an MMC3B. `read_joy3`'s
`count_errors` and `test_buttons` give no verdict.

## Not yet

- **Revisions:** the CPU/PPU alignment is fixed (Mesen's default), not
  random, and there is one MMC3 (rev B).
- **Other**: PAL and Dendy, more mappers (MMC2/4/5, VRC, FDS...),
  expansion audio, the Zapper and other controllers, battery saves.
