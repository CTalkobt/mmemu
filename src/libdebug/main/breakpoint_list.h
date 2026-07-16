#pragma once

#include <cstdint>
#include <string>
#include <vector>

enum class BreakpointType {
    EXEC,
    READ_WATCH,
    WRITE_WATCH,
    VALUE_WATCH    // Watch for value changes at address
};

struct Breakpoint {
    uint32_t       addr;
    BreakpointType type;
    std::string    condition;      // Condition expression (e.g., "x == 0x01")
    std::string    luaAction;      // Lua script to execute on breakpoint hit (Issue #24)
    int            hitCount;       // Current hit count
    int            hitCountLimit;  // Stop when hitCount reaches this (0 = unlimited)
    bool           enabled;
    bool           physical;       // match against 28-bit physical address (#73)
    int            id;

    // Memory watch specific
    uint32_t       watchSize;      // For VALUE_WATCH: size in bytes to watch
    std::vector<uint8_t> lastWatchValue;  // Previous value for change detection
};

enum class BreakpointAction {
    NONE,
    BREAK,
    LOG,
    CONTINUE
};

class DebugContext;

class BreakpointList {
public:
    int  add(uint32_t addr, BreakpointType type, bool physical = false);
    int  addWatch(uint32_t addr, uint32_t size);  // Add memory watch
    void remove(int id);
    void removeByAddress(uint32_t addr);  // Clear breakpoint at address
    void setEnabled(int id, bool enabled);
    void setCondition(int id, const std::string& condition);
    void setLuaAction(int id, const std::string& luaCode);  // Issue #24: Lua breakpoint actions
    void setHitCountLimit(int id, int limit);
    void clearHitCounts();

    Breakpoint* checkExec(uint32_t addr, DebugContext* dbg);
    Breakpoint* checkWrite(uint32_t addr, DebugContext* dbg);
    Breakpoint* checkRead(uint32_t addr, DebugContext* dbg);
    Breakpoint* checkValueChange(uint32_t addr, uint32_t size, DebugContext* dbg);

    Breakpoint* findById(int id);
    const std::vector<Breakpoint>& breakpoints() const { return m_breakpoints; }
    bool hasExecBreakpoints() const { return m_execCount > 0; }
    bool hasValueWatches() const { return m_watchCount > 0; }

private:
    std::vector<Breakpoint> m_breakpoints;
    int m_nextId = 1;
    int m_execCount = 0;
    int m_watchCount = 0;
};
