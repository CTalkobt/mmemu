#pragma once

#include <string>

struct MachineDescriptor;

/**
 * CPU speed control shared by the CLI `speed` command, the MCP `set_speed`
 * tool and the GUI Control > Speed menu. "max" means warp (unbounded: devices
 * advance one cycle per CPU cycle); a number selects that CPU clock in MHz.
 */
namespace machine_speed {

/** True if the machine has a CPU speed model (getWarp/setWarp/cpuSpeedMHz). */
bool supported(const MachineDescriptor& m);

/**
 * Apply "max" or a clock in MHz ("1", "3.5", "40", "2MHz"; "40" means 40.5).
 * Returns false and sets `error` (listing the choices) if the value is invalid.
 * `selectedMHz` (optional) receives the clock selected, or 0 for max.
 */
bool apply(MachineDescriptor& m, const std::string& value, std::string& error,
           double* selectedMHz = nullptr);

/** "max" or the selectable clocks, e.g. "1|2|3.5|40.5|max". */
std::string choices(const MachineDescriptor& m);

/** One-line description: "Speed: max (warp: ...)" or "Speed: 3.5 MHz". */
std::string describe(const MachineDescriptor& m);

/**
 * Extra line when `requestedMHz` differs from what the CPU runs at now (e.g.
 * in the MEGA65 hypervisor or after POKE 0,65); empty otherwise.
 */
std::string pendingNote(const MachineDescriptor& m, double requestedMHz);

/** Format a clock: 1, 3.5, 40.5. */
std::string formatMHz(double mhz);

}  // namespace machine_speed
