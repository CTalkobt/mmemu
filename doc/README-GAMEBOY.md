# Game Boy (`gameboy`), Game Boy Color (`gameboycolor`) and their core (`gb::Gameboy`)

A monochrome Game Boy (DMG), written for the SNES's Super Game Boy (#15)
after Mesen2's `Core/Gameboy` (GPL-3.0, Sour and the Mesen contributors),
and the Game Boy Color (CGB) on the same core, as Mesen has it.
It lives in `src/plugins/machines/gameboy/main/`; its objects are linked
into both the Game Boy plugin (`lib/mmemu-plugin-gameboy.so`, the
standalone machine) and the SNES plugin (the Super Game Boy).

## Running it

```
bin/mmemu-cli -m gameboy            # or -m dmg
> cart path/to/game.gb              # also the GUI's Attach Cartridge, MCP attach_cartridge
> run
bin/mmemu-gui -m gameboy
bin/mmemu-gui -m gameboycolor       # or -m gbc, -m cgb
```

`.gb` and `.gbc` files attach to either. On the `gameboy` (a DMG) a Game
Boy Color cartridge runs only if it also works on the DMG; on the
`gameboycolor` a CGB cartridge (header `$0143` bit 7 set) runs in CGB mode
and a DMG one in the CGB's DMG compatibility mode, coloured by the boot ROM
as on the real machine. On a SNES the same files go into the Super Game Boy
instead. With no cartridge the bus reads `$FF` and the CPU runs `RST $38`,
as on the real machine.

