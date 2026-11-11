# mmemu-plugin-snes: Super Nintendo / Super Famicom

The SNES and Super Famicom (#15), after Mesen2 (GPL-3.0, Sour and the Mesen
contributors) and the fullsnes / Anomie documentation:
- **Timing and memory map:** `Core/SNES/SnesMemoryManager.cpp`, `InternalRegisters.cpp`.
- **DMA and HDMA:** `SnesDmaController.cpp`.
- **Cartridges:** `BaseCartridge.cpp`.
- **PPU:** `SnesPpu.cpp`.
- **Sound:** `Spc.cpp`, `SpcTimer.h`, `DSP/`.
- **Super Game Boy:** `Coprocessors/SGB/SuperGameboy.cpp` and `Core/Gameboy/`.

## Profiles

| Profile | Machine |
|---------|---------|
| `snes` | The SNES: master clock 21.477 MHz, 262 lines of 1364 clocks (NTSC); a PAL cartridge makes it PAL |
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
| `$4016-$4017` | the controller ports' serial lines |
| `$4200-$421F` | interrupts, multiply/divide, timers, DMA enables, auto-read results |
| `$4300-$437F` | the eight DMA channels |
| `$6000-$7FFF` | HiROM battery RAM (banks `$20-$3F`) |
| `$8000-$FFFF` | the cartridge |

  Banks `$7E-$7F` hold all 128 KB of work RAM. Banks `$40-$7D` and
  `$C0-$FF` are the cartridge's.

- **PAL** (as Mesen): a cartridge whose header destination code (`$FFD9`)
  is `$02-$0C`, `$11` or `$12` runs on a PAL console: a 21.281 MHz master
  clock, 312 lines a frame (313 for the even interlaced field), 50 frames
  a second at real time, and `$213F` bit 4 set. Vertical blank still
  starts at line 225 (240 with overscan). The region can be chosen by
  hand, as Mesen's setting: CLI/MCP `expansion region auto|ntsc|pal`, the
  GUI's Control > Expansions, `snes::setRegion`. A change powers the
  console on again.
