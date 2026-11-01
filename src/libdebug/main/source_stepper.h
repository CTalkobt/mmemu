#pragma once

#include "libtoolchain/main/source_map.h"
#include <cstdint>

class DebugContext;
class ICore;
class IBus;
struct MachineDescriptor;

/**
 * Source-level stepping shared by the CLI (step / next / finish) and the GUI
 * (Step Into / Over / Out). Where the current PC has no source mapping, Into
 * and Over fall back to one instruction (Over runs a called subroutine to its
 * return).
 */
namespace source_stepper {

enum class Mode {
    Into,   // to the next source line, following calls
    Over,   // to the next source line at the same call depth or shallower
    Out,    // until the current subroutine returns
};

enum class Stop {
    NewLine,          // reached a different mapped source line
    NextInstruction,  // no mapping at the start: stepped one instruction (or over a call)
    Returned,         // Out: the subroutine returned
    NotInRoutine,     // Out: call depth was already 0
    Breakpoint,       // a breakpoint / watchpoint paused execution
    ProgramEnd,       // the CPU reports program end
    StepLimit,        // maxSteps reached
    Interrupted,      // *interrupted became non-zero
};

struct Result {
    Stop           stop = Stop::NewLine;
    uint64_t       steps = 0;
    bool           sourceLevel = false;   // the start PC had a source mapping
    SourceLocation location{"", -1};       // mapping of the PC where it stopped (may be empty)
};

/**
 * Step the machine (schedulerStep if the machine has one, else cpu->step()).
 * `interrupted` (optional) is polled each step.
 */
Result step(DebugContext& dbg, ICore* cpu, IBus* bus, MachineDescriptor* machine, Mode mode,
            uint64_t maxSteps = 10000000, const volatile int* interrupted = nullptr);

enum class LineBreakpoint { Added, Removed, NoCode };

/**
 * Toggle an execution breakpoint at the first address mapped to `file`:`line`
 * (file as stored in the source map). `addr` (optional) receives the address.
 */
LineBreakpoint toggleLineBreakpoint(DebugContext& dbg, const std::string& file, int line,
                                    uint32_t* addr = nullptr);

/** Short human-readable description of a result, e.g. "Reached main.s:12". */
const char* stopName(Stop stop);

}  // namespace source_stepper