- **Screen**: 160x144, the four shades in the DMG's greens; the Game Boy
  Color's 15-bit colours, each 5-bit channel X as `(X << 3) | (X >> 2)`
  (the conversion c-sp's test pictures use), with no LCD colour
  correction.
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
- **Boot ROM**: none is needed on the DMG (the machine starts in the state
  the DMG's leaves). Your own `roms/gameboy/dmg_boot.bin` (256 bytes; not
  included, `MMEMU_GB_FIRMWARE` names another directory) is run if present.
  The Game Boy Color runs SameBoy's open source CGB boot ROM (built in, as
  Mesen; about 0.23 s to `$0100`), or your own `roms/gameboy/cgb_boot.bin`
  (2304 bytes, `$0000-$08FF` with the header's gap) if present.
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
  or the register (`$0000-$7FFF` switches banks as a CPU write would). On
  the Game Boy Color VRAM and the work RAM at `$D000` are the banks VBK
  (`$FF4F`) and SVBK (`$FF70`) select: write those to see another bank
  (the debugger has no separate bank views; `gb::Gameboy::workRam()` and
  `Ppu::vram()` hold all of them).
- **Disassembly** (`sm83_disasm.h`, also the `sm83` toolchain
  disassembler): every opcode and the CB-prefixed ones, `LD (HL+),A`,
  `LDH ($FF44),A`, `LD ($FF00+C),A`, `ADD SP,-$02`, `LD HL,SP+$05`, JR's
  target address; the unused opcodes as `DB $xx`.
- **Assembly** (`sm83_asm.h`, the `sm83` toolchain assembler: the CLI's
  `asm $C000`, ending with `.`, the GUI's and MCP's): a line at a time,
  reading back everything the disassembler writes, and `LDI`/`LDD`,
  `(HLI)`/`(HLD)`, `(C)`, `LDH ($44),A`, ALU operands with or without
  `A,`, `JP (HL)`, `STOP $xx`, `DB`/`DW`; numbers as `$FF`, `0xFF`, `0FFh`,
  `%1010` or decimal. No labels or files (as the 6502's line assembler).
- **GDB** (`--gdb-port`): the `g` packet is AF, BC, DE, HL, SP, PC, 16 bits
  each, little-endian (the first six of GDB's Z80 order), with `p`/`P` and
  a target description (`qXfer:features:read:target.xml`); `c` stops at
  breakpoints, not at `$00` (that is the 6502's BRK; here NOP).
- **Symbols**: the hardware registers (`P1`, `DIV`, `LCDC`, `STAT`, `LY`,
  `BGP`, `NR10`-`NR52`, `IE`, ...) from `roms/gameboy/hardware.sym`, used
  in the disassembly (`LDH (LY),A`) and expressions.

## What's emulated

- **CPU** (`gb_cpu.cpp`, the SM83): every opcode, timed per machine cycle
  as Mesen (the cycle runs before its access), interrupt dispatch that
  reads IE/IF again between the two pushes (`ie_push`), EI's one-instruction
  delay, HALT (and the HALT bug, and the DMG's later interrupt check while
  halted), STOP until a button is pressed, and the illegal opcodes locking
  the CPU up. As SameBoy where the test ROMs differ from Mesen: HALT looks
  at the interrupts a cycle in and, with one pending and IME set, doesn't
  halt (the interrupt returns to it); the CGB's speed switch resets DIV once
  and pauses 32,769 machine cycles with the timer, PPU and APU running.
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
  corruption), the noise channel (SameBoy's: a 14-bit counter at the
  divisor's rate whose selected bit clocks the LFSR, with its trigger
  alignment and NR43 write glitches), the frame sequencer from DIV, length
  counters with the extra clocking, the envelope's "zombie mode", the DACs'
  fade. The output is Mesen's level, box-filtered to a chosen rate.
- **Joypad and serial port**: the select lines and the joypad interrupt on
  any change of what `$FF00` reads; the serial port with the internal clock
  (a bit every 512 clocks, `$FF` shifted in); the bytes sent are kept
  (`serialOutput()`, how Blargg's tests print).
- **Cartridges** (`gb_cart.cpp`): ROM only, MBC1 (and the MBC1M multicart
  wiring), MBC2 (512 nibbles), MBC3 (with the clock, `.rtc` as Mesen; and
  MBC30's 8 RAM banks) and MBC5 (with rumble's RAM bank bits). Battery RAM
  is the caller's to save (`cartRam()`). The MBC3's clock runs in real time
  (the APU's clock, which double speed doesn't change).

## The Game Boy Color

`Gameboy::setModel(Model::Cgb)`, the `gameboycolor` machine. Each part
follows Mesen's CGB handling:

- **Double speed**: KEY1 (`$FF4D`) arms it, STOP switches: DIV resets and
  the CPU halts 32,769 machine cycles while the timer, PPU and APU run on
  (SameBoy's; an interrupt ends it), then runs twice as fast. The timer,
  OAM DMA and the serial port run with the CPU; the PPU takes a dot per
  CPU half-cycle and the APU keeps its clock (its frame sequencer then off
  DIV bit 13). Each switch moves the LCD half a dot from the CPU: unseen
  in double speed, back at normal speed the PPU is a dot on (AGE's
  `lcd-align-ly`, `spsw-mode0`). `cycles()` counts the CPU's clocks, `apuCycles()` real time
  (the machine paces on the latter).
- **VRAM bank 1** (VBK `$FF4F`), and the background's attribute map in it:
  each tile's palette (0-7), VRAM bank, horizontal and vertical flips and
  priority over objects.
- **Work RAM banks 1-7** at `$D000` (SVBK `$FF70`; 0 selects 1).
- **Colour palettes**: BCPS/BCPD and OCPS/OCPD (`$FF68-$FF6B`), 8 of 4
  colours each for the background and objects, with auto-increment; the
  data registers read `$FF` and ignore writes during mode 3 (the index still
  moves on).
- **Objects**: their palette (attribute bits 0-2) and VRAM bank (bit 3);
  where objects overlap the one earlier in OAM wins (not the lower X);
  LCDC bit 0 cleared puts every object over the background (the master
  priority) instead of blanking it. Objects are fetched even with LCDC
  bit 1 clear (and dropped), as Mesen.
- **HDMA** (`$FF51-$FF55`): general DMA (4 clocks, then 16 bytes per block
  at 2 clocks a byte, 4 in double speed, with the CPU stopped) and HBlank
  DMA (a block on entering each HBlank, one at once if started in HBlank or
  with the LCD off; none while the CPU is halted; stopped by writing bit 7
  clear). Source and destination keep counting; a source in VRAM or at
  `$E000` up reads `$FF`.
- **The PPU's CGB timings and quirks**: no OAM corruption bug, no STAT
  write bug, no DMG window glitch, line 0 not starting in mode 0, BGP's
  mid-line change taking the new value only, the tile fetch glitch when
  LCDC bit 4 is cleared as the fetcher reads tile data (`cgb-acid-hell`),
  the first frame after the LCD is turned on blanked only if it was off
  for a while, and STOP freezing what the LCD can read (Mesen's).
- **OAM DMA**: the CGB's bus conflicts (work RAM has its own bus).
- **The APU**: wave RAM reached any time while the wave channel plays (the
  byte being played) and no retrigger corruption; powering the APU on
  clears the length counters; PCM12/PCM34 (`$FF76/$FF77`) read the
  channels' digital outputs.
- **Other registers**: the undocumented `$FF72-$FF75`, RP (`$FF56`, the
  infrared port: a stub as Mesen's, nothing ever received), SC's fast
  clock bit (kept but not run faster, as Mesen), KEY0 (`$FF4C`, the boot
  ROM's only). OPRI (`$FF6C`) is not implemented, as in Mesen: it reads
  `$FF`, and CGB mode always orders objects by OAM index.
- **DMG compatibility mode**: a DMG cartridge's header makes the boot ROM
  write KEY0 `$04`: the CGB's registers then read `$FF` and ignore writes,
  and the DMG palettes (BGP, OBP0/1) pick colours from colour palettes 0
  (background) and 0/1 (objects), which the boot ROM fills by the title as
  Nintendo's does.
- **Without a boot ROM** (`setSkipBootRom(true)`, used by the unit tests;
  Mesen always runs one) the CPU starts at `$0100` with Pan Docs' CGB
  registers (`A=$11`; `DE=$FF56 HL=$000D` for a CGB cartridge, `DE=$0008
  HL=$007C` for a DMG one), white background palettes for a CGB cartridge,
  and for a DMG one the compatibility mode with the colours Nintendo's boot
  ROM gives a title it has no palette for (c-sp's: background `#FFFFFF
  #7BFF31 #0063C6 #000000`, objects `#FFFFFF #FF8484 #943939 #000000`).

## Boot

Without a boot ROM the CPU starts at `$0100` with the registers and I/O the
DMG boot ROM leaves (Pan Docs' table, the chime playing), down to the
unused bits Mooneye's `boot_hwio-dmgABCmgb` reads (P1 `$CF` with both
select lines low, IF `$E1`), the divider's internal counter at `$ABCA`
(`boot_div-dmgABCmgb`'s value and phase) and the serial clock in step with
it (`boot_sclk_align`). As the Super Game
Boy's Game Boy it runs SameBoy's open source SGB/SGB2 boot ROM (LIJI32's,
MIT licence, the one Mesen embeds), which sends the cartridge header to the
SNES in joypad packets as Nintendo's does. Nintendo's takes a machine cycle
less for each 1 bit of that header, SameBoy's the same time for either, so
when it hands over DIV is moved by the difference (`boot_div-S`,
`boot_div2-S`). `setBootRom()` takes another; the SNES machine passes
`roms/snes/sgb_boot.bin` (`sgb2_boot.bin`, or `sgb_bios.bin`) if present
(see [README-SNES.md](README-SNES.md)).
On the Game Boy Color it runs SameBoy's CGB boot ROM (the one Mesen
embeds): the logo, then for a DMG cartridge KEY0 and the palettes by its
title. Nintendo's boot ROMs are not included.

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
  MBC5 banking with RAM, the joypad and its interrupt (on a falling
  P10-P13 only, as Pan Docs: not on a select line's change), the serial port,
  the APU's samples.
- `test_gameboy_roms.cpp`, with c-sp's
  [gameboy-test-roms](https://github.com/c-sp/gameboy-test-roms) release
  (`MMEMU_GB_TEST_ROMS`, default `../refs/gameboy-test-roms`; skipped when
  absent): `gb_blargg_tests` runs Blargg's `cpu_instrs`, `instr_timing`
  and `mem_timing` and Matt Currie's `dmg-acid2` against its reference
  picture, all passing. `MMEMU_GB_SURVEY=1` runs `gb_rom_survey`: the rest
  of Blargg's DMG tests (`cpu_instrs`, `mem_timing`, `mem_timing-2`,
  `dmg_sound` and `oam_bug` singles, `halt_bug`) and Mooneye's acceptance
  tests for the DMG, plus the SGB ones with the SGB boot ROM, the
  combined `oam_bug.gb` against its picture, and AGE's tests verified on a
  DMG C (`dmgC` in the name): 119 PASS, 1 FAIL. The 1 is AGE's
  `oam-write-dmgC`: its starred row, the writes 2 machine cycles in, lands
  on the mode edges' boundary and changes from line to line and with when
  the LCD was last turned off, its author notes.
  `gb_blargg_tests` also runs `cgb-acid2` on the Game Boy Color (0 pixels
  differ).
  `oam_bug` 7-timing_effect can't pass as built, on hardware or here: its
  20 corrupted-OAM dumps come to 10,525 bytes of text, which it copies to
  cartridge RAM from `$A004` with no end check, so past `$BFFF` the text
  overwrites its own code in work RAM and it crashes. The survey runs it
  with those writes undone (`TextSpill`), so it reaches its CRC check over
  all of the text, and passes. `MMEMU_GB_SHOW=1` lists the passes too.
  The survey's second half runs the Game Boy Color (CPU CGB E) through
  SameBoy's boot ROM: Blargg's whole suites against their CGB pictures
  (`cpu_instrs`, `instr_timing`, `mem_timing`, `mem_timing-2`, `halt_bug`,
  `interrupt_time`, `cgb_sound`, `oam_bug`) and their singles
  (`cpu_instrs`, `mem_timing`, `mem_timing-2`, `cgb_sound`), `cgb-acid2`,
  `cgb-acid-hell` and Mooneye's `sprite_priority` against their pictures,
  and the register-checked tests for the CGB E by their names' model
  suffixes: Mooneye's acceptance and misc tests, SameSuite's and AGE's.
  `MMEMU_GB_SURVEY=dmg` or `=cgb` runs one half. The CGB result is
  192 PASS, 0 FAIL, 3 skipped. Mesen2 (built from its source and run on
  the same list) fails 39 of them; the PPU, HALT, speed switch and APU
  timings that differ from Mesen follow those tests, SameBoy and AGE (see
  `gb_ppu.h`, `gb_cpu.cpp`, `gb_apu.cpp`).
  The 3 skipped are Mooneye's `boot_div-cgbABCDE`, `boot_hwio-C` and
  `unused_hwio-C`, which check what Nintendo's CGB boot ROM leaves (DIV's
  phase after its logo, its chime still sounding in PCM12, the palette
  indexes): they run with your own Nintendo boot ROM in
  `roms/gameboy/cgb_boot.bin` (or `MMEMU_GB_FIRMWARE`), and are skipped
  with SameBoy's, which ends sooner and quieter. `oam_bug`'s CGB picture
  was made with `X<<3` per channel (white `$F8F8F8`), so it is compared
  within 7.
- `test_gameboy.cpp`'s `gb_cgb_*` tests: the CGB's post-boot state, its
  registers (VBK, SVBK, the palettes, `$FF72-$FF75`, RP, KEY0), the
  compatibility mode, general and HBlank DMA, the speed switch, rendering
  (attributes, banks, palettes, object order and priority) and PCM12 and
  wave RAM; `gameboy_color_machine` the machine through its boot ROM.

## Not yet

- The MBC6/MBC7, HuC1/HuC3, MMM01 and the other rare mappers, the infrared
  port (RP is a stub), OPRI, the CGB's fast serial clock, the CGB's LCD
  colour correction, separate VRAM/work RAM bank views in the debugger.
- Reverse stepping and snapshots keep only the CPU's registers (not memory
  or the PPU); file assembly and labels in the assembler.
