#include "memory_bus.h"
#include "libdebug/main/execution_observer.h"
#include <cstring>
#include <algorithm>

FlatMemoryBus::FlatMemoryBus(const std::string& name, uint32_t addrBits)
    : m_name(name)
{
    m_config.addrBits = addrBits;
    m_config.dataBits = 8;
    m_config.role = BusRole::DATA;
    m_config.littleEndian = true;
    m_config.addrMask = (addrBits == 32) ? 0xFFFFFFFFu : (1u << addrBits) - 1;

    m_size = 1ull << addrBits;
    m_data = new uint8_t[m_size];
    std::memset(m_data, 0, m_size);
}

FlatMemoryBus::~FlatMemoryBus() {
    delete[] m_data;
}

const RomOverlay* FlatMemoryBus::findOverlay(uint32_t addr) const {
    for (const auto& overlay : m_overlays) {
        if (addr >= overlay.base && addr < (overlay.base + overlay.size)) {
            return &overlay;
        }
    }
    return nullptr;
}

uint8_t FlatMemoryBus::read8(uint32_t addr) {
    addr &= m_config.addrMask;
    if (addr < m_ioLowBase) {
        const RomOverlay* overlay = findOverlay(addr);
        uint8_t val;
        if (overlay) {
            val = overlay->data[addr - overlay->base];
        } else {
            val = m_data[addr];
        }
        if (m_observer) m_observer->onMemoryRead(this, addr, val);
        return val;
    }
    if (m_ioRead) {
        uint8_t ioVal;
        if (m_ioRead(this, addr, &ioVal)) {
            if (m_observer) m_observer->onMemoryRead(this, addr, ioVal);
            return ioVal;
        }
    }
    return read8Raw(addr);
}

uint8_t FlatMemoryBus::read8Raw(uint32_t addr) {
    addr &= m_config.addrMask;
    uint8_t val;
    const RomOverlay* overlay = findOverlay(addr);
    if (overlay) {
        val = overlay->data[addr - overlay->base];
    } else {
        val = m_data[addr];
    }
    if (m_observer) m_observer->onMemoryRead(this, addr, val);
    return val;
}

void FlatMemoryBus::write8(uint32_t addr, uint8_t val) {
    addr &= m_config.addrMask;
    if (addr < m_ioLowBase) {
        const RomOverlay* overlay = findOverlay(addr);
        uint8_t before;
        if (overlay) {
            before = overlay->data[addr - overlay->base];
        } else {
            before = m_data[addr];
        }
        if (overlay && !overlay->writable) {
            if (m_observer) m_observer->onMemoryWrite(this, addr, before, val);
            return;
        }
        m_data[addr] = val;
        if (m_observer) m_observer->onMemoryWrite(this, addr, before, val);
        m_writeLog.push({addr, before, val});
        return;
    }
    uint8_t before = peek8(addr);
    if (m_ioWrite && m_ioWrite(this, addr, val)) {
        if (m_observer) m_observer->onMemoryWrite(this, addr, before, val);
        return;
    }
    const RomOverlay* overlay = findOverlay(addr);

    if (overlay) {
        if (overlay->writable) {
            m_data[addr] = val;
        } else {
            // ROM: write ignored
            if (m_observer) m_observer->onMemoryWrite(this, addr, before, val);
            return;
        }
    } else {
        m_data[addr] = val;
    }

    if (m_observer) m_observer->onMemoryWrite(this, addr, before, val);

    m_writeLog.push({addr, before, val});
}

uint8_t FlatMemoryBus::peek8(uint32_t addr) {
    addr &= m_config.addrMask;
    if (addr < m_ioLowBase) {
        const RomOverlay* overlay = findOverlay(addr);
        if (overlay) {
            return overlay->data[addr - overlay->base];
        }
        return m_data[addr];
    }
    const auto& peekHook = m_ioPeek ? m_ioPeek : m_ioRead;
    if (peekHook) {
        uint8_t ioVal;
        if (peekHook(this, addr, &ioVal)) {
            return ioVal;
        }
    }
    return peek8Raw(addr);
}

uint8_t FlatMemoryBus::peek8Raw(uint32_t addr) {
    addr &= m_config.addrMask;
    const RomOverlay* overlay = findOverlay(addr);
    if (overlay) {
        return overlay->data[addr - overlay->base];
    }
    return m_data[addr];
}

void FlatMemoryBus::reset() {
    clearWriteLog();
}

size_t FlatMemoryBus::stateSize() const {
    return m_size;
}

void FlatMemoryBus::saveState(uint8_t *buf) const {
    std::memcpy(buf, m_data, m_size);
}

void FlatMemoryBus::loadState(const uint8_t *buf) {
    std::memcpy(m_data, buf, m_size);
}

void FlatMemoryBus::getWrites(uint32_t *addrs, uint8_t *before,
                               uint8_t *after, int max) const {
    int count = std::min((int)m_writeLog.size(), max);
    for (int i = 0; i < count; ++i) {
        addrs[i] = m_writeLog[i].address;
        before[i] = m_writeLog[i].before;
        after[i] = m_writeLog[i].after;
    }
}

void FlatMemoryBus::addOverlay(uint32_t base, uint32_t size, const uint8_t* data, bool writable) {
    m_overlays.push_back({base, size, data, writable});
}

void FlatMemoryBus::addRomOverlay(uint32_t base, uint32_t size, const uint8_t* data) {
    addOverlay(base, size, data, false);
}

void FlatMemoryBus::removeRomOverlay(uint32_t base) {
    auto it = std::remove_if(m_overlays.begin(), m_overlays.end(), [base](const RomOverlay& o) {
        return o.base == base;
    });
    m_overlays.erase(it, m_overlays.end());
}

void FlatMemoryBus::setIoHooks(std::function<bool(IBus*, uint32_t, uint8_t*)> readFn,
                                std::function<bool(IBus*, uint32_t, uint8_t)>  writeFn,
                                std::function<bool(IBus*, uint32_t, uint8_t*)> peekFn) {
    m_ioRead  = std::move(readFn);
    m_ioWrite = std::move(writeFn);
    m_ioPeek  = std::move(peekFn);
}

bool FlatMemoryBus::isHaltRequested() {
    if (m_haltCheck) return m_haltCheck();
    return false;
}
