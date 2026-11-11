#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct MachineDescriptor;

/**
 * The Game Boy (DMG) as a machine (`gameboy`, also `dmg`): the core
 * gb::Gameboy (gameboy.h, after Mesen2) with its SM83 as the debugger's CPU
 * (sm83_core.h), the 160x144 LCD as the screen (four shades, the DMG's
 * green), its sound (mono, 44.1 kHz) and the joypad:
 *
 *   keys     arrows, X (A), Z (B), Return (Start), Tab or Shift (Select)
 *   gamepad  the d-pad, A/X (A), B/Y (B), START, SELECT
 *   joystick the directions, fire (A)
 *
 * Cartridges (.gb; .gbc ones run if they work on the DMG): ROM only, MBC1,
 * MBC2, MBC3 (with its clock) and MBC5. Battery RAM is kept beside the ROM
 * in `<rom>.sav` (read from `<rom>.srm` too, Mesen's name), the MBC3's clock
 * in `<rom>.rtc` (Mesen's), written within a second of a change and on eject.
 *
 * Without a boot ROM the machine starts where the DMG's leaves off (PC $0100);
 * roms/gameboy/dmg_boot.bin (256 bytes, your own; not included), if present,
 * is run instead.
 */
namespace gb {

class Gameboy;

/** A new Game Boy (no cartridge). */
MachineDescriptor* createMachine(std::string* err = nullptr);

/**
 * Plug a cartridge in (a .gb image) and power on. `savePath`: where its
 * battery RAM lives ("" for nowhere). False with `err` if `m` is not a Game
 * Boy or the cartridge's mapper is not supported.
 */
bool insertCartridge(MachineDescriptor* m, const std::vector<uint8_t>& image, const std::string& label,
                     std::string& err, const std::string& savePath = "");
void removeCartridge(MachineDescriptor* m);
/** "<label> (<mapper>, <title>)", or "". */
std::string cartridgeInfo(MachineDescriptor* m);
/** Write the battery RAM (and clock) if they changed. */
void flushSave(MachineDescriptor* m);
/** `<rom path without its extension>.sav` */
std::string savePathFor(const std::string& romPath);

/** The machine's Game Boy, or null if `m` is not one. */
Gameboy* gameboyOf(MachineDescriptor* m);
/** The joypad: Button bits (gameboy.h) held. */
void setButtons(MachineDescriptor* m, uint8_t buttons);

}  // namespace gb
