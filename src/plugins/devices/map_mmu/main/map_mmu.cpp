#include "map_mmu.h"
#include "libmem/main/sparse_memory_bus.h"
#include "libdebug/main/execution_observer.h"
#include <cstring>

MapMmu::MapMmu(const std::string& name, SparseMemoryBus* physBus)
    : m_name(name), m_physBus(physBus)
{
    m_config.addrBits = 16;
    m_config.dataBits = 8;
    m_config.role = BusRole::DATA;
    m_config.littleEndian = true;
    m_config.addrMask = 0xFFFF;

    std::memset(&m_mapState, 0, sizeof(MapState));
}

MapMmu::~MapMmu() {
}

uint32_t MapMmu::translate(uint32_t vaddr) const {
    vaddr &= 0xFFFF;
    int block = (vaddr >> 13) & 7;  // which 8KB block (0-7)

    if (m_mapState.enables & (1 << block)) {
        uint32_t offset = m_mapState.offsets[block] & 0xFFFFF;  // 20-bit offset
        uint32_t megabyte = (block < 4) ? m_mapState.megabyteLow
                                        : m_mapState.megabyteHigh;
        return (megabyte + (offset << 8) + (vaddr & 0x1FFF)) & m_physBus->config().addrMask;
    }

    return vaddr;  // Passthrough: physical address = virtual address (C64 mode)
}

uint8_t MapMmu::read8(uint32_t addr) {
    addr &= 0xFFFF;

    // Check I/O hooks first (virtual address space)
    uint8_t ioVal = 0;
    if (m_ioRead && m_ioRead(this, addr, &ioVal)) {
        if (m_observer) {
            m_observer->onMemoryRead(this, addr, ioVal);
        }
        return ioVal;
    }

    uint32_t physAddr = translate(addr);
    uint8_t val = m_physBus->read8(physAddr);
    if (m_observer) {
        m_observer->onMemoryRead(this, addr, val);
    }
    return val;
}

uint8_t MapMmu::peek8(uint32_t addr) {
    addr &= 0xFFFF;

    // Check I/O hooks first (virtual address space)
    uint8_t ioVal = 0;
    if (m_ioRead && m_ioRead(this, addr, &ioVal)) {
        return ioVal;
    }

    uint32_t physAddr = translate(addr);
    return m_physBus->peek8(physAddr);
}

void MapMmu::write8(uint32_t addr, uint8_t val) {
    addr &= 0xFFFF;
    uint8_t before = peek8(addr);

    // Check I/O hooks first (virtual address space)
    if (m_ioWrite && m_ioWrite(this, addr, val)) {
        if (m_observer) {
            m_observer->onMemoryWrite(this, addr, before, val);
        }
        return;
    }

    uint32_t physAddr = translate(addr);
    m_physBus->write8(physAddr, val);

    if (m_observer) {
        m_observer->onMemoryWrite(this, addr, before, val);
    }
}

void MapMmu::setIoHooks(std::function<bool(IBus*, uint32_t, uint8_t*)> readFn,
                        std::function<bool(IBus*, uint32_t, uint8_t)>  writeFn)
{
    m_ioRead = std::move(readFn);
    m_ioWrite = std::move(writeFn);
}

void MapMmu::setMapState(const MapState& state) {
    m_mapState = state;
}

void MapMmu::reset() {
    clearMapState();
}

void MapMmu::clearMapState() {
    std::memset(&m_mapState, 0, sizeof(MapState));
}

size_t MapMmu::stateSize() const {
    return sizeof(MapState);
}

void MapMmu::saveState(uint8_t *buf) const {
    std::memcpy(buf, &m_mapState, sizeof(MapState));
}

void MapMmu::loadState(const uint8_t *buf) {
    std::memcpy(&m_mapState, buf, sizeof(MapState));
}
