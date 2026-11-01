#pragma once

#include <vector>
#include <cstdint>
#include "io_handler.h"

class IBus;

/**
 * Registry and dispatcher for multiple IOHandler devices.
 */
class IORegistry {
public:
    IORegistry() = default;
    ~IORegistry();

    /**
     * Register an I/O handler. The registry does NOT own the pointer.
     */
    void registerHandler(IOHandler* handler);

    /**
     * Register an I/O handler and take ownership. The handler will be
     * deleted when the registry is destroyed.
     */
    void registerOwnedHandler(IOHandler* handler);

    /**
     * Dispatch a read to the first handler that matches the address.
     */
    bool dispatchRead(IBus* bus, uint32_t addr, uint8_t* val);

    /**
     * Dispatch a write to the first handler that matches the address.
     */
    bool dispatchWrite(IBus* bus, uint32_t addr, uint8_t val);

    /**
     * Call tick() on all registered handlers.
     */
    void tickAll(uint64_t cycles);

    /**
     * Mark a handler as running on the machine's device clock (e.g. the 1 MHz
     * phi clock of the VIC/CIA/SID) rather than the CPU clock. Only matters
     * for the two-argument tickAll().
     */
    void setDeviceClocked(IOHandler* handler, bool deviceClocked = true);

    /**
     * Tick CPU-clocked handlers by cpuCycles and device-clocked handlers by
     * deviceCycles (skipped when 0). Used when the CPU runs faster than the
     * device clock, e.g. the MEGA65 at 3.5 or 40.5 MHz.
     */
    void tickAll(uint64_t cpuCycles, uint64_t deviceCycles);

    /**
     * Call reset() on all registered handlers.
     */
    void resetAll();

    /**
     * Enumerate all registered handlers.
     */
    void enumerate(std::vector<IOHandler*>& list);

    /**
     * Find a handler by name.
     */
    IOHandler* findHandler(const std::string& name);

    /**
     * Remove all handlers from the registry.
     */
    void clear();

    /**
     * Return the lowest address that any registered handler claims.
     * Returns UINT32_MAX if no handlers are registered or all have base address 0.
     */
    uint32_t lowestHandlerBase() const;

private:
    void rebuildDeviceClockFlags();

    std::vector<IOHandler*> m_handlers;
    std::vector<IOHandler*> m_deviceClocked;   // handlers on the device clock
    std::vector<uint8_t>    m_isDeviceClocked; // parallel to m_handlers
    std::vector<IOHandler*> m_ownedHandlers;
};
