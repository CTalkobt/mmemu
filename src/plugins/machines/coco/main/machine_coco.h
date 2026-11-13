#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct MachineDescriptor;

namespace coco {

/** CoCo 1 (MC6847, 0.89 MHz), CoCo 2 (MC6847), CoCo 2B (MC6847T1: lower case). */
enum class Model { CoCo1, CoCo2, CoCo2B };

/** ROM images to use instead of the files (tests' stand-ins). */
struct Roms {
    std::vector<uint8_t> colorBasic;   ///< 8 KB at $A000 (required)
    std::vector<uint8_t> extBasic;     ///< 8 KB at $8000 (optional)
};

/**
 * A TRS-80 Color Computer 1, 2 or 2B: the MC6809E, the MC6883 SAM, the
 * MC6847 (T1) VDG, two MC6821 PIAs, `ramKb` of RAM (4, 16, 32 or 64),
 * Color BASIC and, if present, Extended Color BASIC.
 *
 * The ROMs are the user's (Microsoft / Tandy copyright), from
 * $MMEMU_COCO_FIRMWARE or roms/coco/: Color BASIC bas13.rom, bas12.rom,
 * bas11.rom or bas10.rom (8 KB; the CoCo 1 prefers 1.2); Extended Color
 * BASIC extbas11.rom or extbas10.rom (8 KB, optional); Disk BASIC
 * disk11.rom or disk10.rom for the FD-500. `roms` (non-null) replaces the
 * files. Without Color BASIC the machine is made with a stand-in ROM that
 * says so on the screen, `err` and a warning naming the file.
 */
MachineDescriptor* create(Model model, std::string* err = nullptr, int ramKb = 64, const Roms* roms = nullptr);

/** Plug a ROM Pak (.rom / .ccc image) into the cartridge port (or, with a
 *  Multi-Pak fitted, the slot its switch selects). */
bool insertRomPak(MachineDescriptor* m, const std::vector<uint8_t>& image, const std::string& label, std::string& err);
/** Take the ROM Pak out (from the port or the Multi-Pak). */
void removeRomPak(MachineDescriptor* m);
/** What is in the cartridge port ("" for nothing). */
std::string cartridgeInfo(MachineDescriptor* m);

/** Where the ROM files are looked for. */
std::string firmwareDir();

}  // namespace coco
