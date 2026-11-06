#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct MachineDescriptor;

/**
 * Atari 2600 (VCS) (#11): a 6507 (a 6502 with 13 address lines) at 1/3 of
 * the colour clock, the TIA (video, sound, fire buttons, paddles) and the
 * RIOT 6532 (128 bytes of RAM, joysticks and console switches, the timer),
 * after Stella. The 6507 sees:
 *
 *   A12 = 1         the cartridge ($1000-$1FFF), bank-switched
 *   A12 = 0, A7 = 0 the TIA (writes $00-$3F, reads $00-$0F; mirrored)
 *   A12 = 0, A7 = 1 the RIOT: A9 = 0 RAM, A9 = 1 the ports and timer
 *
 * The TIA and RIOT are clocked lazily: reads catch them up to the cycle of
 * the access; writes take effect on the instruction's last cycle(s), where
 * the 6502 writes; WSYNC holds the CPU until the line ends.
 *
 * Cartridges (.a26/.bin), detected as Stella does: 2K, 4K, F8, F6, F4 (each
 * with the Superchip's 128 bytes of RAM, "SC"), E0 (Parker Brothers), 3F
 * (Tigervision), FA (CBS RAM Plus).
 */
namespace a2600 {

/** A new 2600 (NTSC or PAL). */
MachineDescriptor* create(bool pal = false, std::string* err = nullptr);

/** Plug a cartridge in (bank-switching type detected) and reset. False
 *  with `err` if `m` is not a 2600 or the image is not a cartridge. */
bool insertCartridge(MachineDescriptor* m, const std::vector<uint8_t>& image,
                     const std::string& label, std::string& err);
void removeCartridge(MachineDescriptor* m);
/** "<label> (<type>)", or "". */
std::string cartridgeInfo(MachineDescriptor* m);

/** The bank-switching type Stella would pick for an image ("4K", "F8SC"...). */
std::string detectType(const std::vector<uint8_t>& image);

}  // namespace a2600
