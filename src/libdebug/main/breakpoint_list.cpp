#include "breakpoint_list.h"
#include "debug_context.h"
#include "expression_evaluator.h"
#include "libcore/main/icore.h"
#include "imap_controller.h"
#include <algorithm>

int BreakpointList::add(uint32_t addr, BreakpointType type, bool physical) {
    int id = m_nextId++;
    Breakpoint bp;
    bp.addr = addr;
    bp.type = type;
    bp.condition = "";
    bp.hitCount = 0;
    bp.hitCountLimit = 0;
    bp.enabled = true;
    bp.physical = physical;
    bp.id = id;
    bp.watchSize = 0;
    m_breakpoints.push_back(bp);
    if (type == BreakpointType::EXEC) m_execCount++;
    return id;
}

int BreakpointList::addWatch(uint32_t addr, uint32_t size) {
    int id = m_nextId++;
    Breakpoint bp;
    bp.addr = addr;
    bp.type = BreakpointType::VALUE_WATCH;
    bp.condition = "";
    bp.hitCount = 0;
    bp.hitCountLimit = 0;
    bp.enabled = true;
    bp.physical = false;
    bp.id = id;
    bp.watchSize = size;
    m_breakpoints.push_back(bp);
    m_watchCount++;
    return id;
}

// Resolve a virtual address to physical via MapMmu if available.
static uint32_t toPhysical(uint32_t vaddr, DebugContext* dbg) {
    if (dbg && dbg->cpu()) {
        auto* mmu = dbg->cpu()->getMapMmu();
        if (mmu) return mmu->resolvePhysical(vaddr);
    }
    return vaddr;
}

void BreakpointList::remove(int id) {
    auto it = std::find_if(m_breakpoints.begin(), m_breakpoints.end(),
                           [id](const Breakpoint& b) { return b.id == id; });
    if (it != m_breakpoints.end()) {
        if (it->type == BreakpointType::EXEC) m_execCount--;
        if (it->type == BreakpointType::VALUE_WATCH) m_watchCount--;
        m_breakpoints.erase(it);
    }
}

void BreakpointList::removeByAddress(uint32_t addr) {
    auto it = std::find_if(m_breakpoints.begin(), m_breakpoints.end(),
                           [addr](const Breakpoint& b) { return b.addr == addr; });
    if (it != m_breakpoints.end()) {
        if (it->type == BreakpointType::EXEC) m_execCount--;
        if (it->type == BreakpointType::VALUE_WATCH) m_watchCount--;
        m_breakpoints.erase(it);
    }
}

void BreakpointList::setEnabled(int id, bool enabled) {
    for (auto& b : m_breakpoints) {
        if (b.id == id) {
            b.enabled = enabled;
            return;
        }
    }
}

void BreakpointList::setCondition(int id, const std::string& condition) {
    for (auto& b : m_breakpoints) {
        if (b.id == id) {
            b.condition = condition;
            return;
        }
    }
}

void BreakpointList::setHitCountLimit(int id, int limit) {
    for (auto& b : m_breakpoints) {
        if (b.id == id) {
            b.hitCountLimit = limit;
            return;
        }
    }
}

Breakpoint* BreakpointList::findById(int id) {
    for (auto& b : m_breakpoints) {
        if (b.id == id) {
            return &b;
        }
    }
    return nullptr;
}

void BreakpointList::clearHitCounts() {
    for (auto& b : m_breakpoints) {
        b.hitCount = 0;
    }
}

Breakpoint* BreakpointList::checkExec(uint32_t addr, DebugContext* dbg) {
    for (auto& b : m_breakpoints) {
        if (!b.enabled || b.type != BreakpointType::EXEC) continue;
        uint32_t cmpAddr = b.physical ? toPhysical(addr, dbg) : addr;
        if (b.addr == cmpAddr) {
            if (ExpressionEvaluator::evaluateCondition(b.condition, dbg)) {
                b.hitCount++;
                return &b;
            }
        }
    }
    return nullptr;
}

Breakpoint* BreakpointList::checkWrite(uint32_t addr, DebugContext* dbg) {
    for (auto& b : m_breakpoints) {
        if (!b.enabled || b.type != BreakpointType::WRITE_WATCH) continue;
        uint32_t cmpAddr = b.physical ? toPhysical(addr, dbg) : addr;
        if (b.addr == cmpAddr) {
            if (ExpressionEvaluator::evaluateCondition(b.condition, dbg)) {
                b.hitCount++;
                return &b;
            }
        }
    }
    return nullptr;
}

Breakpoint* BreakpointList::checkRead(uint32_t addr, DebugContext* dbg) {
    for (auto& b : m_breakpoints) {
        if (!b.enabled || b.type != BreakpointType::READ_WATCH) continue;
        uint32_t cmpAddr = b.physical ? toPhysical(addr, dbg) : addr;
        if (b.addr == cmpAddr) {
            if (ExpressionEvaluator::evaluateCondition(b.condition, dbg)) {
                b.hitCount++;
                return &b;
            }
        }
    }
    return nullptr;
}

Breakpoint* BreakpointList::checkValueChange(uint32_t addr, uint32_t size, DebugContext* dbg) {
    for (auto& b : m_breakpoints) {
        if (!b.enabled || b.type != BreakpointType::VALUE_WATCH) continue;

        // Check if this watch covers the changed address range
        if (addr >= b.addr && addr < b.addr + b.watchSize) {
            // Read current value
            std::vector<uint8_t> currentValue(b.watchSize);
            if (dbg && dbg->bus()) {
                for (uint32_t i = 0; i < b.watchSize; i++) {
                    currentValue[i] = dbg->bus()->peek8(b.addr + i);
                }
            }

            // Compare with last known value
            if (b.lastWatchValue.empty()) {
                // First time - store value but don't trigger
                b.lastWatchValue = currentValue;
            } else if (b.lastWatchValue != currentValue) {
                // Value changed!
                b.hitCount++;
                b.lastWatchValue = currentValue;
                if (b.hitCountLimit == 0 || b.hitCount < b.hitCountLimit) {
                    return &b;
                }
            }
        }
    }
    return nullptr;
}
