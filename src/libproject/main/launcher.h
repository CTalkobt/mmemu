#pragma once

#include "project.h"
#include <cstdint>
#include <functional>
#include <string>

struct MachineDescriptor;
class DebugContext;

namespace project {

/** How to start a program on a machine. */
struct MachineLaunchInfo {
    uint32_t basicStart = 0;     // BASIC program start; 0 = no BASIC (machine code only)
    int      endPointer = -1;    // zero-page address of BASIC's end-of-program pointer, -1 = leave
    uint64_t bootSteps  = 0;     // scheduler steps from reset to READY (0 = no ROM to boot)
};

/** Launch details by machine id (c64, vic20, pet, mega65; others: machine code only). */
MachineLaunchInfo launchInfoFor(const std::string& machineId);

struct LaunchResult {
    bool        ok = false;
    std::string message;
    uint32_t    loadAddress = 0;
    bool        typedRun = false;   // started with RUN (BASIC); otherwise PC = load address
};

/**
 * Load `file` into the machine and start it:
 * - reset (if `reset`) and boot to READY,
 * - load the file (physical bus),
 * - a program at the machine's BASIC start: set the end-of-program pointer and
 *   type RUN; anything else: set the PC to the load address.
 * The caller then runs the machine.
 */
LaunchResult launch(MachineDescriptor& machine, DebugContext* dbg, const std::string& file, bool reset);

/**
 * Load the project's symbol files and ca45 source maps into the debugger.
 * Returns a summary line (empty if nothing is configured).
 */
std::string loadDebugInfo(DebugContext& dbg, const Config& cfg);

}  // namespace project
