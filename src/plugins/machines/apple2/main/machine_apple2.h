#pragma once

#include <string>

struct MachineDescriptor;

/**
 * Apple II and II+ (#12), after AppleWin (GPL-2.0-or-later; Memory.cpp,
 * LanguageCard.cpp, Keyboard.cpp, Speaker.cpp, Joystick.cpp): a MOS 6502 at
 * 1.0205 MHz, 48 KB RAM, a 16 KB language card in slot 0, the soft
 * switches at $C000-$C07F, the keyboard latch, the speaker, paddles and
 * buttons, the floating bus, and the video (Apple2Video, a port of
 * AppleWin's NTSC renderer).
 *
 * ROMs (AppleWin's resource images): roms/apple2/Apple2.rom (Integer BASIC
 * and the original monitor), Apple2_Plus.rom (Applesoft and the autostart
 * monitor), 12 KB at $D000-$FFFF; Apple2_Video.rom, the character
 * generator.
 *
 * IIe and enhanced IIe: 64 KB main and 64 KB auxiliary RAM (an extended
 * 80-column card in the aux slot), the internal $C100-$CFFF ROM, the IIe
 * soft switches (80STORE, RAMRD, RAMWRT, INTCXROM, ALTZP, SLOTC3ROM, 80COL,
 * ALTCHARSET, double hi-res) and their status reads, lower case, the Apple
 * keys; the enhanced IIe has a 65C02. ROMs: Apple2e.rom /
 * Apple2e_Enhanced.rom (16 KB at $C000), Apple2e_Enhanced_Video.rom.
 *
 * IIc (after MAME apple2e.cpp): an enhanced IIe with 128 KB, the internal
 * $C100-$CFFF ROM always in, the IOU's interrupts (VBL, mouse) and its
 * switches, two 6551 ACIAs (the serial ports, slots 1 and 2) and the drive
 * (Disk II compatible) in slot 6. ROM: roms/apple2/apple2c-ff.rom (the
 * original 16 KB ROM; local only, see doc/README-APPLE2.md). $C028 banks a
 * 32 KB ROM's halves, but those ROMs (00, 03, 04) also need the 3.5"
 * drive or the memory expansion, not emulated yet.
 */
namespace apple2 {

enum class Model { II, IIPlus, IIe, IIeEnhanced, IIc, IIgs };

/** The Apple IIgs (machine_apple2gs.cpp): ROM 01, roms/apple2/apple2gs-rom01.rom (local only). */
MachineDescriptor* createIIgs(std::string* err = nullptr);

/** A new machine, or nullptr with `err` set (e.g. a ROM missing). */
MachineDescriptor* create(Model model, std::string* err = nullptr);

}  // namespace apple2
