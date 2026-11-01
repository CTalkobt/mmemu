#include "io_registry.h"
#include <algorithm>

void IORegistry::registerHandler(IOHandler* handler) {
    m_handlers.push_back(handler);
    // Sort by base address to establish a predictable priority
    std::sort(m_handlers.begin(), m_handlers.end(), [](IOHandler* a, IOHandler* b) {
        return a->baseAddr() < b->baseAddr();
    });
    rebuildDeviceClockFlags();
}

void IORegistry::setDeviceClocked(IOHandler* handler, bool deviceClocked) {
    auto it = std::find(m_deviceClocked.begin(), m_deviceClocked.end(), handler);
    if (deviceClocked && it == m_deviceClocked.end()) m_deviceClocked.push_back(handler);
    if (!deviceClocked && it != m_deviceClocked.end()) m_deviceClocked.erase(it);
    rebuildDeviceClockFlags();
}

void IORegistry::rebuildDeviceClockFlags() {
    m_isDeviceClocked.assign(m_handlers.size(), 0);
    for (size_t i = 0; i < m_handlers.size(); ++i)
        if (std::find(m_deviceClocked.begin(), m_deviceClocked.end(), m_handlers[i]) != m_deviceClocked.end())
            m_isDeviceClocked[i] = 1;
}

void IORegistry::registerOwnedHandler(IOHandler* handler) {
    m_ownedHandlers.push_back(handler);
    registerHandler(handler);
}

IORegistry::~IORegistry() {
    for (auto* h : m_ownedHandlers) {
        delete h;
    }
}

bool IORegistry::dispatchRead(IBus* bus, uint32_t addr, uint8_t* val) {
    for (auto* handler : m_handlers) {
        if (handler->ioRead(bus, addr, val)) {
            return true;
        }
    }
    return false;
}

bool IORegistry::dispatchWrite(IBus* bus, uint32_t addr, uint8_t val) {
    for (auto* handler : m_handlers) {
        if (handler->ioWrite(bus, addr, val)) {
            return true;
        }
    }
    return false;
}

void IORegistry::tickAll(uint64_t cycles) {
    for (auto* handler : m_handlers) {
        handler->tick(cycles);
    }
}

void IORegistry::tickAll(uint64_t cpuCycles, uint64_t deviceCycles) {
    for (size_t i = 0; i < m_handlers.size(); ++i) {
        if (m_isDeviceClocked[i]) {
            if (deviceCycles) m_handlers[i]->tick(deviceCycles);
        } else {
            m_handlers[i]->tick(cpuCycles);
        }
    }
}

void IORegistry::resetAll() {
    for (auto* handler : m_handlers) {
        handler->reset();
    }
}

void IORegistry::enumerate(std::vector<IOHandler*>& list) {
    for (auto* handler : m_handlers) {
        list.push_back(handler);
    }
}

IOHandler* IORegistry::findHandler(const std::string& name) {
    for (auto* handler : m_handlers) {
        if (name == handler->name()) {
            return handler;
        }
    }
    return nullptr;
}

void IORegistry::clear() {
    m_handlers.clear();
    m_isDeviceClocked.clear();
}

uint32_t IORegistry::lowestHandlerBase() const {
    for (const auto* handler : m_handlers) {
        uint32_t base = handler->baseAddr();
        if (base > 0) return base;
    }
    return UINT32_MAX;
}
