# Game Boy (`gameboy`) and its core (`gb::Gameboy`)

A monochrome Game Boy (DMG), written for the SNES's Super Game Boy (#15)
after Mesen2's `Core/Gameboy` (GPL-3.0, Sour and the Mesen contributors).
It lives in `src/plugins/machines/gameboy/main/`; its objects are linked
into both the Game Boy plugin (`lib/mmemu-plugin-gameboy.so`, the
standalone machine) and the SNES plugin (the Super Game Boy).

## Running it

```
bin/mmemu-cli -m gameboy            # or -m dmg
> cart path/to/game.gb              # also the GUI's Attach Cartridge, MCP attach_cartridge
> run
bin/mmemu-gui -m gameboy
```

`.gb` and `.gbc` files attach (a Game Boy Color cartridge runs only if it
also works on the DMG). On a SNES the same files go into the Super Game Boy
instead. With no cartridge the bus reads `$FF` and the CPU runs `RST $38`,
as on the real machine.

- **Screen**: 160x144, the four shades in the DMG's greens.
- **Sound**: the APU's stereo output mixed to mono, 44.1 kHz.
- **Joypad**: keys arrows, **X** (A), **Z** (B), **Return** (Start),
  **Tab** or **Shift** (Select); a host gamepad's d-pad, A/X (A), B/Y (B),
  START and SELECT; a joystick's directions and fire (A). START and SELECT
  are also buttons under the GUI's screen.
- **Battery saves**: a battery-backed cartridge's RAM is kept in
  `<rom>.sav` beside the ROM (read from `<rom>.srm`, Mesen's name, when
  there is no `.sav`), written within a second of a change, on eject and
  on exit. The MBC3's clock goes in `<rom>.rtc` (Mesen's format: its
  registers and the host time, so the time away passes).
- **Boot ROM**: none is needed (the machine starts in the state the DMG's
  leaves). Your own `roms/gameboy/dmg_boot.bin` (256 bytes; not included,
  `MMEMU_GB_FIRMWARE` names another directory) is run if present.
- **Reset**: the Game Boy has no reset button; `reset` powers it off and on
  (the cartridge's RAM stays).

## Debugging

The SM83 is the machine's CPU for the debugger (`sm83_core.h`, ISA
`sm83`), so the CLI, GUI and MCP tools work as on the other machines:

- **Registers**: `A F B C D E H L SP PC`, F's flags shown as `ZNHC`; the
  pairs `AF BC DE HL` and `IME` are also readable and writable by name
  (expressions, breakpoint conditions: `HL == $C000`, `.Z`).
- **Step / run**: a step is one instruction, an interrupt's dispatch (a
  step of its own, so a breakpoint on `$0040` stops before the VBlank
  handler's first instruction with the return address pushed), or one
  machine cycle while halted or stopped. `isProgramEnd` is a locked CPU
  (halted with no interrupt enabled, as after an unused opcode).
- **Breakpoints, watchpoints, trace**: execution breakpoints, and read and
  write watchpoints on the CPU's data accesses (loads, stores, pushes and
  pops; not opcode fetches); the trace keeps each instruction's registers.
- **Memory**: the 64 KB map as the CPU sees it. Reads have no side effects
  (VRAM and OAM are shown whatever the PPU is doing); writes go to memory
  or the register (`$0000-$7FFF` switches banks as a CPU write would).
- **Disassembly** (`sm83_disasm.h`, also the `sm83` toolchain
  disassembler): every opcode and the CB-prefixed ones, `LD (HL+),A`,
  `LDH ($FF44),A`, `LD ($FF00+C),A`, `ADD SP,-$02`, `LD HL,SP+$05`, JR's
  target address; the unused opcodes as `DB $xx`. There is no SM83
  assembler.
- **Symbols**: the hardware registers (`P1`, `DIV`, `LCDC`, `STAT`, `LY`,
  `BGP`, `NR10`-`NR52`, `IE`, ...) from `roms/gameboy/hardware.sym`, used
  in the disassembly (`LDH (LY),A`) and expressions.

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

- `test_gameboy_machine.cpp`: the machine with hand-built cartridges: the
  registers and steps through `ICore`, a breakpoint on the VBlank handler,
  watchpoints, the lock-up, the screen, sound and joypad, battery saves
  and eject, and Blargg's `06-ld r,r` run through the scheduler with an
  observer attached. `test_sm83_disasm.cpp`: a table of opcodes and their
  text, every opcode's length and the call/return/branch flags, symbols.

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
- An SM83 assembler; reverse stepping and snapshots keep only the CPU's
  registers (not memory or the PPU); the GDB server does not know the SM83.
