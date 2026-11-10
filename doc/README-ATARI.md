# Atari 8-bit computers (`atari800`, `atari800xl`, `atari65xe`)

The Atari 800 and the 800XL/65XE (#10), NTSC or PAL: a 6502 at 1.79 MHz (PAL 1.77 MHz), ANTIC, GTIA, POKEY and a 6520 PIA. The machine plugin is `src/plugins/machines/atari/` (`mmemu-plugin-atari.so`).

```
mmemu-gui -m atari800xl        # boots to BASIC's READY in about 4 s (the disk boot times out)
mmemu-cli -m atari800 --speed max
mmemu-cli -m atari800xl-pal
```

## Models

| Id | RAM | OS | BASIC |
|---|---|---|---|
| `atari800` | 48 KB | OS revision B (`roms/atari/osb.bin`, 10 KB) | cartridge at `$A000` (`basic.bin`) |
| `atari800xl` | 64 KB | XL OS (`roms/atari/osxl.bin`, 16 KB) | built in, banked by PIA port B |
| `atari65xe` | 64 KB | XL OS | built in |

Each has a PAL twin: `atari800-pal`, `atari800xl-pal`, `atari65xe-pal`. PAL runs 312 lines a frame (50 Hz) at 1,773,447 Hz, GTIA's PAL register reads PAL (the OS sets `PALNTS`, `$62`, to 1), and colours come from a PAL palette generated with atari800's PAL model (`src/colours_pal.c`).

## Memory map

| Address | 800 | 800XL / 65XE |
|---|---|---|
| `$0000-$9FFF` | RAM | RAM (`$5000-$57FF`: the self-test ROM when port B bit 7 is 0) |
| `$8000-$9FFF` | RAM, or a cartridge's RD4 window | same |
| `$A000-$BFFF` | BASIC, or the cartridge in the slot | a cartridge's RD5 window; else BASIC when port B bit 1 is 0, else RAM |
| `$C000-$CFFF` | nothing (`$FF`) | OS ROM when port B bit 0 is 1, else RAM |
| `$D000-$D0FF` | GTIA (mirrored every 32 bytes) | same |
| `$D200-$D2FF` | POKEY (every 16) | same |
| `$D300-$D3FF` | PIA: PORTA, PORTB, PACTL, PBCTL (every 4) | same |
| `$D400-$D4FF` | ANTIC (every 16) | same |
| `$D500-$D5FF` | cartridge control (bank switching) | same |
| `$D800-$FFFF` | OS-B: floating point at `$D800`, OS at `$E000` | OS ROM when port B bit 0 is 1, else RAM |

On reset, port B's lines float high, so the OS is on and BASIC off. The OS then turns BASIC on unless OPTION is held.

### Extended RAM (XL/XE)

The 800XL and 65XE have a `ram` expansion slot that takes extended RAM banked into `$4000-$7FFF` by PORTB, as a 130XE has or an upgrade adds. The bank bits follow atari800's `memory.c` (`MEMORY_HandlePORTB`):

```
expansion ram xe 128K          # CLI; MCP expansion and the GUI's Control > Expansions... too
expansion ram xe 1088K
expansion ram remove
```

| Option | Bank bits (PORTB) | Banks |
|---|---|---|
| `128K` (130XE) | 2-3 | 4 |
| `192K` | 2, 3, 6 | 8 |
| `320K-rambo` | 2, 3, 5, 6 | 16 |
| `320K-compy` | 2, 3, 6, 7 | 16 |
| `576K` | 1, 2, 3, 5, 6 | 32 |
| `1088K` | 1, 2, 3, 5, 6, 7 | 64 |

- **Which bank is seen:** bit 4 low gives the CPU the selected bank. With bits 4 and 5 both high, both see base RAM.
- **ANTIC:** on the 130XE and Compy-Shop types, bit 5 low gives ANTIC the bank separately, so the screen can come from extended RAM while the CPU works in base RAM. On the other types bit 5 picks a bank, and ANTIC sees what the CPU sees.
- **Priority:** the self-test ROM keeps `$5000-$57FF` while it's on.
- **Removing it:** the window shows base RAM again.
- `atari800xl_extended_ram` writes a signature into every combination of each size's bits, counts the banks that keep it (4 to 64), and checks ANTIC's separate view.

### Second POKEY (stereo)

On all the models, the `pokey2` expansion slot takes a second POKEY, as the stereo upgrades fit one. It follows atari800's `STEREO_SOUND`:

```
expansion pokey2 pokey
expansion pokey2 remove
```

- **Addresses:** it answers at `$D210-$D21F`. Without it, `$D210` mirrors `$D200`.
- **Registers:** it takes AUDF1-4, AUDC1-4, AUDCTL, STIMER and SKCTL. Reads from it give 0, and it raises no interrupts.
- **Sound:** its four voices are mixed with the first POKEY's into the machine's mono output, each chip at half level. True stereo isn't done.
- **Test:** `atari_second_pokey` plays a tone on the second chip alone and hears it.

## Timing and interrupts

- **Scheduler:** each step runs one CPU instruction and clocks the devices for its cycles. While ANTIC holds the CPU (WSYNC, or the DMA cycles ANTIC takes), the step instead clocks the devices one cycle without the CPU.
- **Interrupts:**
  - POKEY and the PIA share IRQ.
  - ANTIC drives NMI: the vertical blank, display-list interrupts, and the 800's RESET key.
- **Real time (#169):** paced on ANTIC's cycle count, which includes the cycles the CPU loses.

## Keyboard

- **Keys:** names from the GUI or CLI `key` (`a`, `return`, `space`, `left_shift`, `control`, `delete`, `escape`, `tab`, arrow keys...) are converted to POKEY key codes. Shifted symbols come in as names such as `dquote` and `exclaim`.
- **Function keys:**

  | Key | Atari key |
  |---|---|
  | F1 | Atari (inverse) key |
  | F2 | OPTION |
  | F3 | SELECT |
  | F4 | START |
  | F5 | RESET: a reset on the XL/XE, the RESET NMI on the 800 |
  | F6 | HELP |
  | F7 | BREAK |
  | F8 | CAPS |

- **Typed text (CLI `type`, MCP `type_string`):**
  - Each key is held 2 frames and released for 2.
  - A repeated key waits longer, because of the OS's debounce.
  - The next key waits until the OS has taken the last one from `CH` (`$2FC`), so BASIC keeps up.
  - Letters are typed as caps-lock capitals.
- **Joysticks:** `onJoystick` maps them to PIA port A (sticks 0 and 1) and the GTIA triggers.

## Loading software

`load <file>` in the CLI, the GUI's Load Image, and MCP loads it into drive 1 and boots. The machine is reset with OPTION held for the first 3 seconds, so on the XL/XE BASIC is off, as most software expects.

- **`.atr`:** a disk image with 128- or 256-byte sectors.
- **`.xex` / `.com`:** an executable. It is put on a generated boot disk. A 187-byte 6502 loader in the disk's three boot sectors (at `$0700`) reads the file through the OS's `DSKINV`:
  - It loads each segment, pointing INITAD (`$2E2`) at an RTS before each and calling it after.
  - At the end it jumps to RUNAD (`$2E0`).
  - Its variables are at `$087A-$087F` rather than in zero page, which INIT routines may use.
  - It reads sectors through `$0600`.

### Cartridges

`cart <file>` in the CLI, the GUI's Attach Cartridge and MCP `attach_cartridge` insert a cartridge and cold-start; `eject` takes it out (`src/plugins/machines/atari/main/atari_cart.cpp`).

- **Files:** `.car` (a 16-byte `CART` header with the type number, as atari800 and Altirra write it) or a raw `.rom` of 8 KB (standard 8 KB) or 16 KB (standard 16 KB).
- **Types:** standard 8 and 16 KB; OSS 034M, 043M, M091 and 8 KB; Williams 32/64 KB; Express, Diamond and SpartaDOS X 64 KB; SpartaDOS X 128 KB; DB 32 KB; XEGS and switchable XEGS 32 KB to 1 MB; MegaCart 16 KB to 2 MB; Atarimax 128 KB and 1 MB (old and new); Phoenix 8 KB and Blizzard 16 KB. Bank switching follows atari800's `cartridge.c`.
- **Mapping:** a cartridge replaces the 800's BASIC and overrides the XL/XE's built-in BASIC while it drives RD5.
- **TRIG3:** on the XL/XE it reads the cartridge's RD5 line (1 while a cartridge is mapped at `$A000`). The OS checks it every vertical blank.

### The serial bus (SIO)

Disk drives D1:-D4: (devices `$31-$34`) sit on POKEY's serial lines (`src/plugins/machines/atari/main/atari_sio.cpp`):

- **Command frames:** the drive collects the bytes the computer sends while the PIA holds CB2 (the command line) low. It answers once the line goes high, if the checksum is right and the drive holds a disk.
- **Responses:** ACK, COMPLETE and data come back through POKEY's serial input at 19,200 baud (940 cycles a byte).
- **Commands:**

  | Command | Response |
  |---|---|
  | `R` (read sector) | the sector's data |
  | `S` (status) | density and FDC status |
  | `W` / `P` (write) | written to the image, and to its file |
  | others | `N` |

With no disk in a drive, nothing answers, and the OS's boot attempt times out as on real hardware.

- **Mounting:** `load` puts a disk in D1: and boots it. CLI `disk mount <1-4> <file.atr>` (and `disk eject <n>`) changes disks without a reset; the GUI's Drive Status pane lists D1:-D4: with the last sector used.
- **Saving:** a disk from a file is written through: each sector written goes into the `.atr` at once. If the file cannot be opened for writing, the drive is write-protected (status bit 3) and writes end in an error. The generated `.xex` boot disk has no file; its writes stay in memory.

## Graphics

- **GTIA modes** (PRIOR bits 6-7: BASIC's GRAPHICS 9, 10 and 11) work on any ANTIC mode: ANTIC's two bits a colour clock pair up into a 4-bit pixel two colour clocks wide.
  - 9: 16 luminances of COLBK's hue.
  - 10: nine colour registers (values 0-3 are COLPM0-3, 4-7 COLPF0-3, 8 COLBK). Its pixels start a colour clock later than 9 and 11, as on the chip.
  - 11: 16 hues at COLBK's luminance (0 is black of that hue).
- **VDELAY:** a player or missile whose bit is set ignores DMA on even scan lines.
- **Multicolour players** (PRIOR bit 5): where players 0 and 1 (or 2 and 3) overlap, their colours are ORed.

## Not yet supported

- The cassette and other SIO devices (printer, R:); disk formatting.
- Cartridge types not listed above, and the 800's right slot.
- The 400 and 1200XL.
- Mid-line register changes: each line is drawn at cycle 104 with the registers as they are then.
- The console speaker (key click).
