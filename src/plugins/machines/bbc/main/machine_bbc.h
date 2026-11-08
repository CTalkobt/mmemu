#pragma once

#include <string>

class MachineDescriptor;

namespace bbc {

enum class Model { B };

/**
 * A BBC Micro, after b-em (GPL-2.0-or-later). Model B: a 2 MHz 6502, 32 KB
 * RAM, the OS 1.20 ROM, BASIC 2 in sideways slot 15, the 6845/video ULA/
 * SAA5050 video, the System and User VIAs, the keyboard on the slow data
 * bus, the SN76489. ROMs and the teletext font come from roms/bbc/.
 */
MachineDescriptor* create(Model model, std::string* err = nullptr);

}  // namespace bbc
