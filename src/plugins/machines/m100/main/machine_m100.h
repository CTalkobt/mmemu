#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct MachineDescriptor;

namespace m100 {

/**
 * The Kyocera-built portables: Tandy Model 100, Model 102, Tandy 200, and
 * their siblings the NEC PC-8201A, the Olivetti M10 and the Kyotronic 85.
 */
enum class Model { M100, M102, M200, PC8201, M10, KC85 };

/** ROM images to use instead of the files (tests' stand-ins). */
struct Roms {
    std::vector<uint8_t> rom;       ///< 32 KB (all but the 200); 200: 72 KB (main 40 KB then the second 32 KB)
    std::vector<uint8_t> optRom;    ///< optional: an option ROM (up to 32 KB)
};

/**
 * Make a Model 100, 102 or 200. The ROMs are the user's (Microsoft /
 * Tandy / Kyocera copyright), from $MMEMU_M100_FIRMWARE or roms/m100/:
 *  - Model 100: m100.rom (32 KB; also MAME's m100rom.m12, VirtualT's M100rom.bin);
 *  - Model 102: m102.rom (32 KB; m102rom.m12, M102rom.bin);
 *  - Model 200: t200.rom (72 KB: VirtualT's T200rom.bin layout, the 40 KB
 *    main ROM then the 32 KB second ROM), or MAME's three chips "rom 1-1.m15"
 *    (32 KB), "rom 1-2.m13" (8 KB) and "rom 2.m14" (32 KB);
 *  - NEC PC-8201A: pc8201a.rom (32 KB; MAME's pc8201rom.rom, VirtualT's PC8201rom.bin);
 *  - Olivetti M10: m10.rom (32 KB; m10rom.m12, VirtualT's M10rom.bin, whose
 *    16 bytes past 32 KB are dropped);
 *  - Kyotronic 85: kc85.rom (32 KB; kc85rom.bin, KC85rom.bin).
 * `roms` (non-null) replaces the files. Without its ROM the machine is made
 * with a stand-in that says so on the LCD, `err` and a warning naming the
 * file. `ramKb` 0 is the most RAM the model takes (32 KB; the 200: 72 KB;
 * the PC-8201A: 96 KB).
 */
MachineDescriptor* create(Model model, std::string* err = nullptr, const Roms* roms = nullptr, int ramKb = 0);

/** Where the ROM files are looked for. */
std::string firmwareDir();

/** The stand-in ROM: a message on the LCD (no copyrighted code). */
std::vector<uint8_t> placeholderRom(Model model, const std::string& message);

/** Fit an option ROM (empty removes it); false with `err` if it doesn't fit. */
bool insertOptionRom(MachineDescriptor* m, const std::vector<uint8_t>& image, const std::string& label,
                     std::string& err);

}  // namespace m100
