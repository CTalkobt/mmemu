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
- **IEC serial bus**: nothing attached (lines read released); unit 8 is the host folder (HostFS, below) or the SD card.
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

## Host folder (HostFS)

`X16HostFs` (`src/plugins/machines/x16/main/x16_hostfs.cpp`, ported from x16-emulator's `ieee.c`) serves a host folder as an IEEE / CBM DOS unit, as x16-emulator's HostFS does: it intercepts the KERNAL's IEEE API — `LISTEN` `$FFB1`, `SECOND` `$FF93`, `CIOUT` `$FFA8`, `MCIOUT` `$FEB1`, `UNLSN` `$FFAE`, `TALK` `$FFB4`, `TKSA` `$FF96`, `ACPTR` `$FFA5`, `MACPTR` `$FF44`, `UNTLK` `$FFAB` — and answers calls for its unit itself (then returns as RTS would); calls for other units run in the KERNAL. It fires only while the KERNAL's ROM bank is mapped.

- **Default**: unit 8, the current directory of the emulator (`-m x16` in a folder serves that folder). Nothing outside the folder can be reached (`..` stops at it).
- **What works**: `LOAD`, `SAVE` (`@:` to replace), `OPEN`/`CLOSE`/`GET#`/`INPUT#`/`PRINT#` with `,S,R`/`,W`/`,A`/`,M`, block transfers (MACPTR/MCIOUT, as `LOAD`, `BLOAD`, `BSAVE` use), `DOS"$"` with CMDR-DOS's options (`$:PAT*`, `$=D`, `$=P`, `$=T`, `$=L`, `$=C`), the error channel (`DOS`, channel 15; `73,HOST FS V1.0 X16` after reset), and the DOS commands `CD`, `MD`, `RD`, `S:` (scratch), `R:NEW=OLD`, `P` (seek), `T` (tell), `I`, `UI`, `U0>`n, `S-8`/`S-9`. Matching is case-insensitive with `*` and `?`; host names are shown in ISO-8859-15 (UTF-8 on the host).
- **SD card rule**: while an SD card is mounted (see above) and HostFS uses unit 8, HostFS steps aside and the SD card (CMDR-DOS in ROM) serves unit 8. This is checked at every call, so mounting or ejecting at run time switches between them. Move HostFS to another unit to use both.
- **CLI** (`intercept`): the intercept is `x16.hostfs`.

  ```
  intercept list                          # x16.hostfs [x16.hostfs] on  Host folder as IEEE unit 8: .
  intercept set x16.hostfs path ~/x16     # serve another folder (closes open files, back to its root)
  intercept set x16.hostfs startin sub    # start in a directory inside it
  intercept set x16.hostfs unit 9         # 8-31
  intercept off x16.hostfs                # no host folder: the KERNAL's serial bus (nothing attached)
  ```

Not ported: the 65C816-only `EXTAPI16` calls (`XMACPTR`/`XMCIOUT`), x16-emulator's `-prg` autoload, and its cycle compensation for the host time a call takes (a call costs 6 cycles here).

## YM2151

`src/plugins/devices/ym2151/` wraps [ymfm](https://github.com/aaronsgiles/ymfm)'s OPM (vendored in `src/third_party/ymfm`, BSD-3-Clause), with x16-emulator's glue (`ymglue.cpp`): 3.579545 MHz, one sample per 64 chip clocks (55,930 Hz), the busy flag and timers counted in those clocks, writes while busy dropped, timer interrupts on the shared IRQ line. VERA mixes it into its output as the X16 does — `(psg + pcm) * 2 + ym` through VERA's limiter — resampling it linearly to 48,828 Hz. BASIC's `FMINIT`, `FMNOTE`, `FMPLAY`... play (`FMNOTE 0,$4A` gives 440 Hz).

## Tests

- `x16_boots_to_ready_and_runs_basic` boots R49 to READY and runs a typed `PRINT 6*7` (skipped without `roms/x16/rom.bin`).
- `test_x16_i2c.cpp`: the I2C bus bit-banged through a real VIA as the KERNAL does it: SMC keyboard buffer and version, RTC time, ticking and NVRAM.
- `x16_basic_reads_gamepad_and_mouse` (ROM): `JOY(1)` with pad buttons held, `MX`/`MY`/`MB` after a mouse move; `test_x16_i2c.cpp` also scans the pads as the KERNAL does and reads mouse packets from the SMC.
- `test_ym2151.cpp`: the busy flag, a keyed-on voice producing sound, timer A raising the IRQ line.
- `test_x16_hostfs.cpp`: HostFS on R49 in a temporary folder: `SAVE` (the host file's bytes), `NEW`/`LOAD`/`RUN`, `DOS"$"`, the error channel, `S:`, `MD`/`CD`, `R:`, `OPEN`/`PRINT#`/`GET#`, and stepping aside while an SD card is mounted (skipped without the ROM).
- `test_sdcard_spi.cpp`: initialisation, a block read and write against an image file, out-of-range reads.
- `test_vera.cpp`: VERA registers, address ports, palette, text layer, sprites, collisions, LINE/VSYNC interrupts, peeks, PSG, PCM and FX.

## Keyboard

Keys go to the SMC as IBM key numbers (x16-emulator `keyboard.c`): the GUI sends PC keys positionally (Shift is a real key), and CLI `key`/`type` and MCP accept the same names (`a`, `return`, `left_shift`, `escape`, `f1`...). Typed text: uppercase letters are unshifted (the X16 starts in the PETSCII upper case / graphics set), lowercase shifted; one key event goes to the SMC whenever its buffer is empty.

## Controllers and mouse

- **SNES pads** (x16-emulator `joystick.c`): four ports on VIA1 port A (latch PA2, clock PA3, data PA7-PA4 for KERNAL joysticks 1-4). The latch loads each pad's 12 buttons and ID bits; each rising clock shifts. A port reads as connected from the first time it is used; an empty port reads all 1s, which the KERNAL reports as not present (`JOY(n)` = -1). Joystick 0 is the KERNAL's keyboard joystick.
- **Mouse** (x16-emulator `i2c.c`/`smc.c`): the SMC's PS/2 mouse, device ID 3 (4-byte packets with the wheel), registers `$21`, `$22`, `$42`, `$43` and `$20`. Movement and buttons go to the SMC once per frame. `MOUSE 1` shows the pointer; `MX`, `MY`, `MB` read it.
- Input: CLI `joy <port> [up down left right a b x y l r select start]` (port 0 = KERNAL joystick 1; `fire` = B; no buttons releases them) and `mouse <dx> <dy> [buttons] [wheel]`; MCP `set_joystick` and `mouse_input`; the GUI sends the host mouse while it is over the picture, and host USB controllers as SNES pads (face buttons by position: bottom = B, right = A, left = Y, top = X; shoulders L/R; Back = SELECT).

## Not yet emulated

- IEC serial bus devices (bit-level; the host folder is served at the KERNAL API instead).
