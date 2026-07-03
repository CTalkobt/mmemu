#include "breakpoint_list.h"
#include "debug_context.h"
#include "expression_evaluator.h"
#include "libcore/main/icore.h"
#include "imap_controller.h"
#include <algorithm>

int BreakpointList::add(uint32_t addr, BreakpointType type, bool physical) {
    int id = m_nextId++;
    m_breakpoints.push_back({addr, type, "", 0, true, physical, id});
    if (type == BreakpointType::EXEC) m_execCount++;
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
