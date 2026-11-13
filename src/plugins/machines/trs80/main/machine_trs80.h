#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct MachineDescriptor;

namespace trs80 {

/**
 * The Z80 TRS-80s: the Model I (Level I or Level II BASIC), the Model III,
 * the Model 4 and the Model 4P.
 */
enum class Model { M1Level1, M1Level2, M3, M4, M4P };

inline bool isModel1(Model m) { return m == Model::M1Level1 || m == Model::M1Level2; }
inline bool isModel4(Model m) { return m == Model::M4 || m == Model::M4P; }

/** ROM images to use instead of the files (tests' stand-ins). */
struct Roms {
    std::vector<uint8_t> rom;        ///< Level I (4 KB), Level II (12 KB), Model III/4 (14 KB), 4P boot (4 KB)
    std::vector<uint8_t> charGen;    ///< optional: a character generator ROM (1 KB Model I, 2 KB Model III/4)
};

/**
 * Make a TRS-80. The ROMs are the user's (Tandy / Microsoft copyright),
 * from $MMEMU_TRS80_FIRMWARE or roms/trs80/:
 *  - Model I Level I: level1.rom (4 KB);
 *  - Model I Level II: level2.rom (12 KB), or MAME's rom-a.z1, rom-b.z2,
 *    rom-c.z3 (4 KB each);
 *  - Model III: model3.rom (14 KB), or MAME's 8041364.u104 (8 KB),
 *    8040332.u105 (4 KB) and 8040316c.u106 (or 8040316b.u106, 2 KB);
 *  - Model 4: model4.rom (14 KB; MAME's trs80m4.rom), else the Model III's;
 *  - Model 4P: model4p.rom (the 4 KB boot ROM; MAME's 8075332.u69).
 * Optional character generators: mcm6670p.z29 (Model I, 1 KB) and
 * 8044316a.u36 (Model III/4) or 8049007.u103 (4P), 2 KB; without them a
 * built-in font is used.
 * `roms` (non-null) replaces the files. Without its ROM the machine is
 * made with a stand-in that says so on the screen, `err` and a warning
 * naming the file.
 */
MachineDescriptor* create(Model model, std::string* err = nullptr, const Roms* roms = nullptr);

/** Where the ROM files are looked for. */
std::string firmwareDir();

/** Load a TRS-80 /CMD program (DOS load module) into a machine made by
 *  create(): its load blocks into memory, PC at its transfer address. */
bool loadCmd(MachineDescriptor* m, const std::vector<uint8_t>& image, std::string& err);

}  // namespace trs80
