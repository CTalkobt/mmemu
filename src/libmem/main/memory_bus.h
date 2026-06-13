#pragma once

#include "ibus.h"
#include "util/circular_buffer.h"
#include <functional>
#include <vector>
#include <string>

/**
 * Entry in the write log.
 */
#ifndef WRITELOGENTRY_DEFINED
#define WRITELOGENTRY_DEFINED
struct WriteLogEntry {
    uint32_t address;
    uint8_t  before;
    uint8_t  after;
};
#endif

/**
 * ROM Overlay mapping.
 */
struct RomOverlay {
    uint32_t base;
    uint32_t size;
    const uint8_t* data;
    bool writable;
};

/**
 * Generic heap-allocated flat memory bus.
 */
class FlatMemoryBus : public IBus {
public:
    FlatMemoryBus(const std::string& name, uint32_t addrBits);
    virtual ~FlatMemoryBus();

    const BusConfig& config() const override { return m_config; }
    const char*      name()   const override { return m_name.c_str(); }

    uint8_t  read8 (uint32_t addr) override;
    void     write8(uint32_t addr, uint8_t val) override;

    uint8_t  peek8(uint32_t addr) override;

    /**
     * Bypasses I/O hooks and reads directly from RAM or overlays.
     */
    uint8_t read8Raw(uint32_t addr);
    uint8_t peek8Raw(uint32_t addr);

    void reset() override;

    size_t stateSize()             const override;
    void   saveState(uint8_t *buf) const override;
    void   loadState(const uint8_t *buf) override;

    int  writeCount()                                               const override { return (int)m_writeLog.size(); }
    void getWrites(uint32_t *addrs, uint8_t *before,
                            uint8_t *after, int max)                        const override;
    void clearWriteLog() override { m_writeLog.clear(); }

    /**
     * Add a ROM overlay to the bus (internal, low-level).
     */
    void addOverlay(uint32_t base, uint32_t size, const uint8_t* data, bool writable = false);

    /**
     * IBus ROM overlay API — used by ICartridgeHandler implementations.
     */
    void addRomOverlay   (uint32_t base, uint32_t size, const uint8_t* data) override;
    void removeRomOverlay(uint32_t base) override;

    bool isHaltRequested() override;

    /**
     * Direct pointer to the underlying flat RAM array (bypasses overlays and IO
     * hooks).  Use for banking controllers that need to return underlying RAM
     * content when ROM is banked out.  Size is always 2^addrBits bytes.
     */
    const uint8_t* rawData() const { return m_data; }

    /**
     * Register IO dispatch hooks.  The read hook returns true and fills *val if
     * it owns the address; the write hook returns true if it owns the address.
     * Called before overlay / RAM on every read8 / write8.
     */
    void setIoHooks(std::function<bool(IBus*, uint32_t, uint8_t*)> readFn,
                    std::function<bool(IBus*, uint32_t, uint8_t)>  writeFn);

    void setIoLowBase(uint32_t base) { m_ioLowBase = base; }

    void setHaltCheck(std::function<bool()> checkFn) { m_haltCheck = std::move(checkFn); }

private:
    std::string m_name;
    BusConfig   m_config;
    uint8_t*    m_data;
    size_t      m_size;

    std::vector<RomOverlay> m_overlays;
    CircularBuffer<WriteLogEntry> m_writeLog;

    std::function<bool(IBus*, uint32_t, uint8_t*)> m_ioRead;
    std::function<bool(IBus*, uint32_t, uint8_t)>  m_ioWrite;
    std::function<bool()>                          m_haltCheck;
    uint32_t m_ioLowBase = UINT32_MAX;

    const RomOverlay* findOverlay(uint32_t addr) const;
};
