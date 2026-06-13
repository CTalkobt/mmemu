#pragma once

#include <cstdint>

// Forward declarations
struct MapState;
class IBus;

/**
 * Interface for controllers that manage memory mapping via the MAP instruction.
 * This interface allows the 45GS02 CPU to update mapping state without direct
 * coupling to the MapMmu class, solving cross-plugin symbol visibility issues.
 */
class IMapController {
public:
    virtual ~IMapController() {}

    // Update the current MAP state (called by MAP instruction)
    virtual void setMapState(const MapState& state) = 0;

    // Get the current MAP state
    virtual const MapState& getMapState() const = 0;

    // Reset MAP state to default (all blocks disabled)
    virtual void clearMapState() = 0;

    // Access the underlying physical bus (bypasses MAP translation and overlays).
    // Used for 32-bit indirect long addressing. Returns nullptr if not available.
    virtual IBus* getPhysBus() const { return nullptr; }
};
