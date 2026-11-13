#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct MachineDescriptor;

namespace coco {

/**
 * CoCo 1 (MC6847, 0.89 MHz), CoCo 2 (MC6847), CoCo 2B (MC6847T1: lower
 * case); the Dragon 32 and Dragon 64 (Dragon Data's: the same chips, their
 * own keyboard matrix, ROM, printer port and, on the 64, a 6551 and a
 * second BASIC).
 */
enum class Model { CoCo1, CoCo2, CoCo2B, Dragon32, Dragon64 };

inline bool isDragon(Model m) { return m == Model::Dragon32 || m == Model::Dragon64; }

/**
 * The television standard: NTSC (60 Hz, 262 lines, 14.31818 MHz) or PAL
 * (50 Hz: the board pads the VDG to 312 lines; 14.218 MHz on a Dragon,
 * 14.23753 MHz on a PAL CoCo; no artifact colours). Default: the model's
 * own (NTSC for a CoCo, PAL for a Dragon).
 */
enum class Tv { Default, NTSC, PAL };

/** ROM images to use instead of the files (tests' stand-ins). */
struct Roms {
    std::vector<uint8_t> colorBasic;   ///< 8 KB at $A000 (required on a CoCo)
    std::vector<uint8_t> extBasic;     ///< 8 KB at $8000 (optional)
    std::vector<uint8_t> dragonBasic;  ///< Dragon: 16 KB at $8000 (required; the 64's 32K-mode BASIC)
    std::vector<uint8_t> dragon64k;    ///< Dragon 64: the 16 KB 64K-mode BASIC (PIA1 PB2 low)
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
 *
 * The Dragons' ROMs are Microsoft's and Dragon Data's, from
 * $MMEMU_DRAGON_FIRMWARE or roms/dragon/: the Dragon 32's d32.rom, the
 * Dragon 64's d64_1.rom (32K mode) and d64_2.rom (64K mode), 16 KB each;
 * DragonDOS ddos12a.rom (or ddos12, ddos10...) for its cartridge. A
 * Dragon 64 with `tv` NTSC is the Tano Dragon.
 */
MachineDescriptor* create(Model model, std::string* err = nullptr, int ramKb = 64, const Roms* roms = nullptr,
                          Tv tv = Tv::Default);

/** Plug a ROM Pak (.rom / .ccc image) into the cartridge port (or, with a
 *  Multi-Pak fitted, the slot its switch selects). */
bool insertRomPak(MachineDescriptor* m, const std::vector<uint8_t>& image, const std::string& label, std::string& err);
/** Take the ROM Pak out (from the port or the Multi-Pak). */
void removeRomPak(MachineDescriptor* m);
/** What is in the cartridge port ("" for nothing). */
std::string cartridgeInfo(MachineDescriptor* m);

/** Where the ROM files are looked for. */
std::string firmwareDir();
/** Where the Dragon's ROM files are looked for ($MMEMU_DRAGON_FIRMWARE or roms/dragon/). */
std::string dragonFirmwareDir();

}  // namespace coco
