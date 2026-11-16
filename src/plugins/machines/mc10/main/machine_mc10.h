#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct MachineDescriptor;

namespace mc10 {

/**
 * The TRS-80 MC-10 Micro Color Computer (Tandy, 1983; NTSC) and its French
 * twin the Matra Alice (Matra & Hachette, 1983: PAL, an AZERTY keyboard);
 * the Alice 32 (1984) and Alice 90 (1985), with the EF9345 instead of the
 * VDG, 16 KB of ROM at $C000 and 8 KB / 32 KB of RAM at $3000.
 */
enum class Model { MC10, Alice, Alice32, Alice90 };

/** The Alice 32 and 90 (the EF9345). */
inline bool hasEf9345(Model m) { return m == Model::Alice32 || m == Model::Alice90; }

/** ROM images to use instead of the files (tests' stand-ins). */
struct Roms {
    std::vector<uint8_t> basic;     ///< 8 KB at $E000 (required); the Alice 32/90's 16 KB at $C000
    std::vector<uint8_t> mcx128;    ///< the MCX-128's 16 KB EPROM (optional)
    std::vector<uint8_t> charset;   ///< the EF9345's 8 KB character ROM (the Alice 32/90)
};

/** The expansion port's device at power on. */
enum class Expansion { None, RamPack16K, Mcx128 };

/**
 * An MC-10 or Alice: the MC6803 at 0.89 MHz (mode 2), the MC6847 VDG, 4 KB
 * of RAM at $4000, the 8 KB Micro Color BASIC ROM at $E000, the keyboard,
 * 1-bit sound, the cassette and the serial printer port; `expansion` in the
 * expansion port (by default the 16K RAM pack: 20 KB in all).
 *
 * The ROMs are the user's (Microsoft / Tandy copyright), from
 * $MMEMU_MC10_FIRMWARE or roms/mc10/: mc10.rom (Micro Color BASIC 1.0,
 * 8 KB), alice.rom for the Alice, mcx128bas.rom (16 KB) for the MCX-128,
 * alice32.rom (16 KB, the Alice 32's and 90's) and the EF9345's
 * character ROM ef9345.rom (8 KB; MAME's charset.rom is taken too).
 * `roms` (non-null) replaces the files. Without BASIC the machine is made
 * with a stand-in ROM that says so on the screen, `err` and a warning
 * naming the file.
 */
MachineDescriptor* create(Model model, std::string* err = nullptr, Expansion expansion = Expansion::RamPack16K,
                          const Roms* roms = nullptr);

/** Where the ROM files are looked for ($MMEMU_MC10_FIRMWARE or roms/mc10/). */
std::string firmwareDir();

}  // namespace mc10
