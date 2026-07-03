#pragma once

#include <cstdint>
#include <string>
#include <vector>

enum class BreakpointType {
    EXEC,
    READ_WATCH,
    WRITE_WATCH
};

struct Breakpoint {
    uint32_t       addr;
    BreakpointType type;
    std::string    condition;
    int            hitCount;
    bool           enabled;
    bool           physical;   // match against 28-bit physical address (#73)
    int            id;
};

class DebugContext;

class BreakpointList {
public:
    int  add(uint32_t addr, BreakpointType type, bool physical = false);
    void remove(int id);
    void setEnabled(int id, bool enabled);
    void setCondition(int id, const std::string& condition);
    void clearHitCounts();

    Breakpoint* checkExec(uint32_t addr, DebugContext* dbg);
    Breakpoint* checkWrite(uint32_t addr, DebugContext* dbg);
    Breakpoint* checkRead(uint32_t addr, DebugContext* dbg);

    const std::vector<Breakpoint>& breakpoints() const { return m_breakpoints; }
    bool hasExecBreakpoints() const { return m_execCount > 0; }

private:
    std::vector<Breakpoint> m_breakpoints;
    int m_nextId = 1;
    int m_execCount = 0;
};
