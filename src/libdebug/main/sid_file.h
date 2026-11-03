#pragma once

#include <cstdint>
#include <string>
#include <vector>

class IBus;
class ICore;

/**
 * PSID/RSID music files (.sid): header parsing and a play driver, shared by
 * the CLI `sid load` command and the MCP `load_sid` tool.
 *
 * load() copies the tune into memory and installs a small driver, as a SID
 * player does, then points the CPU at it; running the machine plays the tune:
 *
 *   - init is called with the song number in A, with $01 set from init's
 *     address (sidplayfp's banking: below $A000 $37, below $D000 $36,
 *     $E000 and up $35, else $34)
 *   - play is called once per CIA 1 timer A period, polled with interrupts
 *     off: one PAL frame (19656 cycles) for VBI songs, or the tune's own
 *     timer value for CIA-speed songs (60 Hz if init sets none)
 *   - with no play address (the tune installs its own interrupt handler),
 *     interrupts are enabled after init and the driver idles
 *
 * The driver is placed in free RAM outside the tune: the cassette buffer at
 * $0334, else $0200, else the first free 64 bytes from $C000 or $1000.
 */
namespace psid {

struct Info {
    std::string magic;          ///< "PSID" or "RSID"
    int version = 0;
    std::string title, author, released;
    uint16_t loadAddr = 0, initAddr = 0, playAddr = 0;
    uint32_t dataLen = 0;
    int songs = 1, startSong = 1;
    int subtune = 1;            ///< song being played (1-based)
    bool ciaSpeed = false;      ///< the song is timed by CIA 1, not the VBI
    int clock = 0;              ///< header: 0 unknown, 1 PAL, 2 NTSC, 3 both
    int sidModel = 0;           ///< header: 0 unknown, 1 6581, 2 8580, 3 both
    uint16_t driverAddr = 0;    ///< where load() put the play driver
};

/** Parse the header (and the load address). Returns false with `err` set. */
bool parse(const std::vector<uint8_t>& file, Info& info, std::string& err);

/**
 * Load the tune's data, install the driver for `subtune` (1-based; < 1 =
 * the header's start song) and set the CPU's PC to it.
 */
bool load(const std::vector<uint8_t>& file, int subtune, IBus* bus, ICore* cpu,
          Info& info, std::string& err);

/** Several lines: title, author, release, songs, addresses, timing, driver. */
std::string describe(const Info& info);

/** $01 value for calling code at `addr` (sidplayfp's PSID banking). */
uint8_t bankFor(uint16_t addr);

/** CIA 1 timer A latch for one PAL frame (312 lines x 63 cycles, minus 1). */
constexpr uint16_t PAL_FRAME_LATCH = 312 * 63 - 1;
/** Default CIA-speed timer latch: 60 Hz on a PAL machine. */
constexpr uint16_t CIA_DEFAULT_LATCH = 0x4025;

}  // namespace psid
