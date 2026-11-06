#pragma once

#include <string>

struct MachineDescriptor;

/**
 * Commodore Plus/4 and C16 (#9): a 7501/8501 CPU (a 6502 with an I/O port at
 * $00/$01) clocked by the TED (MOS 7360/8360), which also does video, sound,
 * timers and the keyboard/joystick input. Memory map and ROM banking after
 * VICE's plus4mem.c:
 *
 *   $0000-$7FFF  RAM (C16: 16 KB mirrored)
 *   $8000-$BFFF  RAM or the low ROM bank: BASIC 3.5, 3-plus-1 low, cartridges
 *   $C000-$FFFF  RAM or the high ROM bank: KERNAL, 3-plus-1 high, cartridges
 *                ($FC00-$FCFF is always the KERNAL while ROM is on)
 *   $FD00-$FF3F  always I/O: $FD00 ACIA (Plus/4), $FD10 6529 user port,
 *                $FD30 6529 keyboard row latch, $FDD0-$FDDF ROM bank select
 *                (address bits 0-1 low bank, 2-3 high bank), $FF00-$FF3F TED
 *                ($FF3E ROM on, $FF3F RAM on)
 *
 * ROMs: roms/plus4/basic.318006-01.bin, kernal.318004-05.bin (PAL) or
 * kernal.318005-05.bin (NTSC), 3-plus-1.317053-01.bin / 317054-01.bin.
 */
namespace plus4 {

/** C116: a C16 in a smaller case with a rubber keyboard; the same board
 *  logic, ROMs and 16 KB (VICE: "C16/116"). */
enum class Model { Plus4, C16, C116 };

/** A new machine, or nullptr with `err` set (e.g. a ROM missing). */
MachineDescriptor* create(Model model, bool ntsc = false, std::string* err = nullptr);

} // namespace plus4
