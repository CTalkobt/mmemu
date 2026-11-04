# Commander X16 (`x16`)

The Commander X16 (#16): a WDC 65C02 at 8 MHz, VERA (video and sound), two 65C22 VIAs, and the I2C system management controller (keyboard) and real-time clock. The machine plugin is `src/plugins/machines/x16/` (`mmemu-plugin-x16.so`); VERA is `src/plugins/devices/vera/`. The memory map, I/O decode and devices follow [x16-emulator](https://github.com/X16Community/x16-emulator) (2-clause BSD), the reference the X16 ROM is tested against.

```
mmemu-gui -m x16
mmemu-cli -m x16 --speed max
```

## References

- [x16-emulator](https://github.com/X16Community/x16-emulator): the behaviour mmsim follows.
- [x16-rom](https://github.com/X16Community/x16-rom): KERNAL, BASIC and DOS source and releases.
- [x16-docs](https://github.com/X16Community/x16-docs): the Programmer's Reference Guide (memory map, VERA, YM2151, KERNAL API).
- [x16-user-guide](https://github.com/X16Community/x16-user-guide): the User's Guide (setup, keyboard and screen, BASIC graphics and sound, with examples to type in).

## ROM

`roms/x16/rom.bin`: the `rom.bin` of an [x16-rom release](https://github.com/X16Community/x16-rom/releases) (developed against R49 "Pyrite"), 16 KB banks, up to 512 KB. It is not in the repository (`.gitignore`); the release's `.sym` files can go next to it.

## Memory map

| Address | Contents |
|---|---|
| `$0000` | RAM bank register (also RAM) |
| `$0001` | ROM bank register (also RAM) |
| `$0002-$9EFF` | RAM |
| `$9F00-$9F0F` | VIA1: PA0/PA1 I2C (SMC, RTC), PA2-PA7 NES controllers; PB IEC serial bus |
| `$9F10-$9F1F` | VIA2 (user port, nothing attached) |
| `$9F20-$9F3F` | VERA |
| `$9F40-$9F5F` | YM2151: even addresses select the register, odd ones write data and read status (3 wait states) |
| `$9FA0-$9FFF` | slow I/O expansion (3 wait states; open bus `$9F`) |
| `$A000-$BFFF` | banked RAM: 64 banks of 8 KB (512 KB); banks above read open bus |
| `$C000-$FFFF` | banked ROM: banks 0-31 from `rom.bin` |

## Devices

- **CPU**: `MOS65C02` (`doc/README-6502.md` §7).
- **VIAs**: `VIA6522` at the CPU clock; VIA1, VIA2 and VERA share the IRQ line.
- **I2C** (`x16_i2c.cpp`, after x16-emulator's `i2c.c`, `smc.c`, `rtc.c`): VIA1's PA0 (SDA) and PA1 (SCL) are open collector — a line is low while its DDR bit is 1 and its output bit 0. Devices:
  - **SMC** (`$42`): the keyboard buffer (16 key codes), version 47.0.0, reset/NMI/power requests; no mouse yet (mouse reads give "no packet").
  - **RTC** (`$6F`, MCP7940N): BCD time starting at the host's local time, 64 bytes of NVRAM (`$20-$5F`).
- **IEC serial bus**: nothing attached (lines read released).
- **Interrupt vectors**: the 65C02's VPB pin selects ROM bank 0 while an interrupt (IRQ, NMI, BRK) fetches its vector, as on the hardware — other banks hold `$AAAA` there. Only the bank latch changes: `$01` still reads the interrupted bank, which the KERNAL's handler restores (x16-emulator `vp6502()`). The core exposes this as `MOS6502::setVectorPullHook`.

## VERA

`src/plugins/devices/vera/` (`Vera`, `VeraPsg`, `VeraPcm`), ported from x16-emulator's `video.c`, `vera_psg.c`, `vera_pcm.c` and `vera_spi.c`:

- **Registers and VRAM**: `$9F20-$9F3F`, 128 KB VRAM (PSG `$1F9C0`, palette `$1FA00`, sprite attributes `$1FC00`), both address ports with the increment/decrement table and nibble steps, every DCSEL bank including FX and the version string (DCSEL 63). Peeks (debugger, memory views) do not move the address ports.
- **Video**: 640×480; layers in text, tile and bitmap modes; 128 sprites with Z depth, flips and collisions; HSCALE/VSCALE, border and display window; VGA, NTSC interlaced and 240p timing at 25 MHz; VSYNC, LINE, SPRCOL and AFLOW interrupts. Mid-line register changes are rendered.
- **FX**: line, polygon and affine address helpers, the 32-bit cache with nibble masks and transparent writes, the multiplier/accumulator.
- **Audio**: 16 PSG voices and the PCM FIFO, mixed at 48,828 Hz (25 MHz / 512) as x16-emulator does; the GUI and `recordaudio` play it.
- **SPI**: the registers and busy timing, with the SD card below on the select line.

Where the port departs from the emulator (integer scan timing, a repeatable VRAM fill at reset, 17-bit address wrap, peeks that return what a read would) is commented in `vera.cpp`.

## SD card

`SdCardSpi` (`src/plugins/devices/vera/main/sdcard_spi.cpp`, after x16-emulator's `sdcard.c`) answers VERA's SPI port as an SD card in SPI mode, from a raw image file: CMD0/8/9/12/13/16/17/18/24/55/58 and ACMD41, block reads (single and multiple) and block writes, which go straight to the file (read-only if the file cannot be opened for writing). The X16's DOS wants a FAT32 partition, for example:

```
truncate -s 64M sd.img
echo 'start=2048, type=c' | sfdisk sd.img
mkfs.vfat -F 32 -n X16SD --offset 2048 sd.img
```

Mount it as unit 8 (CLI `disk mount 8 sd.img`, the GUI drive pane, MCP `mount_disk`), or put it at `roms/x16/sdcard.img` to have it mounted at start. Then `SAVE"NAME"`, `DOS"$"`, `LOAD"NAME"` work.

## YM2151

`src/plugins/devices/ym2151/` wraps [ymfm](https://github.com/aaronsgiles/ymfm)'s OPM (vendored in `src/third_party/ymfm`, BSD-3-Clause), with x16-emulator's glue (`ymglue.cpp`): 3.579545 MHz, one sample per 64 chip clocks (55,930 Hz), the busy flag and timers counted in those clocks, writes while busy dropped, timer interrupts on the shared IRQ line. VERA mixes it into its output as the X16 does — `(psg + pcm) * 2 + ym` through VERA's limiter — resampling it linearly to 48,828 Hz. BASIC's `FMINIT`, `FMNOTE`, `FMPLAY`... play (`FMNOTE 0,$4A` gives 440 Hz).

## Tests

- `x16_boots_to_ready_and_runs_basic` boots R49 to READY and runs a typed `PRINT 6*7` (skipped without `roms/x16/rom.bin`).
- `test_x16_i2c.cpp`: the I2C bus bit-banged through a real VIA as the KERNAL does it: SMC keyboard buffer and version, RTC time, ticking and NVRAM.
- `test_ym2151.cpp`: the busy flag, a keyed-on voice producing sound, timer A raising the IRQ line.
- `test_sdcard_spi.cpp`: initialisation, a block read and write against an image file, out-of-range reads.
- `test_vera.cpp`: VERA registers, address ports, palette, text layer, sprites, collisions, LINE/VSYNC interrupts, peeks, PSG, PCM and FX.

## Keyboard

Keys go to the SMC as IBM key numbers (x16-emulator `keyboard.c`): the GUI sends PC keys positionally (Shift is a real key), and CLI `key`/`type` and MCP accept the same names (`a`, `return`, `left_shift`, `escape`, `f1`...). Typed text: uppercase letters are unshifted (the X16 starts in the PETSCII upper case / graphics set), lowercase shifted; one key event goes to the SMC whenever its buffer is empty.

## Not yet emulated

- IEC devices (and the host filesystem as a drive); mouse; NES/SNES controllers.
