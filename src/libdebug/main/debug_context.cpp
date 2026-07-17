#include "debug_context.h"
#include "o45_object_loader.h"
#include "o45_symbol_parser.h"
#include "lua_event_registry.h"
#include "include/mmemu_plugin_api.h"
#include "libcore/main/image_loader.h"
#include "include/util/logging.h"
#include <iostream>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <string>

// Issue #24: Lua breakpoint action support
#ifdef __has_include
#if __has_include("lua_engine.h")
#include "lua_engine.h"
#define HAVE_LUA_ENGINE 1
#endif
#endif

static std::string toHex(uint32_t v) {
    std::ostringstream ss;
    ss << std::uppercase << std::hex << v;
    return ss.str();
}

#include "observer_registry.h"

DebugContext::DebugContext(ICore* cpu, IBus* bus)
    : m_cpu(cpu), m_bus(bus), m_luaEventRegistry(std::make_unique<LuaEventRegistry>()) {
}

bool DebugContext::needsDisasm() const {
    return true;
}

void DebugContext::onStepLite(ICore* cpu, uint32_t pc) {
    TraceEntry te;
    te.addr = pc;
    te.cycles = cpu->cycles();
    m_trace.push(te);
}

bool DebugContext::onStep(ICore* cpu, IBus* bus, const DisasmEntry& entry) {
    bool cont = true;
    for (auto* obs : ObserverRegistry::instance().observers()) {
        if (!obs->onStep(cpu, bus, entry)) cont = false;
    }

    TraceEntry te;
    te.addr = entry.addr;
    te.mnemonic = entry.complete;
    te.cycles = cpu->cycles();

    for (int i = 0; i < cpu->regCount(); ++i) {
        const auto* desc = cpu->regDescriptor(i);
        if (!(desc->flags & REGFLAG_INTERNAL)) {
            te.regs[desc->name] = cpu->regRead(i);
        }
    }

    m_trace.push(te);

    // Track cycle count for Lua event system
    m_cycleCounter += cpu->cycles();

    // On resume, skip the breakpoint at the address we just paused on (one step).
    if (entry.addr == m_resumeSkipAddr) {
        m_resumeSkipAddr = ~0u;
        trackStack(cpu, entry);
        return true;
    }

    if (auto* bp = m_breakpoints.checkExec(entry.addr, this)) {
        m_lastHitMessage = "Execution breakpoint " + std::to_string(bp->id) + " hit at $" + toHex(entry.addr);
        m_cpu->log(SIM_LOG_INFO, m_lastHitMessage.c_str());
        m_lastPausedAddr = entry.addr;
        m_paused = true;
        cont = false;

        // Issue #24: Execute Lua breakpoint action if present
        executeLuaBreakpointAction(*bp);
    }

    // Issue #24 Phase 4.2: Execute Lua cycle events
    executeLuaCycleEvents(m_cycleCounter);

    if (!cont) return false;

    trackStack(cpu, entry);
    monitorKernal(cpu, entry);
    monitorBasic(cpu, entry);
    return true;
}

static uint32_t regByName(ICore* cpu, const char* name) {
    for (int i = 0; i < cpu->regCount(); ++i) {
        if (strcmp(cpu->regDescriptor(i)->name, name) == 0)
            return cpu->regRead(i);
    }
    return 0;
}

void DebugContext::monitorKernal(ICore* cpu, const DisasmEntry& entry) {
    if (!cpu) return;
    auto logger = LogRegistry::instance().getLogger("kernal");
    if (!logger || logger->level() > spdlog::level::debug) return;

    // 1. Check for Exit (RTS)
    // 6502 RTS is opcode $60. It pulls from stack when SP is at the value it was at entry.
    if (entry.isReturn && !m_kernalStack.empty()) {
        if (cpu->sp() == m_kernalStack.back().entrySp) {
            std::string name = m_kernalStack.back().name;
            m_kernalStack.pop_back();
            logger->debug("EXIT  {}: {}", name, formatState(cpu));
        }
    }

    // 2. Check for Entry
    // We consider it a Kernal entry if the address has a symbol AND is in the Kernal ROM area (>$E000)
    if (entry.addr >= 0xE000) {
        std::string label = m_symbols.getLabel(entry.addr);
        if (!label.empty()) {
            // Log entry
            logger->debug("ENTRY {}: {}", label, formatState(cpu));
            // Push to stack to monitor exit
            m_kernalStack.push_back({label, cpu->sp()});
        }
    }
}

