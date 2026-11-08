#pragma once

#include <string>

class MachineDescriptor;

namespace spectrum {

/** Spectrum models (#242). */
enum class Model { S48, S128, Plus2 };

/**
 * The Sinclair ZX Spectrum, after Fuse (GPL-2.0-or-later): a Z80 timed
 * per bus cycle against the ULA's contended memory and I/O, the ULA's
 * 256 x 192 screen with its border drawn as the beam passes, port $FE
 * (keyboard, border, beeper, MIC/EAR), the floating bus, a Kempston
 * joystick at $1F. The 128K and +2 add $7FFD paging. ROMs come from
 * roms/spectrum/. See doc/README-SPECTRUM.md.
 */
MachineDescriptor* create(Model model, std::string* err = nullptr);

}  // namespace spectrum
