#include "debug_context.h"
#include "include/mmemu_plugin_api.h"
#include "libcore/main/image_loader.h"
#include "include/util/logging.h"
#include <iostream>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <string>

static std::string toHex(uint32_t v) {
    std::ostringstream ss;
    ss << std::uppercase << std::hex << v;
    return ss.str();
}

#include "observer_registry.h"

DebugContext::DebugContext(ICore* cpu, IBus* bus)
    : m_cpu(cpu), m_bus(bus) {
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
    }

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