void DebugContext::monitorBasic(ICore* cpu, const DisasmEntry& entry) {
    if (!cpu) return;
    auto logger = LogRegistry::instance().getLogger("basic");
    if (!logger || logger->level() > spdlog::level::debug) return;

    // 1. Check for Exit (RTS)
    if (entry.isReturn && !m_basicStack.empty()) {
        if (cpu->sp() == m_basicStack.back().entrySp) {
            std::string name = m_basicStack.back().name;
            m_basicStack.pop_back();
            logger->debug("EXIT  {}: {}", name, formatState(cpu));
        }
    }

    // 2. Check for Entry
    // BASIC area is generally between $A000 and $DFFF across C64, VIC-20, and PET.
    if (entry.addr >= 0xA000 && entry.addr <= 0xDFFF) {
        std::string label = m_symbols.getLabel(entry.addr);
        if (!label.empty()) {
            // Log entry
            logger->debug("ENTRY {}: {}", label, formatState(cpu));
            // Push to stack to monitor exit
            m_basicStack.push_back({label, cpu->sp()});
        }
    }
}

std::string DebugContext::formatState(ICore* cpu) {
    std::stringstream ss;
    for (int i = 0; i < cpu->regCount(); ++i) {
        const auto* desc = cpu->regDescriptor(i);
        if (desc->flags & REGFLAG_INTERNAL) continue;
        uint32_t val = cpu->regRead(i);
        ss << desc->name << "=$" << std::hex << std::uppercase << std::setfill('0')
           << std::setw(desc->width == RegWidth::R16 ? 4 : 2) << val << " ";
    }
    return ss.str();
}

void DebugContext::trackStack(ICore* cpu, const DisasmEntry& entry) {
    if (entry.isCall) {
        m_stackTrace.push(StackPushType::CALL, entry.addr, entry.targetAddr);
        return;
    }
    if (entry.isReturn) {
        m_stackTrace.pop();
        return;
    }
    const std::string& mn = entry.mnemonic;
    if      (mn == "PHA") m_stackTrace.push(StackPushType::PHA, entry.addr, regByName(cpu, "A"));
    else if (mn == "PHX") m_stackTrace.push(StackPushType::PHX, entry.addr, regByName(cpu, "X"));
    else if (mn == "PHY") m_stackTrace.push(StackPushType::PHY, entry.addr, regByName(cpu, "Y"));
    else if (mn == "PHP") m_stackTrace.push(StackPushType::PHP, entry.addr, regByName(cpu, "P"));
    else if (mn == "PHZ") m_stackTrace.push(StackPushType::PHZ, entry.addr, regByName(cpu, "Z"));
    else if (mn == "BRK") m_stackTrace.push(StackPushType::BRK, entry.addr, 0);
    else if (mn == "PLA" || mn == "PLX" || mn == "PLY" || mn == "PLP" || mn == "PLZ")
        m_stackTrace.pop();
}

void DebugContext::onMemoryWrite(IBus* bus, uint32_t addr, uint8_t before, uint8_t after) {
    for (auto* obs : ObserverRegistry::instance().observers()) {
        obs->onMemoryWrite(bus, addr, before, after);
    }

    // Record memory write for undo support (time-travel debugging)
    if (TraceEntry* cur = m_trace.current()) {
        cur->memWrites.push_back({addr, before});
    }

    if (m_heatmap.isEnabled()) m_heatmap.recordWrite(addr);

    if (auto* bp = m_breakpoints.checkWrite(addr, this)) {
        m_lastHitMessage = "Write watchpoint " + std::to_string(bp->id) + " hit at $" + toHex(addr);
        m_cpu->log(SIM_LOG_INFO, m_lastHitMessage.c_str());
        m_paused = true;
    }
}

void DebugContext::onMemoryRead(IBus* bus, uint32_t addr, uint8_t val) {
    for (auto* obs : ObserverRegistry::instance().observers()) {
        obs->onMemoryRead(bus, addr, val);
    }

    if (m_heatmap.isEnabled()) m_heatmap.recordRead(addr);

    if (auto* bp = m_breakpoints.checkRead(addr, this)) {
        m_lastHitMessage = "Read watchpoint " + std::to_string(bp->id) + " hit at $" + toHex(addr);
        m_cpu->log(SIM_LOG_INFO, m_lastHitMessage.c_str());
        m_paused = true;
    }
}

void DebugContext::onMachineLoad(MachineDescriptor* desc) {
    for (auto* obs : ObserverRegistry::instance().observers()) {
        obs->onMachineLoad(desc);
    }
}

int DebugContext::saveSnapshot(const std::string& label) {
    SystemSnapshot snap;
    snap.label = label;
    
    snap.cpuState.resize(m_cpu->stateSize());
    m_cpu->saveState(snap.cpuState.data());
    
    snap.busState.resize(m_bus->stateSize());
    m_bus->saveState(snap.busState.data());

    auto* cart = ImageLoaderRegistry::instance().getActiveCartridge(m_bus);
    if (cart) {
        snap.cartridgePath = cart->metadata().imagePath;
    }
    
    m_snapshots.push_back(std::move(snap));
    return (int)m_snapshots.size() - 1;
}

