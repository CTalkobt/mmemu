#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct MachineDescriptor;

/**
 * Commodore 128 (#22), in C128 mode with the 40-column VIC-IIe screen.
 * Memory map and MMU (8722) after VICE (src/c128/c128mem.c, c128meminit.c,
 * c128mmu.c), GPL-2.0-or-later:
 *
 *   CR ($FF00 / $D500): bit 0 I/O off, bit 1 $4000-$7FFF RAM (else BASIC
 *   low), bits 2-3 $8000-$BFFF (BASIC high, internal / external function
 *   ROM, RAM), bits 4-5 $C000-$FFFF (KERNAL with the screen editor at $C000
 *   and the character ROM at $D000 while I/O is off, function ROMs, RAM),
 *   bits 6-7 the RAM bank. $FF00-$FF04 are always the MMU: CR, and the
 *   load-configuration registers that copy PCR A-D into CR.
 *
 *   RCR ($D506): shared RAM (1, 4, 8 or 16 KB at the bottom and/or top, in
 *   bank 0 whatever the CR bank), and the VIC-II's RAM bank (bit 6).
 *   P0L/P0H, P1L/P1H ($D507-$D50A): where pages 0 and 1 are (they swap with
 *   the target page); the high byte is latched until the low one is written.
 *
 * I/O ($D000-$DFFF with CR bit 0 clear): VIC-IIe, SID, MMU ($D500-$D50B),
 * VDC ($D600), 2 KB colour RAM (the 8502 port picks the CPU's and the VIC's
 * 1 KB halves), CIA1, CIA2.
 *
 * The Z80 starts the machine from its boot BIOS and hands over to the 8502
 * through MCR bit 0 (CP/M switches back and forth). Function ROMs: internal
 * (the board's socket) and external (a cartridge), 32 KB each. Not yet: 1571
 * fast serial, bank-switching cartridges.
 *
 * ROMs: roms/c128/basic-4000.318018-04.bin, basic-8000.318019-04.bin,
 * kernal.318020-05.bin (editor, Z80 BIOS, KERNAL), characters.390059-01.bin
 * (C64 set, then the C128 set), c64.251913-01.bin (C64 mode).
 */
namespace c128 {

/** A new C128 (PAL, or NTSC), or nullptr with `err` set (e.g. a ROM missing). */
MachineDescriptor* create(bool ntsc = false, std::string* err = nullptr);

/**
 * Plug a cartridge (an external function ROM) in and reset: a raw 1-32 KB
 * image or a VICE .crt ("C128 CARTRIDGE  ", generic). The KERNAL starts it
 * if it has "CBM" at $8007 (or $C007) and $01 at $8006. False with `err`.
 */
bool insertCartridge(MachineDescriptor* m, const std::vector<uint8_t>& image,
                     const std::string& label, std::string& err);
/** Take the cartridge out and reset. */
void removeCartridge(MachineDescriptor* m);
/** "<label> (<type>)", or "" with no cartridge. */
std::string cartridgeInfo(MachineDescriptor* m);
/** Fit (or with an empty image, remove) the internal function ROM, the
 *  socket on the board (same formats as a cartridge). */
bool setInternalFunctionRom(MachineDescriptor* m, const std::vector<uint8_t>& image, std::string& err);

}  // namespace c128
