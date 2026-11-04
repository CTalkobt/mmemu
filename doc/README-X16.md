# Commander X16 (`x16`)

The Commander X16 (#16): a WDC 65C02 at 8 MHz, VERA (video and sound), two 65C22 VIAs, and the I2C system management controller (keyboard) and real-time clock. The machine plugin is `src/plugins/machines/x16/` (`mmemu-plugin-x16.so`); VERA is `src/plugins/devices/vera/`. The memory map, I/O decode and devices follow [x16-emulator](https://github.com/X16Community/x16-emulator) (2-clause BSD), the reference the X16 ROM is tested against.

```
mmemu-gui -m x16
mmemu-cli -m x16 --speed max
```

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
| `$9F40-$9F5F` | YM2151 (not emulated yet: the status register reads 0, never busy; 3 wait states) |
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

## Keyboard

Keys go to the SMC as IBM key numbers (x16-emulator `keyboard.c`): the GUI sends PC keys positionally (Shift is a real key), and CLI `key`/`type` and MCP accept the same names (`a`, `return`, `left_shift`, `escape`, `f1`...). Typed text: uppercase letters are unshifted (the X16 starts in the PETSCII upper case / graphics set), lowercase shifted; one key event goes to the SMC whenever its buffer is empty.

## Not yet emulated

- YM2151 FM sound.
- SD card (VERA SPI), IEC devices, mouse, NES/SNES controllers.
