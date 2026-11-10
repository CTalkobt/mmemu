#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct MachineDescriptor;

/**
 * Super Nintendo / Super Famicom (#15), after Mesen2 (GPL-3.0): the 5A22
 * (the WDC 65C816 core with the console's multiply/divide, timers, DMA and
 * HDMA, joypad ports), 128 KB of work RAM, the PPU (64 KB VRAM, CGRAM,
 * OAM), the sound system (SPC700, 64 KB ARAM, S-DSP) and a cartridge
 * (LoROM, HiROM, ExHiROM; battery RAM saved to `.srm`; DSP-1..4, ST010/011, Super FX,
 * SA-1, Cx4, S-DD1, SPC7110, OBC1, ST018).
 *
 * Time is the 21.477 MHz master clock: each CPU bus cycle takes 6, 8 or 12
 * clocks by region (FastROM through MEMSEL), internal cycles 6. A line is
 * 1364 clocks (341 dots), 262 lines: DRAM refresh steals 40 clocks a line,
 * each visible line is drawn at the start of horizontal blank followed by
 * HDMA, vertical blank (NMI, auto joypad read) starts at line 225.
 *
 * Controllers: onGamepad (all twelve buttons), onJoystick (fire is B),
 * and the keyboard for pad 1: arrows, X (A), Z (B), S (X), A (Y), Q (L),
 * W (R), Return (Start), Tab or Shift (Select).
 */
namespace snes {

/** A new SNES (NTSC) or Super Famicom (the same machine here). */
MachineDescriptor* create(bool superFamicom = false, std::string* err = nullptr);

/**
 * Plug a cartridge in (.sfc / .smc, a copier header is skipped) and power on.
 * `savePath`: where a battery-backed cartridge keeps its RAM (Mesen's and
 * snes9x's `<rom>.srm`): loaded now, written back within a second of a
 * change and when the cartridge is removed. Empty: not saved.
 */
bool insertCartridge(MachineDescriptor* m, const std::vector<uint8_t>& image, const std::string& label, std::string& err,
                     const std::string& savePath = "");
/** Unplug the cartridge (its battery RAM is saved first). */
void removeCartridge(MachineDescriptor* m);
/** Write the battery RAM out now if it changed (also done once a second). */
void flushSave(MachineDescriptor* m);
/** `<rom path without its extension>.srm` */
std::string savePathFor(const std::string& romPath);
/** "<label> (<title>, <map>, <ROM size>, <RAM size>)", or "". */
std::string cartridgeInfo(MachineDescriptor* m);

/** Pad buttons, as the 16-bit auto-read word ($4218/$4219) has them. */
enum Button : uint16_t {
    BTN_R = 0x0010, BTN_L = 0x0020, BTN_X = 0x0040, BTN_A = 0x0080,
    BTN_RIGHT = 0x0100, BTN_LEFT = 0x0200, BTN_DOWN = 0x0400, BTN_UP = 0x0800,
    BTN_START = 0x1000, BTN_SELECT = 0x2000, BTN_Y = 0x4000, BTN_B = 0x8000,
};
void setButtons(MachineDescriptor* m, int port, uint16_t buttons);

/** The master clock (for tests). */
uint64_t masterClock(MachineDescriptor* m);
/** Current scanline (0-261) and H clock (0-1363). */
int scanline(MachineDescriptor* m);
int hclock(MachineDescriptor* m);

}  // namespace snes
