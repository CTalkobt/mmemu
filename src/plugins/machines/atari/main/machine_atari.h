#pragma once

#include <string>

struct MachineDescriptor;

/**
 * Atari 8-bit computers (#10): the 800 (48 KB, OS revision B, BASIC
 * cartridge) and the 800XL and 65XE (64 KB, XL OS, built-in BASIC banked by
 * PIA port B). NTSC timing: a 1.79 MHz 6502, ANTIC, GTIA, POKEY and a 6520
 * PIA. ROMs from roms/atari/ (osb.bin, osxl.bin, basic.bin).
 */
namespace atari {

enum class Model { Atari800, Atari800XL, Atari65XE };

/** Build a machine; nullptr if its ROMs are missing (the reason in `err`). */
MachineDescriptor* create(Model model, std::string* err = nullptr);

/** CPU clock (NTSC), cycles a line and lines a frame. */
constexpr unsigned CLOCK_HZ = 1789773;

}  // namespace atari
