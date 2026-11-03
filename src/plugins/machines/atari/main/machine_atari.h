#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct MachineDescriptor;

/**
 * Atari 8-bit computers (#10): the 800 (48 KB, OS revision B, BASIC
 * cartridge) and the 800XL and 65XE (64 KB, XL OS, built-in BASIC banked by
 * PIA port B). NTSC timing: a 1.79 MHz 6502, ANTIC, GTIA, POKEY and a 6520
 * PIA. ROMs from roms/atari/ (osb.bin, osxl.bin, basic.bin).
 */
namespace atari {

enum class Model { Atari800, Atari800XL, Atari65XE };

/**
 * Build a machine; nullptr if its ROMs are missing (the reason in `err`).
 * `pal`: PAL timing (1.77 MHz, 312 lines, 50 Hz) and colours; the machine
 * id gets "-pal".
 */
MachineDescriptor* create(Model model, std::string* err = nullptr, bool pal = false);

/** CPU clock: NTSC and PAL. */
constexpr unsigned CLOCK_HZ = 1789773;
constexpr unsigned CLOCK_HZ_PAL = 1773447;

/**
 * Put an ATR disk image in drive 1 and boot it: the machine is reset with
 * OPTION held (on the XL/XE: BASIC off, as games expect), released once the
 * OS has read it. `path`: the file, which writes are saved back to (empty:
 * writes stay in memory).
 */
bool bootDisk(MachineDescriptor* m, std::vector<uint8_t> atr, const std::string& label, std::string& err,
              const std::string& path = "");

/** Boot an executable (.xex) from a generated disk (see xexToAtr). */
bool bootExecutable(MachineDescriptor* m, const std::vector<uint8_t>& xex,
                    const std::string& label, std::string& err);

/**
 * Insert a cartridge (.car, or a raw 8/16 KB ROM; see Cartridge::load) and
 * cold-start with it. It replaces the 800's BASIC cartridge and overrides
 * the XL/XE's built-in BASIC.
 */
bool insertCartridge(MachineDescriptor* m, const std::vector<uint8_t>& image,
                     const std::string& label, std::string& err);
/** Take the cartridge out and cold-start. */
void removeCartridge(MachineDescriptor* m);
/** The cartridge in the slot ("name (type)"), or "" if none. */
std::string cartridgeInfo(MachineDescriptor* m);

}  // namespace atari