- **Timing**: a line is 1364 master clocks (341 dots; dots 323 and 327
  take 6 clocks) and a frame 262 lines. Interlaced, every other frame has
  263 lines; otherwise line 240 of every other frame is 4 clocks short. The
  DRAM refresh steals 40 clocks a line at clock 538 less the master clock's
  position in 8 (as Mesen). HDMA is asked for at dot 276, for the next
  line. Vertical blank starts at line 225 (240 with overscan): the NMI flag
  (`$4210`), the auto joypad read, and the OAM address reload. The auto
  read (Mesen's): from the first multiple of 256 clocks after clock 130 of
  that line, less 128, it holds the ports' strobe for 256 clocks, then
  clocks 16 bits in 128-clock steps (a read, then a shift) into
  `$4218-$421F` (D0 of port 1 and 2, then D1); `$4212` bit 0 is set from
  128 clocks in until the end. HDMA is set up again at clock 12 (plus the same
  position in 8) of line 0.
- **Interrupts**: NMI (`$4200` bit 7) and the H/V timer IRQ (`HTIME`,
  `VTIME`, `$4211`). As Mesen, the NMI flag rises at clock 2 of the NMI
  line (falls at clock 2 of line 0) and the CPU sees it from clock 6; a
  `$4210` read before clock 6 of that line doesn't clear it, nor does a
  `$4211` read within 4 clocks of the IRQ flag rising. When DMA or HDMA ran
  in an instruction's last cycle, an IRQ waits one more instruction, and so
  does an NMI that rose in that cycle (Mesen's IrqLock).
- **H/V IRQ counters** (Mesen's ProcessIrqCounters): H and V tick every 4
  clocks, at clocks 2, 6, 10... of the line, through DRAM refresh too. H is
  reset at clocks 6 and 10 and counts from 14; at clock 2 it is the last
  line's last count + 1 (339), so `HTIME` 339 matches at the start of the
  next line. V counts at clock 6 and is reset at clock 2 of line 0. When
  they come to match `$4207-$420A` the flag is set a tick later (clock 10
  for V alone, `4*HTIME + 14` for H) and the CPU sees it the tick after
  (H = 0 a tick later still). A write to `$4207-$420A` that makes them
  match raises it as well. The CPU samples the IRQ line as each of its
  cycles starts.
- **Open bus** (as Mesen): reads of the internal registers (`$42xx`) don't
  change it; `$4210`, `$4211`, `$4212`, `$4016`/`$4017` fill their unused
  bits from it. The write-only PPU registers `$21x4-6` and `$21x8-A` give
  PPU1's last value read; the others, and `$2137`, the CPU's open bus.
  `$213B`'s high byte, `$213C`/`$213D`'s high bits and `$213E`/`$213F`'s
  unused bit come from PPU1/PPU2's own. `$4212`'s horizontal blank starts
  after dot 274.
- **Multiply/divide**: `$4202`/`$4203` and `$4204-$4206`, with results at
  `$4214-$4217`. As Mesen's `AluMulDiv`, they work a bit a CPU cycle on
  the result registers themselves: 8 cycles to multiply, 16 to divide.
  Reading early gives the partial result, a write while one runs doesn't
  start another, and they stand still during DMA (they count the CPU's own
  cycles). Dividing by zero gives `$FFFF` remainder the dividend.
- **DMA**: modes 0-7 in both directions, with A-bus increment, decrement or
  fixed. Work RAM to `$2180` writes nothing, and `$2180` to work RAM
  writes a meaningless byte, as Mesen.
- **HDMA**: direct and indirect tables, repeat and line counts. The last
  active channel ending an indirect table reads one address byte, not two.
- **DMA timing** (Mesen's `SnesDmaController`): a DMA or HDMA asked for
  (by `$420B`, or at dot 276) runs at the start of the CPU's next cycle but
  one, even in WAI. It begins on a whole multiple of 8 master clocks, takes
  8 clocks of overhead, then 8 a channel and 8 a byte (read at 4, written
  at 8). HDMA reads each channel's next table byte (8) and indirect
  addresses (16). It ends on a whole CPU cycle. HDMA cuts in between a
  DMA's bytes.
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
  - **Coprocessors:** the DSP-1, 1B, 2, 3 and 4, the ST010 and ST011, the Super FX, the SA-1, the Cx4, the S-DD1, the SPC7110, the OBC1 and the ST018 (see below), the Sufami Turbo adapter, the Satellaview and the MSU-1.

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

### ST010 and ST011

Seta's ST010 (F1 ROC II) and ST011 (2dan Morita Shougi) are a NEC
uPD96050, the uPD7725's bigger relative, run by the same `snes::NecDsp`
(`setModel(true, ...)`, as Mesen): 16384 program words, a 2048-word data
ROM, 2048 words of RAM and an 8-level stack, at 11 MHz (ST010) or 22 MHz
(ST011).

- **Detection:** chipset `$Fx` with cartridge type `$01` (`$FFBF`). The
  title `2DAN MORITA SHOUGI` picks the ST011, anything else the ST010.
- **Registers:** `$60:0000-$0FFF` (and `$E0`), DR at even addresses, SR at odd.
- **RAM:** `$68-$6F:0000-$0FFF` (and `$E8-$EF`), byte by byte (low byte at
  even addresses). It is the battery RAM, saved as the `.srm` (4096 bytes,
  little-endian words, as Mesen).
- **Firmware:** `st010.rom`/`st011.rom` (53248 bytes: 49152 of program, then
  4096 of data), or `.program.rom` plus `.data.rom`, in `roms/snes/`, or
  appended to the ROM image (52 KB past a 64 KB boundary).

### ST018

Seta's ST018 (Hayazashi Nidan Morita Shougi 2) is an ARM: `snes::St018`,
after Mesen's `St018.cpp` and `ArmV3Cpu.cpp`, an ARMv3 core with 128 KB of
program ROM, 32 KB of data ROM and 16 KB of work RAM. Its multiplier (result,
carry and timing) is zaydlang's, copied from Mesen (`snes_arm_multiply.h`,
zlib licence).

- **Detection:** chipset `$Fx` with cartridge type `$02` (`$FFBF`). It is
  battery-backed; the rest is the plain LoROM map.
- **The link (`$3000-$3FFF` in `$00-$3F`/`$80-$BF`, decoded by A1-A2 and
  A8-A15):** `$3800` reads the ARM's byte, `$3802` writes one to it (a read
  clears the acknowledge), `$3804` is the status (bit 0 a byte for the SNES,
  bit 2 acknowledge, bit 3 a byte for the ARM, bit 7 running). Writing
  `$3804` non-zero holds the ARM in reset; zero restarts it.
- **The ARM's side:** `$4000_0000` its byte to the SNES, `$4000_0010` the
  SNES's byte (writing acknowledges), `$4000_0020` the status; ROM at
  `$0000_0000` and `$A000_0000`, RAM at `$E000_0000`.
- **Timing:** as Mesen, each ARM bus access or internal cycle takes one
  master clock; the ARM is run up to the SNES's time after every step and
  before each link access. The RESET button leaves it alone (the game resets
  it through `$3804`).
- **Firmware:** `roms/snes/st018.rom` (163840 bytes: the program, then the
  data ROM), Mesen's name. Not included (Seta's copyright).

### Sufami Turbo

Bandai's adapter takes two small cartridges (`.st`) on top of its own BIOS
cartridge, mapped as Mesen:

- **Slot A** is the cartridge attached as usual (`cart game.st`, any
  frontend); a file starting `BANDAI SFC-ADX` without a SNES header is
  taken as one, whatever its extension.
- **Slot B**: CLI/MCP `expansion slotb cart <file>` (empty to take it
  out), the GUI's Control > Expansions, `snes::insertSufamiSlotB`. Putting
  one in or out powers the console on again; a new slot A cartridge leaves
  slot B empty.
- **Map:** the BIOS at `$00-$1F:8000-FFFF`, slot A's ROM at `$20-$3F`,
  slot B's at `$40-$5F`, slot A's RAM at `$60-$63` and slot B's at
  `$70-$73` (all mirrored at `$80+`). As Mesen, they are 4 KB pages whose
  number wraps at the size.
- **RAM**, by the game as Mesen: 2 KB for Poi Poi Ninja and SD Ultra
  Battle, 8 KB for SD Gundam Generation, none otherwise. Each slot saves
  beside its file (`.srm`); the same game in both slots saves slot B as
  `<name>_SlotB.srm`.
- **BIOS:** `roms/snes/sufamiturbo.sfc` (262144 bytes, Mesen's
  `SufamiTurbo.sfc`). Not included (Bandai's copyright); without it the
  cartridge is refused with a message naming the file.

### Satellaview (BS-X)

Nintendo's satellite modem for the Super Famicom (`snes::Bsx`, after
Mesen's `BsxCart`, `BsxMemoryPack`, `BsxSatellaview` and `BsxStream`):

- **The BS-X cartridge** (chipset `$E5`): attaching its BIOS as a
  cartridge runs it with no memory pack; a `.bs` memory pack attached as
  a cartridge goes in it, with the BIOS from `roms/snes/bsx.bin` (Mesen's
  `BS-X.bin`; not included, Nintendo's copyright; without it the pack is
  refused with a message naming the file).
- **Its registers** at `$00-$0F:5000` (bit 7; `$0E` and `$0F` are
  write-only): a write is pending until `$0E:5000` is written. They lay
  out the map: the pack as LoROM or HiROM, the 512 KB PSRAM and where it
  sits, holes in the pack, and the BIOS over `$00-$3F`/`$80-$BF`. As
  Mesen, the map is a table of 4 KB pages that each layout rewrites in
  part, so pages an earlier layout set stay. The BIOS's RAM is also at
  `$10-$1F:5000`, and is one save for every pack: `roms/snes/bsx.srm`
  (Mesen's `BsxBios.srm`). The RESET button resets the registers.
- **The memory pack** is flash: commands written to its first byte (`$10`
  or `$40` a byte write that only clears bits, `$70` status, `$71`
  extended status, `$75` vendor information, `$20 $D0` page erase, `$A7
  $D0` chip erase). As Mesen, a `.bs` pack's changes aren't saved.
- **The Satellaview** at `$2188-$219A`: two streams (channel, prefix
  count, prefix, data, status) at 1000 packets of 22 bytes a second, the
  stream/LED register and fixed status bytes. Channel 0 is the time: the
  host's clock, or `snes::setSatellaviewDate` (from the next reset). Other
  channels play `BSX<channel>-<n>.bin` from `roms/snes/satellaview/`
  (Mesen's file names), going back to file 0 at the end.
- **A pack slot on an ordinary cartridge** (game code `Zxxx`, maker
  `$33`, as Mesen: Sound Novel Tsukuru and others): the pack at
  `$C0-$EF` (LoROM) or `$20-$3F`/`$60-$7D`/`$A0-$BF`/`$E0-$FF` (HiROM),
  an erased 1 MB pack at first, saved beside the ROM as `<rom>.bs`. As
  Mesen, not on SA-1 cartridges.

### Super Game Boy

Nintendo's adapter: a SNES cartridge holding a whole Game Boy behind the
ICD2 chip (`snes::Sgb`, after Mesen's `SuperGameboy`, with the Game Boy core
of [README-GAMEBOY.md](README-GAMEBOY.md)).

- **Supplying it:** load the Game Boy cartridge itself (`cart game.gb`,
  `.gb`/`.gbc`, any frontend): as in Mesen it goes in the SGB, whose BIOS is
  read from `roms/snes/SGB1.sfc` (or `SGB2.sfc`; `MMEMU_SNES_FIRMWARE`
  moves the folder). The BIOS is not included (Nintendo's copyright);
  without it the cartridge is refused with a message naming the file.
  Attaching the BIOS itself (chipset `$E3`, or game code `042J`, as Mesen)
  gives an SGB with an empty slot: a Game Boy cartridge goes in through
  CLI/MCP `expansion gameboy cart <file>` (empty to take it out), the GUI's
  Control > Expansions, or `snes::insertSgbCartridge`. Either way the
  console powers on again. The cartridge's description ends
  `, Super Game Boy: <title>, <mapper>, <sizes>`.
- **SGB or SGB2** by the BIOS (`GAMEBOY2` in its title, or 512 KB): the
  SGB1's Game Boy runs at the SNES master clock / 5 (4.295 MHz, a little
  fast, as on the real one), the SGB2's at its own 20.97 MHz crystal / 5;
  `$6003`'s divider gives / 4, 5, 7 or 9. The Game Boy is brought up to
  the master clock on every CPU cycle and before each ICD2 access.
- **The ICD2's registers** at `$6000-$7FFF` of `$00-$3F`/`$80-$BF`: the LCD
  row being written and its buffer (`$6000`), the row buffer to read
  (`$6001`), packet ready (`$6002`), the Game Boy's reset, player count and
  divider (`$6003`), the four players' buttons (`$6004-$6007`), the version
  `$21` (`$600F`), the 16-byte packet (`$7000-$700F`), and the row buffer a
  byte at a time (`$7800`: 20 tiles of 2bpp, 320 bytes, then `$FF`).
- **The Game Boy's side:** its LCD pixels fill four 8-line buffers in turn;
  its writes to P14/P15 are decoded as packets (a reset pulse, 128 bits, a
  stop bit), or with both lines high step to the next player; `$FF00`
  reads the current player's buttons, or `$F` minus the player. The boot
  ROM is SameBoy's open source SGB boot ROM (MIT licence, as Mesen embeds
  it), which sends the cartridge's header to the BIOS in six packets. What
  the packets mean (palettes, borders, `MLT_REQ`, sound) is the SGB BIOS's
  own work, so nothing of it is emulated here.
- **Sound:** the Game Boy's APU is box-filtered to 32 kHz and added to the
  S-DSP's output a sample at a time, not while the DSP's `FLG` mutes it (as
  Mesen). Unlike Mesen, which resamples with a Hermite filter, a DC
  blocker takes out the Game Boy's offset.
- **Battery RAM:** the Game Boy cartridge's RAM is saved beside it
  (`<game>.srm`), an MBC3's clock to `<game>.rtc` (Mesen's format).
- **Not emulated:** the link port (the SGB has none), the SGB2's link
  cable; Game Boy Color cartridges run as on a Game Boy.

### MSU-1

byuu's add-on for homebrew and fan patches (`snes::Msu1`, after Mesen's
`Msu1` and `PcmReader`): a data file read a byte at a time and CD-quality
audio tracks, at `$2000-$2007` of banks `$00-$3F`/`$80-$BF`, on any
cartridge.

- **Found as Mesen does:** `<rom>.msu` beside the ROM (the data; tracks
  `<rom>-<n>.pcm`), or `msu1.rom` in the ROM's folder (tracks
  `track-<n>.pcm`). The cartridge's description then ends `, MSU-1`.
