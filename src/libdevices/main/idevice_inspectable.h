#pragma once

#include <cstdint>
#include <string>

/// Interface for devices that support runtime inspection and manipulation
/// without CPU execution (e.g., advancing timers to test interrupt timing)
class IDeviceInspectable {
public:
    virtual ~IDeviceInspectable() = default;

    /// Advance device state by N cycles without triggering interrupts or side effects
    /// Useful for testing interrupt conditions at specific cycle counts
    /// Returns a status message describing what happened
    virtual std::string advanceCycles(uint32_t cycles) = 0;

    /// Get formatted status string showing current device state
    /// Includes timer counts, running state, interrupt conditions, etc.
    virtual std::string getStatusString() const = 0;
};
