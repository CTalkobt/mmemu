#pragma once

#include <string>

class MachineDescriptor;

namespace electron {

/**
 * The Acorn Electron, after Elkulator (GPL-2.0-or-later): a 6502 at 2 MHz
 * with 32 KB of RAM the ULA shares (the CPU waits for it), OS 1.00 and
 * BASIC 2 in paged ROM, the ULA's Modes 0-6, interrupts, square-wave sound
 * and cassette, and the keyboard read through paged ROM. ROMs come from
 * roms/electron/.
 */
MachineDescriptor* create(std::string* err = nullptr);

}  // namespace electron