- **Registers:** the status (revision 1, track missing, playing,
  repeating; never busy, as Mesen), the data port, `S-MSU1` at
  `$2002-$2007`; written, the data pointer (taking effect at `$2003`), the
  track (loaded at `$2005`), the volume and play/repeat.
- **Audio:** a track is `MSU1`, a 32-bit loop point in samples, then
  44.1 kHz 16-bit stereo. It is resampled to the DSP's 32 kHz (Hermite, as
  Mesen) and added to the DSP's output times volume / 255; at its end it
  stops (playing clears) or goes back to the loop point.
- **Unlike Mesen:** the audio follows the emulated time, not the host's
  audio pulls; and a track ending just as a batch of samples does still
  loops (Mesen's stalls). Power-on starts the MSU-1 afresh.

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

### S-DD1

The S-DD1 (`snes::Sdd1`, after Mesen's `Sdd1.cpp`/`Sdd1Mmc.cpp`) is used by
Star Ocean and Street Fighter Alpha 2. Its decompressor is Andreas Naive's
public-domain code, as Mesen uses it.

- **Detection:** chipset `$43`/`$45`.
- **ROM:**
  - **`$C0-$FF`:** four 1 MB blocks, chosen by `$4804-$4807`.
  - **`$00-$3F`/`$80-$BF`:** LoROM. Bit 7 of `$4805`/`$4807` mirrors
    `$20-$3F` onto `$00-$1F` (and `$A0-$BF` onto `$80-$9F`).
- **Battery RAM:** at `$6000-$7FFF` and `$70-$73`.
- **Decompression:** `$4800` and `$4801` enable DMA channels. When an
  enabled channel reads its own source address in `$C0-$FF`, the S-DD1
  returns decompressed data, until the DMA length runs out. It watches the
  DMA registers for that address and length.

### SPC7110

The SPC7110 (`snes::Spc7110`, after Mesen's `Spc7110.cpp`, `Spc7110Decomp.cpp`
and `Rtc4513.cpp`) is in Far East of Eden Zero, Momotarou Dentetsu Happy and
Super Power League 4. The decompressor is bsnes', as Mesen uses it.

- **Detection:** chipset `$F5`/`$F9` with cartridge type `$00` (`$FFBF`).
  `$F9` has the clock. These cartridges always have battery RAM.
- **ROM:** the first 1 MB is the program (HiROM at `$00-$3F`/`$80-$BF`
  `$8000-$FFFF` and at `$C0-$CF`). The rest is data ROM, seen through three
  1 MB windows at `$D0-$DF`, `$E0-$EF` and `$F0-$FF` (`$4831-$4833`). A ROM
  of 6 MB or more also puts its second program megabyte at `$40-$4F`.
- **Battery RAM:** at `$6000-$7FFF` in `$00-$3F`/`$80-$BF`.
- **Decompression (`$4800-$480C`):** a directory of 4-byte entries (mode,
  source address) in data ROM. Writing `$4806` starts it; the tiles are read
  from `$4800` or anywhere in bank `$50`, at 1, 2 or 4 bits a pixel.
- **The data port (`$4810-$481A`):** reads data ROM from a base, an offset
  and a step, signed or not.
- **The ALU (`$4820-$482F`):** 16 x 16 multiply and 32 / 16 divide, signed
  or not, with no delay.
- **The RTC-4513 (`$4840-$4842`):** as far as Mesen goes, without the
  30-second adjust, 12-hour mode or calendar flags. It keeps time from the
  host clock and is saved beside the `.srm` as `<rom>.rtc` (Mesen's layout:
  16 registers, then the time as 8 bytes, high first). The date arithmetic
  is done in UTC, where Mesen uses local time, so daylight saving can't
  move it.

### OBC1

The OBC1 (`snes::Obc1`, after Mesen's `Obc1.cpp`) is in Metal Combat:
Falcon's Revenge. It helps with sprites, over the cartridge's battery RAM at
`$6000-$7FFF` in `$00-$3F`/`$80-$BF`; the rest is the plain LoROM map.

- **Detection:** chipset `$2x` (with cartridge RAM).
- **Tables:** two OAM-shaped tables in the RAM, at `$1C00` or `$1800`
  (`$7FF5` bit 0), and a sprite index at `$7FF6`.
- **Ports:** `$7FF0-$7FF3` are the chosen sprite's four bytes. `$7FF4` is
  its two bits in the high table; writing it changes only those two.

### PPU

Drawn as the beam goes: before each write to a PPU register (by the CPU,
DMA or HDMA) the line is drawn up to the current dot with the old values,
pixel x leaving at dot x + 22. Mid-line changes split the line as on the
hardware. A line's sprites are found and fetched in the previous line's
horizontal blank.

The backgrounds' tilemap and character data are fetched ahead of the beam,
as Mesen: the 8-pixel column c in dots 8c to 8c+7 (33 columns by dot 263),
in each mode's slot order, offset-per-tile reading BG3's entries for the
next column. A mid-line change to the scroll, the map or character bases,
the BG mode or the mosaic reaches the tiles at the next column fetched;
the fine scroll is taken as each pixel is drawn. Nothing is fetched in
forced blank or mode 7 (mode 7 reads VRAM as it draws, as Mesen).

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
  The mosaic's rows follow Mesen's line counter: reloaded on line 0 and
  when the mosaic is turned on from off, so a block starts at the line it's
  turned on. Mode 7 holds the row after its vertical flip. In modes 5 and 6
  a mosaic of any size gives both half-pixels the even one.
- **Offset-per-tile, as Mesen**: BG3's entries are addressed straight from
  its column and row (a 64-wide map's right half is not its second screen),
  the vertical entry 32 words on, wrapping in the map. In mode 6 the offset
  is applied to the scroll in half-pixels. A 16x16 tile's half comes from
  the BG's own scroll register, not the offset.
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

- **During the picture** (not forced blank, before vertical blank; as
  Mesen): VRAM isn't written (the address still steps) and its reads give
  0. CGRAM outside H 88-1095 (line 0 excepted) goes to the address the PPU
  is reading: the last colour looked up, 0 when the backdrop shows on
  either screen. OAM goes to the sprite being evaluated (one per two dots
  from the line's start, by the priority rotation) and then to the first
  sprite fetched, and a `$2104` write lands in the high table too.
  Sprites are evaluated even when not shown, for `$213E`'s flags. Writing
  `$2100` in forced blank on vertical blank's first line reloads the OAM
  address.

**Not yet**: The OAM and CGRAM addresses during the picture are approximate (Mesen's
are too).

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
    every port access and every instruction. Each bus access takes its
    time (a DSP step, the timers) as it happens; the rest of an
    instruction is idle cycles.
  - **TEST (`$F0`)**, as Mesen (written only with the P flag clear): wait
    states of 1, 2, 5 or 10 cycles for internal accesses (`$F0-$FF`, the
    IPL ROM, idle cycles) and for external ones, the timers stepping 2, 4,
    8 or 16 with them; the timers' global enable (bit 3) and disable (bit 0),
    switching which can count a tick; and RAM writes on or off (bit 1).
  - **Port latch** (as Mesen): a main-CPU write to `$2140-$2143` that
    lands in the first half of an SPC700 cycle is seen in that cycle; one
    in the second half only from the next cycle on.
- **S-DSP** (`snes::Dsp`):
  - **Pipeline:** the hardware's 32-step pipeline, one step per SPC700
    cycle (32 kHz stereo).
  - **Voices:** BRR decoding with its filters, ADSR and the four GAIN
    modes, Gaussian interpolation, pitch modulation, and noise.
  - **Echo:** the echo buffer with its 8-tap FIR, plus KON/KOFF/ENDX timing.
  - **Output:** mixed to mono and resampled to 44.1 kHz (a DSP sample
    that makes two output samples gives the same value to both).

## Input

- **Controller ports**: each takes a pad (the default), the SNES Mouse,
  the Super Scope, a multitap or the Justifier: CLI/MCP `expansion
  port1|port2 <device>` (`pad`, `mouse`, `superscope`, `multitap`,
  `justifier`, or empty for nothing), the GUI's Control > Expansions,
  `snes::setPortDevice`. After Mesen's devices (the Justifier after
  bsnes's, as Mesen has none):
  - **Pad:** 12 buttons, then `0000`, then 1s.
  - **Mouse** (the host mouse, left and right buttons): 32 bits, buttons
    and speed then Y and X (sign and 7 bits; the sign keeps the last way
    moved). Reading it with the strobe high steps the speed (0-2).
  - **Super Scope** (aimed with the host mouse; left fires, right is the
    cursor button, middle pause, the wheel flips turbo): fire, cursor,
    turbo, pause and offscreen, then 1s. Fire counts once a press unless
    turbo is on. With fire or cursor held, the PPU latches its counters
    where it points (10 dots right and 3 lines up, as Mesen) once the
    picture gets there, so `$213C/$213D` say where.
  - **Multitap:** four pads, two at a time on D0 and D1, by `$4201` bit 7
    (port 2) or bit 6 (port 1); strobed it answers D1 high, D0 low.
  - **Justifier** (Konami's gun, for port 2; gun 1 aimed with the host
    mouse, left is the trigger, right START): 32 bits, 12 zeros, the
    signature `1110 0101 0101`, both triggers, both STARTs, the active gun,
    then `000` and 1s. Every latch makes the other gun active, and the
    active gun latches the PPU's counters every frame it points at the
    picture, trigger or not, at the Super Scope's offset. The second gun is
    not plugged in (offscreen, buttons up) unless set through the API.
- **Gamepad** (`onGamepad`, the CLI/MCP `joy`, host controllers): all
  twelve buttons, players 1-5: the pads in port 1, then port 2 (a
  multitap's four in turn).
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
- `test_snes_bsx.cpp`: a hand-built BS-X BIOS: its registers and the maps
  they give (and the pages an earlier map leaves), the RAM's shared save, a
  `.bs` pack's flash commands, the time channel's packet for a given date,
  a channel played from a file, and a cartridge's pack slot and its save.
- `test_snes_msu1.cpp`: hand-made data and tracks beside a ROM (and
  `msu1.rom`): the ID, the status, the data port, a missing track, the
  audio at full and half volume, pausing, a track's end and its loop.
- `test_snes_sgb.cpp`: a hand-built SGB BIOS and Game Boy cartridge: the
  message when the BIOS is missing, the boot ROM's first header packet at
  `$7000`, the Game Boy's clock ratio (SGB1, SGB2, divider 9, held in
  reset), the LCD row buffers read at `$7800`, the buttons from `$6004`,
  the Game Boy's tone in the SNES's output once the DSP isn't muted, the
  `gameboy` expansion slot, and an MBC1 cartridge's battery RAM saved and
  reloaded.
- `test_snes_sufami.cpp`: a hand-built BIOS and two cartridges: the map
  and its mirrors, both RAMs and their saves, slot B through the expansion
  slot, and the message when the BIOS is missing.
- `test_snes_st018.cpp`: the ST018's ARM and link. A hand-assembled ARM
  program answers the SNES through the link and the cartridge map, and the
  reset hold is checked. A random firmware run is checked against Mesen's
  final registers, cycles and work RAM: Mesen's `ArmV3Cpu.cpp`, compiled on
  its own with the same bus, matched this core register for register and
  cycle for cycle over 30 million random instructions in all seven modes.
- `test_snes_obc1.cpp`: the OBC1's ports, both tables and the high bits,
  on a hand-built cartridge.
- `test_snes_spc7110.cpp`: the SPC7110's map, data port, ALU, clock and
  `.rtc` file, and decompression in all three modes. The expected tiles came
  from Mesen's own decompressor, compiled on its own and fed the same
  pseudo-random data ROM.
- `test_snes_sdd1.cpp`: the S-DD1's banking and mirroring, and a DMA
  receiving decompressed data. The bytes are checked against the
  decompressor run on the same ROM, since there is no compressor to make
  known data.
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
  checks the error given when firmware is missing. A hand-built uPD96050
  firmware checks the ST010's larger RAM and program, its map, and its
  RAM saved and reloaded as the battery.
- `test_snes_machine.cpp` also checks the battery RAM's `.srm` (written,
  reloaded, saved on removal), and a mid-line write splitting a line,
  hi-res and interlace frame sizes and fields, and overscan.

Also seen running: all of krom's PPU demos (mode 7 rotate/zoom and
perspective, HDMA waves and gradients, windows, mosaic, 8bpp pictures, tile
flips, the H-IRQ palette pictures with 64/128 colours a tile row, pseudo-hires,
the six interlace pictures, mode 5 mosaic) and Tecmo's Monster Farm Jump
(menus with colour math, mode 7 stages with sprites).

## Not yet

- Cartridge coprocessors other than the DSP-1..4, Super FX, SA-1, Cx4,
  S-DD1, SPC7110, OBC1, ST010/011 and ST018, the Sufami Turbo, the
  Satellaview, the Super Game Boy and the MSU-1.
- The Satellaview's Soundlink audio and serial I/O (fixed values, as Mesen).
- Two Justifiers from the host (the second gun is there for the API only).
