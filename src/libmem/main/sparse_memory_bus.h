#pragma once

#include "ibus.h"
#include "util/circular_buffer.h"
#include <unordered_map>
#include <vector>
#include <string>
#include <cstdint>

struct WriteLogEntry {
    uint32_t address;
    uint8_t  before;
    uint8_t  after;
};

struct SparseRegion {
    uint32_t base;
    uint32_t size;
    const uint8_t* data;
    bool writable;
};

class SparseMemoryBus : public IBus {
public:
    SparseMemoryBus(const std::string& name, uint32_t addrBits);
    virtual ~SparseMemoryBus();

    const BusConfig& config() const override { return m_config; }
    const char*      name()   const override { return m_name.c_str(); }

    uint8_t  read8 (uint32_t addr) override;
    void     write8(uint32_t addr, uint8_t val) override;
    uint8_t  peek8(uint32_t addr) override;

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

    void addRegion(uint32_t base, uint32_t size, const uint8_t* data, bool writable = false);
    void addRomOverlay   (uint32_t base, uint32_t size, const uint8_t* data) override;
    void removeRomOverlay(uint32_t base) override;

    bool isHaltRequested() override;

private:
    std::string m_name;
    BusConfig   m_config;
    uint32_t    m_addrMask;
    uint32_t    m_pageBits;  // log2(page size) = 12 (4 KB)

    std::unordered_map<uint32_t, uint8_t*> m_pages;  // pageAddr -> page data
    std::vector<SparseRegion>                m_regions;
    CircularBuffer<WriteLogEntry>           m_writeLog;

    const SparseRegion* findRegion(uint32_t addr) const;
    uint8_t* allocatePage(uint32_t pageAddr);
    uint32_t getPageAddr(uint32_t addr) const { return addr >> m_pageBits; }
    uint32_t getPageOffset(uint32_t addr) const { return addr & ((1u << m_pageBits) - 1); }
};
