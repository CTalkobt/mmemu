#pragma once

#include <string>
#include <vector>
#include <map>
#include <cstdint>

/// Registry for Lua machine event handlers
/// Manages cycle events, interrupt events, and timer events
///
/// Example usage:
///   registry.registerCycleEvent(10000, "my_handler");  // Every 10k cycles
///   registry.registerInterruptEvent("IRQ", "on_irq");   // On IRQ
class LuaEventRegistry {
public:
    LuaEventRegistry();
    ~LuaEventRegistry();

    /// Register a handler to fire every N CPU cycles
    /// @param interval Number of cycles between handler invocations
    /// @param functionName Name of Lua function to call
    void registerCycleEvent(uint64_t interval, const std::string& functionName);

    /// Register a handler for interrupt events (IRQ, NMI, BRK)
    /// @param type One of "IRQ", "NMI", or "BRK"
    /// @param functionName Name of Lua function to call
    void registerInterruptEvent(const std::string& type, const std::string& functionName);

    /// Unregister a cycle event handler
    void unregisterCycleEvent(size_t index);

    /// Unregister an interrupt event handler
    void unregisterInterruptEvent(const std::string& type);

    /// Check if a cycle event should fire at the given cycle count
    /// Returns the function names of handlers that should fire
    std::vector<std::string> getReadyCycleHandlers(uint64_t currentCycle);

    /// Get the interrupt handler for a given interrupt type
    const std::string* getInterruptHandler(const std::string& type) const;

    /// Clear all registered event handlers
    void clear();

    /// Get number of registered cycle events
    size_t cycleEventCount() const { return m_cycleEvents.size(); }

    /// Get number of registered interrupt events
    size_t interruptEventCount() const { return m_interruptHandlers.size(); }

private:
    struct CycleEvent {
        uint64_t interval;
        uint64_t nextFire;  // Absolute cycle count for next firing
        std::string functionName;
    };

    std::vector<CycleEvent> m_cycleEvents;
    std::map<std::string, std::string> m_interruptHandlers;  // type -> function name
};
