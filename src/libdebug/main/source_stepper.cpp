#include "source_stepper.h"
#include "debug_context.h"
#include "libcore/main/icore.h"
#include "libcore/main/machine_desc.h"

namespace source_stepper {

const char* stopName(Stop stop) {
    switch (stop) {
        case Stop::NewLine:         return "reached a new source line";
        case Stop::NextInstruction: return "stepped one instruction";
        case Stop::Returned:        return "subroutine returned";
        case Stop::NotInRoutine:    return "not inside a subroutine";
        case Stop::Breakpoint:      return "breakpoint hit";
        case Stop::ProgramEnd:      return "program end reached";
        case Stop::StepLimit:       return "step limit reached (possible infinite loop)";
        case Stop::Interrupted:     return "interrupted";
    }
    return "";
}

LineBreakpoint toggleLineBreakpoint(DebugContext& dbg, const std::string& file, int line, uint32_t* addrOut) {
    uint32_t addr = dbg.sourceMap().sourceToAddr(file, line);
    if (addrOut) *addrOut = addr;
    if (addr == 0xFFFFFFFF) return LineBreakpoint::NoCode;
    auto& bps = dbg.breakpoints();
    for (const auto& bp : bps.breakpoints())
        if (bp.type == BreakpointType::EXEC && bp.addr == addr) {
            bps.remove(bp.id);
            return LineBreakpoint::Removed;
        }
    bps.add(addr, BreakpointType::EXEC);
    return LineBreakpoint::Added;
}

static bool sameLine(const SourceLocation& a, const SourceLocation& b) {
    return a.file == b.file && a.line == b.line;
}

Result step(DebugContext& dbg, ICore* cpu, IBus* bus, MachineDescriptor* machine, Mode mode,
            uint64_t maxSteps, const volatile int* interrupted) {
    Result r;
    const uint32_t startPc = cpu->pc();
    const SourceLocation startLoc = dbg.sourceMap().addrToSource(startPc);
    const int startDepth = dbg.stackTrace().depth();
    const uint64_t startCycles = cpu->cycles();
    r.sourceLevel = !startLoc.file.empty();

    if (mode == Mode::Out && startDepth == 0) {
        r.stop = Stop::NotInRoutine;
        r.location = startLoc;
        return r;
    }

    dbg.resume();
    auto stepOnce = [&]() {
        if (machine && machine->schedulerStep) machine->schedulerStep(*machine);
        else cpu->step();
    };

    while (true) {
        stepOnce();
        ++r.steps;
        if (dbg.isPaused())                    { r.stop = Stop::Breakpoint; break; }
        if (cpu->isProgramEnd(bus))            { r.stop = Stop::ProgramEnd; break; }

        // A scheduler step may only stall the CPU (DMA, badlines): wait for an instruction
        const bool ranInstruction = cpu->cycles() != startCycles;
        const int depth = dbg.stackTrace().depth();
        const SourceLocation loc = dbg.sourceMap().addrToSource(cpu->pc());

        bool done = false;
        if (mode == Mode::Out) {
            done = depth < startDepth;
            if (done) r.stop = Stop::Returned;
        } else if (r.sourceLevel) {
            bool depthOk = (mode == Mode::Into) || depth <= startDepth;
            done = depthOk && !loc.file.empty() && !sameLine(loc, startLoc);
            if (done) r.stop = Stop::NewLine;
        } else {
            bool depthOk = (mode == Mode::Into) || depth <= startDepth;
            done = ranInstruction && depthOk;
            if (done) r.stop = Stop::NextInstruction;
        }
        if (done) break;
        if (interrupted && *interrupted)       { r.stop = Stop::Interrupted; break; }
        if (r.steps >= maxSteps)               { r.stop = Stop::StepLimit; break; }
    }
    r.location = dbg.sourceMap().addrToSource(cpu->pc());
    return r;
}

}  // namespace source_stepper
