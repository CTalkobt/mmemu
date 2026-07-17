#pragma once

#include "execution_observer.h"
#include "breakpoint_list.h"
#include "trace_buffer.h"
#include "stack_trace.h"
#include "memory_heatmap.h"
#include "lua_event_registry.h"
#include "libtoolchain/main/symbol_table.h"
#include "libtoolchain/main/variable_symbol.h"
#include "libtoolchain/main/source_map.h"
#include <vector>
#include <memory>

// Forward declaration for Lua integration (Issue #24)
class LuaEngine;

struct SystemSnapshot {
    std::string label;
    std::vector<uint8_t> cpuState;
    std::vector<uint8_t> busState;
    std::string cartridgePath;
};

class DebugContext : public ExecutionObserver {
public:
    DebugContext(ICore* cpu, IBus* bus);

    bool needsDisasm() const override;
    bool onStep(ICore* cpu, IBus* bus, const DisasmEntry& entry) override;
    void onStepLite(ICore* cpu, uint32_t pc) override;
    void onMemoryWrite(IBus* bus, uint32_t addr, uint8_t before, uint8_t after) override;
    void onMemoryRead(IBus* bus, uint32_t addr, uint8_t val) override;
    void onMachineLoad(MachineDescriptor* desc) override;

    BreakpointList& breakpoints() { return m_breakpoints; }
    TraceBuffer&    trace()       { return m_trace; }
    StackTrace&     stackTrace()  { return m_stackTrace; }
    SymbolTable&    symbols()     { return m_symbols; }
    VariableSymbolTable& variables() { return m_variables; }
    SourceMap&      sourceMap()   { return m_sourceMap; }
    MemoryHeatMap&  heatmap()     { return m_heatmap; }
    LuaEventRegistry& luaEvents() { return *m_luaEventRegistry; }

    /**
     * Load debug symbols from an .o45 object file.
     * Extracts OPT_DEBUG_SYMBOLS metadata and populates the VariableSymbolTable.
     *
     * @param path File path to .o45 object file
     * @return true if debug symbols were successfully loaded
     */
    bool loadDebugSymbolsFromO45(const std::string& path);

    int  saveSnapshot(const std::string& label);
    bool restoreSnapshot(int index);
    bool deleteSnapshot(int index);
    void clearSnapshots() { m_snapshots.clear(); }

    // Returns indices of snapshots
    const std::vector<SystemSnapshot>& snapshots() const { return m_snapshots; }

    /**
     * Compare two snapshots and return a list of memory addresses that differ.
     */
    std::vector<uint32_t> diffSnapshots(int idxA, int idxB);

    /** Reverse one step: undo the last traced instruction (restore registers + memory). */
    bool reverseStep() { return m_trace.reverseStep(m_cpu, m_bus); }

    bool isPaused() const { return m_paused; }
    void resume() { m_resumeSkipAddr = m_lastPausedAddr; m_paused = false; }
    const std::string& lastHitMessage() const { return m_lastHitMessage; }

    ICore* cpu() const { return m_cpu; }
    IBus*  bus() const { return m_bus; }

    void setIoRegistry(void* io) { m_ioRegistry = io; }
    void* ioRegistry() const { return m_ioRegistry; }

private:
    void* m_ioRegistry = nullptr;
    struct KernalCall {
        std::string name;
        uint32_t    entrySp;
    };

    void trackStack(ICore* cpu, const DisasmEntry& entry);
    void monitorKernal(ICore* cpu, const DisasmEntry& entry);
    void monitorBasic(ICore* cpu, const DisasmEntry& entry);
    std::string formatState(ICore* cpu);
    void executeLuaBreakpointAction(const Breakpoint& bp);  // Issue #24: Execute Lua on breakpoint
    void executeLuaCycleEvents(uint64_t currentCycle);      // Issue #24 Phase 4.2: Cycle events

    ICore* m_cpu;
    IBus*  m_bus;

    BreakpointList m_breakpoints;
    TraceBuffer    m_trace;
    StackTrace     m_stackTrace;
    SymbolTable    m_symbols;
    VariableSymbolTable m_variables;
    SourceMap      m_sourceMap;
    MemoryHeatMap  m_heatmap;
    std::unique_ptr<LuaEventRegistry> m_luaEventRegistry;
    std::vector<KernalCall> m_kernalStack;
    std::vector<KernalCall> m_basicStack;
    std::vector<SystemSnapshot> m_snapshots;
    bool        m_paused          = false;
    uint32_t    m_lastPausedAddr  = ~0u;
    uint32_t    m_resumeSkipAddr  = ~0u;
    std::string m_lastHitMessage;
    uint64_t    m_cycleCounter    = 0;
};
