#pragma once

#include <cstdint>

class IBus;

struct MapState {
    uint32_t offsets[8];  // 20-bit offset for each 8KB block
    uint8_t  enables;     // bitmask: bit i = block i enabled
    uint32_t megabyteLow;  // megabyte base for lower 32KB (set via MAP with X==0x0F)
    uint32_t megabyteHigh; // megabyte base for upper 32KB (set via MAP with Z==0x0F)
};

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

    // Translate a 16-bit virtual address to a 28-bit physical address
    // using the current MAP state. Used by physical-address breakpoints (#73).
    virtual uint32_t resolvePhysical(uint32_t vaddr) const {
        const MapState& ms = getMapState();
        vaddr &= 0xFFFF;
        int block = (vaddr >> 13) & 7;
        if (ms.enables & (1 << block)) {
            uint32_t offset = ms.offsets[block] & 0xFFFFF;
            uint32_t megabyte = (block < 4) ? ms.megabyteLow : ms.megabyteHigh;

            // Hardware-accurate addressing with 12-bit wrap on bits 19:8
            uint32_t offsetHigh12 = (offset >> 8) & 0xFFF;
            uint32_t vaddrHigh = (vaddr >> 8) & 0xFF;
            uint32_t sum12bit = (offsetHigh12 + vaddrHigh) & 0xFFF;
            uint32_t vaddrLow8 = vaddr & 0xFF;
            uint32_t physAddr = (sum12bit << 8) | vaddrLow8;

            return megabyte + physAddr;
        }
        return vaddr;
    }
};