bool DebugContext::restoreSnapshot(int index) {
    if (index < 0 || index >= (int)m_snapshots.size()) return false;
    
    const auto& snap = m_snapshots[index];
    m_cpu->loadState(snap.cpuState.data());
    m_bus->loadState(snap.busState.data());

    // Restore cartridge if path is set and different from current
    auto* currentCart = ImageLoaderRegistry::instance().getActiveCartridge(m_bus);
    std::string currentPath = currentCart ? currentCart->metadata().imagePath : "";
    
    if (snap.cartridgePath != currentPath) {
        if (currentCart) {
            currentCart->eject(m_bus);
            ImageLoaderRegistry::instance().setActiveCartridge(m_bus, nullptr);
        }
        if (!snap.cartridgePath.empty()) {
            auto newCart = ImageLoaderRegistry::instance().createCartridgeHandler(snap.cartridgePath);
            if (newCart) {
                if (newCart->attach(m_bus, nullptr)) {
                    ImageLoaderRegistry::instance().setActiveCartridge(m_bus, std::move(newCart));
                }
            }
        }
    }
    
    return true;
}

bool DebugContext::deleteSnapshot(int index) {
    if (index < 0 || index >= (int)m_snapshots.size()) return false;
    m_snapshots.erase(m_snapshots.begin() + index);
    return true;
}

std::vector<uint32_t> DebugContext::diffSnapshots(int idxA, int idxB) {
    std::vector<uint32_t> diffs;
    if (idxA < 0 || idxA >= (int)m_snapshots.size()) return diffs;
    if (idxB < 0 || idxB >= (int)m_snapshots.size()) return diffs;

    const auto& snapA = m_snapshots[idxA];
    const auto& snapB = m_snapshots[idxB];

    if (snapA.busState.size() != snapB.busState.size()) return diffs;

    for (uint32_t i = 0; i < snapA.busState.size(); ++i) {
        if (snapA.busState[i] != snapB.busState[i]) {
            diffs.push_back(i);
        }
    }

    return diffs;
}

bool DebugContext::loadDebugSymbolsFromO45(const std::string& path) {
    // Load debug symbols from .o45 object file
    std::vector<uint8_t> debugData;
    if (!O45ObjectLoader::loadDebugSymbols(path, debugData)) {
        return false;
    }

    // Parse debug symbols and populate VariableSymbolTable
    if (!O45SymbolParser::populateTable(debugData, m_variables)) {
        return false;
    }

    return true;
}

// Issue #24: Execute Lua breakpoint action scripts
void DebugContext::executeLuaBreakpointAction(const Breakpoint& bp) {
#ifdef HAVE_LUA_ENGINE
    if (bp.luaAction.empty()) {
        return;  // No action to execute
    }

    if (!m_cpu || !m_bus) {
        return;  // Cannot execute without CPU and bus context
    }

    try {
        // Create a Lua engine with current machine context
        LuaEngine engine(m_cpu, m_bus, this);

        // Execute the Lua action
        if (!engine.executeString(bp.luaAction)) {
            // Log error but don't crash debugger
            auto logger = LogRegistry::instance().getLogger("lua");
            if (logger) {
                logger->warn("[Breakpoint {}] Lua action failed: {}", bp.id, engine.getLastError());
            }
        } else {
            // Log successful execution
            auto logger = LogRegistry::instance().getLogger("lua");
            if (logger) {
                logger->debug("[Breakpoint {}] Lua action executed", bp.id);
            }
        }
    } catch (const std::exception& e) {
        // Catch any C++ exceptions and log them
        auto logger = LogRegistry::instance().getLogger("lua");
        if (logger) {
            logger->error("[Breakpoint {}] Lua execution exception: {}", bp.id, e.what());
        }
    }
#else
    // Lua engine not available - silently skip (Issue #24 Phase 4 deferred)
    // Users without lua5.4-dev will see the breakpoint action stored but not executed
#endif
}

// Issue #24 Phase 4.2: Execute Lua cycle event handlers
void DebugContext::executeLuaCycleEvents(uint64_t currentCycle) {
#ifdef HAVE_LUA_ENGINE
    if (!m_cpu || !m_bus || !m_luaEventRegistry) {
        return;  // Cannot execute without context
    }

    // Get all handlers that should fire
    auto handlers = m_luaEventRegistry->getReadyCycleHandlers(currentCycle);
    if (handlers.empty()) {
        return;  // No handlers to execute
    }

    try {
        // Create a Lua engine with current machine context
        LuaEngine engine(m_cpu, m_bus, this);

        for (const auto& functionName : handlers) {
            // Call the registered Lua function
            if (!engine.callFunction(functionName)) {
                auto logger = LogRegistry::instance().getLogger("lua");
                if (logger) {
                    logger->warn("[Cycle Event] Handler '{}' failed: {}", functionName, engine.getLastError());
                }
            } else {
                auto logger = LogRegistry::instance().getLogger("lua");
                if (logger) {
                    logger->debug("[Cycle Event] Handler '{}' executed at cycle {}", functionName, currentCycle);
                }
            }
        }
    } catch (const std::exception& e) {
        auto logger = LogRegistry::instance().getLogger("lua");
        if (logger) {
            logger->error("[Cycle Event] Lua execution exception: {}", e.what());
        }
    }
#endif
}
