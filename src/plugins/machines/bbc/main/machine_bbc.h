#pragma once

#include <string>

class MachineDescriptor;

namespace bbc {

/** A: 16 KB; B: 32 KB with DFS 0.90; BTube: a B with DNFS and the 6502
 *  Second Processor on the Tube */
enum class Model { A, B, BTube, BZ80, B6809, BArm };

/**
 * A BBC Micro, after b-em (GPL-2.0-or-later). Model B: a 2 MHz 6502, 32 KB
 * RAM, the OS 1.20 ROM, BASIC 2 in sideways slot 15, the 6845/video ULA/
 * SAA5050 video, the System and User VIAs, the keyboard on the slow data
 * bus, the SN76489, the 8271 with DFS, the cassette, the ADC. Model A: 16 KB,
 * no User VIA, ADC or disc. ROMs and the teletext font come from roms/bbc/.
 */
MachineDescriptor* create(Model model, std::string* err = nullptr);

}  // namespace bbc
