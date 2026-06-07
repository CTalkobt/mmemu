#pragma once

#include "libmem/main/ibus.h"
#include "imap_controller.h"
#include <cstdint>
#include <string>
#include <functional>

class SparseMemoryBus;

/**
 * MEGA65 Memory Address Mapper (MAP) - Address Translation Only
 *
 * MapMmu is a pure address translator that sits between the 45GS02 CPU's 16-bit
 * virtual address space and the SparseMemoryBus's 28-bit physical address space.
 *
 * SCOPE (What MapMmu Does):
 * - Translate 16-bit virtual addresses to 28-bit physical addresses based on MAP state
 * - MAP instruction sets 8 × 8KB block mappings (4 for $0000–$7FFF, 4 for $8000–$FFFF)
 * - When a block's enable bit is set: phys = (offset[block] << 8) | (vaddr & 0x1FFF)
 * - When disabled: passthrough (phys = vaddr for C64 mode)
 * - Preserve all reads/writes through translation
 *
 * OUT OF SCOPE (Machine Factory Responsibilities):
 * - C64 banking ($0001: LORAM/HIRAM/CHAREN): Handled by C64BankController when in C64 mode
 * - I/O personality ($D02F): Managed by machine factory during setup
 * - ROM overlay management: Machine factory loads ROMs and calls SparseMemoryBus::addRegion
 *
 * USAGE:
 * 1. Machine factory creates SparseMemoryBus (28-bit physical bus)
 * 2. Machine factory creates MapMmu pointing to SparseMemoryBus
 * 3. Machine factory sets MapMmu as CPU's IBus pointer
 * 4. When CPU executes MAP instruction, it calls mapMmu->setMapState(newState)
 * 5. All subsequent reads/writes route through MapMmu's translation
 *
 * In C64 MODE (MAP disabled, default):
 * - CPU addresses 0x0000–0xFFFF map directly to physical 0x0000–0xFFFF
 * - SparseMemoryBus overlays (C64 ROM) are pre-wired by machine factory
 * - No MAP state translation occurs
 *
 * In MEGA65 MODE (MAP enabled):
 * - CPU addresses route through MAP block translations
 * - Full 28-bit physical space becomes accessible
 * - C64 ROM overlays are typically disabled
 */

struct MapState {
    uint32_t offsets[8];  // 20-bit offset for each 8KB block
    uint8_t  enables;     // bitmask: bit i = block i enabled
    uint32_t megabyteLow;  // megabyte base for lower 32KB (set via MAP with X==0x0F)
    uint32_t megabyteHigh; // megabyte base for upper 32KB (set via MAP with Z==0x0F)
};

class MapMmu : public IBus, public IMapController {
public:
    MapMmu(const std::string& name, SparseMemoryBus* physBus);
    virtual ~MapMmu();

    const BusConfig& config() const override { return m_config; }
    const char*      name()   const override { return m_name.c_str(); }

    uint8_t  read8 (uint32_t addr) override;
    void     write8(uint32_t addr, uint8_t val) override;
    uint8_t  peek8(uint32_t addr) override;

    void reset() override;

    size_t stateSize()             const override;
    void   saveState(uint8_t *buf) const override;
    void   loadState(const uint8_t *buf) override;

    int  writeCount()                                               const override { return 0; }
    void getWrites(uint32_t *addrs, uint8_t *before,
                            uint8_t *after, int max)                        const override {}
    void clearWriteLog() override {}

    void setMapState(const MapState& state) override;
    const MapState& getMapState() const override { return m_mapState; }
    void clearMapState() override;

    /**
     * Set I/O hooks for the virtual address space (before translation).
     */
    void setIoHooks(std::function<bool(IBus*, uint32_t, uint8_t*)> readFn,
                    std::function<bool(IBus*, uint32_t, uint8_t)>  writeFn);

    /**
     * Set hypervisor overlay so debugger reads see the hypervisor ROM/RAM
     * at $8000-$BFFF when the CPU is in hypervisor mode.
     *
     * @param isActive  Returns true when hypervisor mode is active
     * @param ram       Writable copy of hypervisor ROM (CPU may modify it)
     * @param base      Virtual address base (typically 0x8000)
     * @param size      Size in bytes (typically 16384)
     */
    void setHypervisorOverlay(std::function<bool()> isActive,
                              uint8_t* ram, uint16_t base, uint32_t size);

private:
    std::string m_name;
    BusConfig   m_config;
    SparseMemoryBus* m_physBus;
    MapState m_mapState;

    std::function<bool(IBus*, uint32_t, uint8_t*)> m_ioRead;
    std::function<bool(IBus*, uint32_t, uint8_t)>  m_ioWrite;

    // Hypervisor overlay (visible to debugger when hypervisor mode is active)
    std::function<bool()> m_hyperActive;
    uint8_t*  m_hyperRam  = nullptr;
    uint16_t  m_hyperBase = 0;
    uint32_t  m_hyperSize = 0;

    uint32_t translate(uint32_t vaddr) const;
};
