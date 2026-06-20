#pragma once

#include <cstdint>
#include <cstddef>

class ExecutionObserver;

/**
 * Interface for state snapshot support.
 */
class ISnapshotable {
public:
    virtual ~ISnapshotable() {}
    virtual size_t stateSize()             const = 0;
    virtual void   saveState(uint8_t *buf) const = 0;
    virtual void   loadState(const uint8_t *buf) = 0;
};

/**
 * Interface for write log support.
 */
class IBusWriteLog {
public:
    virtual ~IBusWriteLog() {}
    virtual int  writeCount() const = 0;
    virtual void getWrites(uint32_t *addrs, uint8_t *before,
                            uint8_t *after, int max) const = 0;
    virtual void clearWriteLog() = 0;
};

/**
 * Role of a specific bus in the system.
 */
enum class BusRole {
    DATA,       // Main memory or data bus
    CODE,       // Instruction bus (for Harvard architectures)
    IO_PORT,    // Separate I/O space (e.g., Z80 IN/OUT)
    DMA         // Dedicated DMA view
};

/**
 * Configuration for an address bus.
 */
struct BusConfig {
    uint32_t addrBits;      // 16, 24, 28, 32
    uint32_t dataBits;      // 8, 16, 32
    BusRole  role;
    bool     littleEndian;
    uint32_t addrMask;      // (addrBits == 32) ? 0xFFFFFFFF : ((1u << addrBits) - 1)
};

/**
 * Abstract interface for an address / memory bus.
 */
class IBus : public ISnapshotable, public IBusWriteLog {
public:
    virtual ~IBus() {}

    virtual const BusConfig& config() const = 0;
    virtual const char*      name()   const = 0;

    /**
     * Primary 8-bit access.
     */
    virtual uint8_t  read8 (uint32_t addr) = 0;
    virtual void     write8(uint32_t addr, uint8_t val) = 0;

    /**
     * Multi-byte helpers. Default implementation assembles from read8/write8.
     */
    virtual uint16_t read16 (uint32_t addr);
    virtual void     write16(uint32_t addr, uint16_t val);
    virtual uint32_t read32 (uint32_t addr);
    virtual void     write32(uint32_t addr, uint32_t val);

    /**
     * Side-effect-free read for debugger/disassembler.
     * Must be implemented by subclasses to be side-effect free.
     */
    virtual uint8_t peek8(uint32_t addr) = 0;

    virtual void reset() {}

    /**
     * ROM overlay management for cartridge attachment.
     * Default implementations are no-ops; bus types that support overlays
     * (e.g. FlatMemoryBus) override these.
     */
    virtual void addRomOverlay   (uint32_t base, uint32_t size, const uint8_t* data) {}
    virtual void removeRomOverlay(uint32_t base) {}

    virtual bool isHaltRequested() { return false; }

    void setObserver(ExecutionObserver* obs) { m_observer = obs; }
    ExecutionObserver* getObserver() const { return m_observer; }

protected:
    ExecutionObserver* m_observer = nullptr;
};
