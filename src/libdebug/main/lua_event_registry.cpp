#include "lua_event_registry.h"
#include <algorithm>

LuaEventRegistry::LuaEventRegistry() {
}

LuaEventRegistry::~LuaEventRegistry() {
}

void LuaEventRegistry::registerCycleEvent(uint64_t interval, const std::string& functionName) {
    if (interval == 0) return;  // Invalid interval

    CycleEvent event;
    event.interval = interval;
    event.nextFire = interval;  // Fire after first interval
    event.functionName = functionName;

    m_cycleEvents.push_back(event);
}

void LuaEventRegistry::registerInterruptEvent(const std::string& type, const std::string& functionName) {
    // Validate interrupt type
    if (type != "IRQ" && type != "NMI" && type != "BRK") {
        return;  // Invalid interrupt type
    }

    m_interruptHandlers[type] = functionName;
}

void LuaEventRegistry::unregisterCycleEvent(size_t index) {
    if (index < m_cycleEvents.size()) {
        m_cycleEvents.erase(m_cycleEvents.begin() + index);
    }
}

void LuaEventRegistry::unregisterInterruptEvent(const std::string& type) {
    m_interruptHandlers.erase(type);
}

std::vector<std::string> LuaEventRegistry::getReadyCycleHandlers(uint64_t currentCycle) {
    std::vector<std::string> readyHandlers;

    for (auto& event : m_cycleEvents) {
        if (currentCycle >= event.nextFire) {
            readyHandlers.push_back(event.functionName);
            // Schedule next firing
            event.nextFire = currentCycle + event.interval;
        }
    }

    return readyHandlers;
}

const std::string* LuaEventRegistry::getInterruptHandler(const std::string& type) const {
    auto it = m_interruptHandlers.find(type);
    if (it != m_interruptHandlers.end()) {
        return &it->second;
    }
    return nullptr;
}

void LuaEventRegistry::clear() {
    m_cycleEvents.clear();
    m_interruptHandlers.clear();
}
