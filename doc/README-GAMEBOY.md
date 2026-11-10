# Game Boy core (`gb::Gameboy`)

A monochrome Game Boy (DMG), written for the SNES's Super Game Boy (#15)
after Mesen2's `Core/Gameboy` (GPL-3.0, Sour and the Mesen contributors).
It lives in `src/plugins/machines/gameboy/main/` and is linked into the
SNES plugin; there is no standalone Game Boy machine yet.

## What's emulated

- **CPU** (`gb_cpu.cpp`, the SM83): every opcode, timed per machine cycle
  as Mesen (the cycle runs before its access), interrupt dispatch that
  reads IE/IF again between the two pushes (`ie_push`), EI's one-instruction
  delay, HALT (and the HALT bug, and the DMG's later interrupt check while
  halted), STOP until a button is pressed, and the illegal opcodes locking
  the CPU up.
- **Memory map** (`gameboy.cpp`, Mesen's `GbMemoryManager`): 256 pages of
  256 bytes, each memory (ROM, work RAM, cartridge RAM, boot ROM) or
  registers. VRAM is blocked to the CPU while the PPU reads it, OAM during
  OAM search and drawing.
- **Timer and DIV**: the 16-bit divider, TIMA ticking on the falling edge of
  the selected bit (including the glitches when TAC or DIV are written), the
  reload a cycle late and the writes on that cycle.
- **OAM DMA**: a byte a machine cycle after a cycle's delay, the bus
  conflicts (the CPU reads what DMA reads), paused while the CPU is halted.
- **PPU** (`gb_ppu.cpp`): a dot at a time; OAM search, the background and
  window fetcher with its pixel FIFO, sprite fetches stalling it, sprite
  priority, the window's line counter and its DMG glitch, the STAT
  interrupt timing (line 153, the DMG's STAT write bug, LYC), BGP written
  mid-line, the OAM corruption bug, and the first frame after the LCD is
  turned on left blank. The output is a 160x144 shade per pixel (0 white to
  3 black).
- **APU** (`gb_apu.cpp`): two square channels (sweep on the first), the wave
  channel (DMG wave RAM access only on the cycle it is read, retrigger
  corruption), the noise channel, the frame sequencer from DIV, length
  counters with the extra clocking, the envelope's "zombie mode", the DACs'
  fade. The output is Mesen's level, box-filtered to a chosen rate.
- **Joypad and serial port**: the select lines and the joypad interrupt on
  any change of what `$FF00` reads; the serial port with the internal clock
  (a bit every 512 clocks, `$FF` shifted in); the bytes sent are kept
  (`serialOutput()`, how Blargg's tests print).
- **Cartridges** (`gb_cart.cpp`): ROM only, MBC1 (and the MBC1M multicart
  wiring), MBC2 (512 nibbles), MBC3 (with the clock, `.rtc` as Mesen; and
  MBC30's 8 RAM banks) and MBC5 (with rumble's RAM bank bits). Battery RAM
  is the caller's to save (`cartRam()`).

## Boot

Without a boot ROM the CPU starts at `$0100` with the registers and I/O the
DMG boot ROM leaves (Pan Docs' table, the chime playing). As the Super Game
Boy's Game Boy it runs SameBoy's open source SGB/SGB2 boot ROM (LIJI32's,
MIT licence, the one Mesen embeds), which sends the cartridge header to the
SNES in joypad packets as Nintendo's does. `setBootRom()` takes another.
Nintendo's boot ROMs are not included.

## Tests

- `src/plugins/machines/gameboy/test/test_gameboy.cpp`: hand-assembled
  cartridges: the post-boot state, instruction timing and the ALU, the
  timer interrupt with HALT, OAM DMA from HRAM, LY and the modes, MBC1 and
  MBC5 banking with RAM, the joypad and its interrupt, the serial port,
  the APU's samples.
- `test_gameboy_roms.cpp`, with c-sp's
  [gameboy-test-roms](https://github.com/c-sp/gameboy-test-roms) release
  (`MMEMU_GB_TEST_ROMS`, default `../refs/gameboy-test-roms`; skipped when
  absent): `gb_blargg_tests` runs Blargg's `cpu_instrs`, `instr_timing`
  and `mem_timing` and Matt Currie's `dmg-acid2` against its reference
  picture, all passing. `MMEMU_GB_SURVEY=1` runs `gb_rom_survey`: the rest
  of Blargg's DMG tests (`cpu_instrs`, `mem_timing`, `mem_timing-2`,
  `dmg_sound` and `oam_bug` singles, `halt_bug`) and Mooneye's acceptance
  tests for the DMG, plus the SGB ones with the SGB boot ROM: 102 PASS,
  6 FAIL. The failures: `boot_div` and `boot_hwio` (DMG and SGB) and
  `boot_sclk_align`, which check the exact state and timing Nintendo's boot
  ROM leaves (the post-boot state here, SameBoy's boot ROM on the SGB),
  and `oam_bug` 7-timing_effect. `MMEMU_GB_SHOW=1` lists the passes too.

## Not yet

- The Game Boy Color (and so CGB-only cartridges), the MBC6/MBC7, HuC1/HuC3,
  MMM01 and the other rare mappers, the infrared port.
- A standalone Game Boy machine (CLI/GUI), and a debugger for the SM83.
