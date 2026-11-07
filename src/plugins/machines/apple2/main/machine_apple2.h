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
 */
namespace apple2 {

enum class Model { II, IIPlus, IIe, IIeEnhanced };

/** A new machine, or nullptr with `err` set (e.g. a ROM missing). */
MachineDescriptor* create(Model model, std::string* err = nullptr);

}  // namespace apple2
