#pragma once

#include <string>

struct MachineDescriptor;

/**
 * Commander X16 (#16): WDC 65C02 at 8 MHz, VERA (video, PSG, PCM), two
 * VIAs, the I2C SMC (keyboard) and RTC, 512 KB banked RAM, 32 x 16 KB ROM
 * banks (roms/x16/rom.bin, the R49 release image).
 *
 * Memory: $0000-$9EFF RAM ($00 = RAM bank, $01 = ROM bank, both also RAM);
 * $9F00 VIA1, $9F10 VIA2, $9F20-$9F3F VERA, $9F40/$9F41 YM2151 (not
 * emulated yet: status reads 0); $A000-$BFFF banked RAM; $C000-$FFFF
 * banked ROM. After x16-emulator's memory.c.
 */
namespace x16 {

constexpr unsigned CPU_HZ = 8000000;
constexpr unsigned RAM_BANKS = 64;              // 512 KB, the emulator's default

/** A new X16, or nullptr with `err` set (e.g. no ROM). */
MachineDescriptor* create(std::string* err = nullptr);

} // namespace x16
