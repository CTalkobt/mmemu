#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct MachineDescriptor;

/**
 * NES / Famicom (#14): the Ricoh 2A03 (a 6502 without decimal mode, plus
 * the APU, OAM DMA and the controller ports) at 1.789773 MHz, the 2C02 PPU
 * at three dots per CPU cycle, 2 KB of RAM and a cartridge (iNES / NES 2.0;
 * mappers 0, 1, 2, 3, 4, 7), after Mesen2. The CPU sees:
 *
 *   $0000-$1FFF  2 KB of RAM, mirrored
 *   $2000-$3FFF  the PPU's eight registers, mirrored
 *   $4000-$4017  the APU, OAM DMA ($4014), controllers ($4016/$4017)
 *   $4020-$FFFF  the cartridge (work RAM at $6000, PRG ROM at $8000)
 *
 * The CPU (nes_cpu.h) is cycle-exact: every cycle is a bus access, the PPU
 * and APU run in step with it, and OAM DMA and the DMC's fetches take the
 * CPU's read cycles as on the chip.
 *
 * The standard controllers: onGamepad (PAD_A/PAD_B/SELECT/START and the
 * d-pad; X and Y also press A and B), onJoystick (fire is A), and the keys
 * arrows, X (A), Z (B), Return (Start), Tab or Shift (Select).
 */
namespace nes {

/** A new NES (or the Famicom, the same machine here). */
MachineDescriptor* create(bool famicom = false, std::string* err = nullptr);

/** Plug a cartridge in (an iNES image) and reset. False with `err` if `m`
 *  is not an NES or the image is not a supported cartridge. */
bool insertCartridge(MachineDescriptor* m, const std::vector<uint8_t>& image,
                     const std::string& label, std::string& err);
void removeCartridge(MachineDescriptor* m);
/** "<label> (<mapper, sizes>)", or "". */
std::string cartridgeInfo(MachineDescriptor* m);

/** Controller buttons, in the order the 4021 shift register sends them. */
enum Button : uint8_t {
    BTN_A = 0x01, BTN_B = 0x02, BTN_SELECT = 0x04, BTN_START = 0x08,
    BTN_UP = 0x10, BTN_DOWN = 0x20, BTN_LEFT = 0x40, BTN_RIGHT = 0x80,
};
/** Set controller `port` (0 or 1) to the BTN_* bits held. */
void setButtons(MachineDescriptor* m, int port, uint8_t buttons);

}  // namespace nes
