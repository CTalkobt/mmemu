#include "sparse_memory_bus.h"
#include "libdebug/main/execution_observer.h"
#include <cstring>
#include <algorithm>
#include <cstdlib>

static const uint32_t PAGE_SIZE = 4096;

SparseMemoryBus::SparseMemoryBus(const std::string& name, uint32_t addrBits)
    : m_name(name), m_pageBits(12)
{
    m_config.addrBits = addrBits;
    m_config.dataBits = 8;
    m_config.role = BusRole::DATA;
    m_config.littleEndian = true;
    m_config.addrMask = (addrBits == 32) ? 0xFFFFFFFFu : (1u << addrBits) - 1;
    m_addrMask = m_config.addrMask;
}

SparseMemoryBus::~SparseMemoryBus() {
    for (auto& [pageAddr, pageData] : m_pages) {
        delete[] pageData;
    }
}

const SparseRegion* SparseMemoryBus::findRegion(uint32_t addr) const {
    for (const auto& region : m_regions) {
        if (addr >= region.base && addr < (region.base + region.size)) {
            return &region;
        }
    }
    return nullptr;
}

uint8_t* SparseMemoryBus::allocatePage(uint32_t pageAddr) {
    auto it = m_pages.find(pageAddr);
    if (it != m_pages.end()) {
        return it->second;
    }
    uint8_t* page = new uint8_t[PAGE_SIZE];
    std::memset(page, 0xFF, PAGE_SIZE);  // Unallocated reads return $FF
    m_pages[pageAddr] = page;
    return page;
}

uint8_t SparseMemoryBus::read8(uint32_t addr) {
    addr &= m_addrMask;

    if (m_observer) {
        uint8_t val = peek8(addr);
        m_observer->onMemoryRead(this, addr, val);
        return val;
    }
    return peek8(addr);
}

uint8_t SparseMemoryBus::read8Raw(uint32_t addr) {
    addr &= m_addrMask;
    uint8_t val = peek8Raw(addr);
    if (m_observer) {
        m_observer->onMemoryRead(this, addr, val);
    }
    return val;
}

uint8_t SparseMemoryBus::peek8(uint32_t addr) {
    addr &= m_addrMask;
    return peek8Raw(addr);
}

uint8_t SparseMemoryBus::peek8Raw(uint32_t addr) {
    addr &= m_addrMask;

    const SparseRegion* region = findRegion(addr);
    if (region) {
        return region->data[addr - region->base];
    }

    uint32_t pageAddr = getPageAddr(addr);
    uint32_t offset = getPageOffset(addr);

    auto it = m_pages.find(pageAddr);
    if (it != m_pages.end()) {
        return it->second[offset];
    }

    return 0xFF;  // Unallocated page
}

void SparseMemoryBus::write8(uint32_t addr, uint8_t val) {
    addr &= m_addrMask;
    uint8_t before = peek8Raw(addr);

    const SparseRegion* region = findRegion(addr);
    if (region) {
        if (!region->writable) {
            if (m_observer) {
                m_observer->onMemoryWrite(this, addr, before, val);
            }
            return;  // ROM: write ignored
        }
        // Writable region: write to underlying data (const_cast needed here)
        const_cast<uint8_t*>(region->data)[addr - region->base] = val;
    } else {
        uint32_t pageAddr = getPageAddr(addr);
        uint32_t offset = getPageOffset(addr);
        uint8_t* page = allocatePage(pageAddr);
        page[offset] = val;
    }

    if (m_observer) {
        m_observer->onMemoryWrite(this, addr, before, val);
    }

    m_writeLog.push({addr, before, val});
}

void SparseMemoryBus::reset() {
    clearWriteLog();
    for (auto& [pageAddr, pageData] : m_pages) {
        delete[] pageData;
    }
    m_pages.clear();
}

size_t SparseMemoryBus::stateSize() const {
    size_t size = sizeof(uint32_t);  // page count
    size += m_pages.size() * (sizeof(uint32_t) + PAGE_SIZE);  // each page: addr + data
    return size;
}

void SparseMemoryBus::saveState(uint8_t *buf) const {
    uint8_t *ptr = buf;

    uint32_t pageCount = m_pages.size();
    std::memcpy(ptr, &pageCount, sizeof(uint32_t));
    ptr += sizeof(uint32_t);

    for (const auto& [pageAddr, pageData] : m_pages) {
        std::memcpy(ptr, &pageAddr, sizeof(uint32_t));
        ptr += sizeof(uint32_t);
        std::memcpy(ptr, pageData, PAGE_SIZE);
        ptr += PAGE_SIZE;
    }
}

void SparseMemoryBus::loadState(const uint8_t *buf) {
    reset();

    const uint8_t *ptr = buf;

    uint32_t pageCount;
    std::memcpy(&pageCount, ptr, sizeof(uint32_t));
    ptr += sizeof(uint32_t);

    for (uint32_t i = 0; i < pageCount; ++i) {
        uint32_t pageAddr;
        std::memcpy(&pageAddr, ptr, sizeof(uint32_t));
        ptr += sizeof(uint32_t);

        uint8_t* page = allocatePage(pageAddr);
        std::memcpy(page, ptr, PAGE_SIZE);
        ptr += PAGE_SIZE;
    }
}

void SparseMemoryBus::getWrites(uint32_t *addrs, uint8_t *before,
                                uint8_t *after, int max) const {
    int count = std::min((int)m_writeLog.size(), max);
    for (int i = 0; i < count; ++i) {
        addrs[i] = m_writeLog[i].address;
        before[i] = m_writeLog[i].before;
        after[i] = m_writeLog[i].after;
    }
}

void SparseMemoryBus::addRegion(uint32_t base, uint32_t size, const uint8_t* data, bool writable) {
    m_regions.push_back({base, size, data, writable});
}

void SparseMemoryBus::addRomOverlay(uint32_t base, uint32_t size, const uint8_t* data) {
    addRegion(base, size, data, false);
}

void SparseMemoryBus::removeRomOverlay(uint32_t base) {
    auto it = std::remove_if(m_regions.begin(), m_regions.end(), [base](const SparseRegion& r) {
        return r.base == base;
    });
    m_regions.erase(it, m_regions.end());
}

bool SparseMemoryBus::isHaltRequested() {
    return false;
}
