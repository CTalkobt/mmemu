#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct MachineDescriptor;

/**
 * Commodore Plus/4 and C16 (#9): a 7501/8501 CPU (a 6502 with an I/O port at
 * $00/$01) clocked by the TED (MOS 7360/8360), which also does video, sound,
 * timers and the keyboard/joystick input. Memory map and ROM banking after
 * VICE's plus4mem.c:
 *
 *   $0000-$7FFF  RAM (C16: 16 KB mirrored)
 *   $8000-$BFFF  RAM or the low ROM bank: BASIC 3.5, 3-plus-1 low, C1 low, C2 low
 *   $C000-$FFFF  RAM or the high ROM bank: KERNAL, 3-plus-1 high, C1 high, C2 high
 *                ($FC00-$FCFF is always the KERNAL while ROM is on)
 *   $FD00-$FF3F  always I/O: $FD00 ACIA (Plus/4), $FD10 6529 user port,
 *                $FD30 6529 keyboard row latch, $FDD0-$FDDF ROM bank select
 *                (address bits 0-1 low bank, 2-3 high bank: 0 BASIC/KERNAL,
 *                1 3-plus-1, 2 C1 = the cartridge port, 3 C2), $FF00-$FF3F TED
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

/**
 * Plug a cartridge in and reset (the KERNAL starts it if it marks itself
 * so): a raw 8/16 KB (C1 low) or 32 KB (C1 low and high) image, or a VICE
 * .crt ("PLUS4 CARTRIDGE ", generic). False with `err` if it is not one, or
 * `m` is not a Plus/4, C16 or C116. The CLI `cart`, the GUI's Attach
 * Cartridge, MCP attach_cartridge.
 */
bool insertCartridge(MachineDescriptor* m, const std::vector<uint8_t>& image,
                     const std::string& label, std::string& err);
/** Take the cartridge out and reset. */
void removeCartridge(MachineDescriptor* m);
/** "<label> (<type>)", or "" with no cartridge. */
std::string cartridgeInfo(MachineDescriptor* m);

} // namespace plus4
