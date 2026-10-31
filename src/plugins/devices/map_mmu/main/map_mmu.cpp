#include "map_mmu.h"
#include "libmem/main/sparse_memory_bus.h"
#include "libdebug/main/execution_observer.h"
#include <cstring>
#include "include/util/logging.h"

/** "mapmmu" logger: MAP changes at debug; per-access tracing (map log on/mem) at info. */
static spdlog::logger* mapLog() {
    static auto logger = LogRegistry::instance().getLogger("mapmmu");
    return logger.get();
}

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

        // HARDWARE-ACCURATE ADDRESSING:
        // VHDL hardware does 12-bit addition on bits 19:8 with NO carry propagation into megabyte field.
        // Bits 19:8 = (offset[19:8] + vaddr[15:8]) with 12-bit wrap (overflow discarded)
        // Bits 7:0 = vaddr[7:0]
        uint32_t sum12bit = (((offset >> 8) & 0xFFF) + ((vaddr >> 8) & 0xFF)) & 0xFFF;
        uint32_t result = megabyte + ((sum12bit << 8) | (vaddr & 0xFF));

        if (m_logTranslations)
            mapLog()->info("MAPPED vaddr=${:04X} (block {}) -> phys=${:07X} (offset=${:05X} mb=${:07X})",
                           vaddr, block, result, offset, megabyte);
        return result;
    }

    if (m_logTranslations)
        mapLog()->info("PASSTHRU vaddr=${:04X} (block {}) (MAP disabled)", vaddr, block);
    return vaddr;  // Passthrough: physical address = virtual address (C64 mode)
}

uint8_t MapMmu::read8(uint32_t addr) {
    addr &= 0xFFFF;

    // Hypervisor overlay: $8000-$BFFF reads from hypervisor RAM when active
    if (m_hyperRam && addr >= m_hyperBase && addr < m_hyperBase + m_hyperSize &&
        m_hyperActive && m_hyperActive()) {
        uint8_t val = m_hyperRam[addr - m_hyperBase];
        if (m_logMemAccess) mapLog()->info("READ  ${:04X} -> HYPERVISOR ${:02X}", addr, val);
        if (m_observer) m_observer->onMemoryRead(this, addr, val);
        return val;
    }

    // Check I/O hooks first (virtual address space)
    uint8_t ioVal = 0;
    if (m_ioRead && m_ioRead(this, addr, &ioVal)) {
        if (m_logMemAccess) mapLog()->info("READ  ${:04X} -> I/O ${:02X}", addr, ioVal);
        if (m_observer) m_observer->onMemoryRead(this, addr, ioVal);
        return ioVal;
    }

    uint32_t physAddr = translate(addr);
    uint8_t val = m_physBus->read8(physAddr);
    if (m_logMemAccess) mapLog()->info("READ  ${:04X} -> PHYS ${:07X} ${:02X}", addr, physAddr, val);
    if (m_observer) m_observer->onMemoryRead(this, addr, val);
    return val;
}

uint8_t MapMmu::peek8(uint32_t addr) {
    addr &= 0xFFFF;

    // Hypervisor overlay
    if (m_hyperRam && addr >= m_hyperBase && addr < m_hyperBase + m_hyperSize &&
        m_hyperActive && m_hyperActive()) {
        return m_hyperRam[addr - m_hyperBase];
    }

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
    // The "before" value is only needed by the observer (undo log) and access logging
    uint8_t before = (m_observer || m_logMemAccess) ? peek8(addr) : 0;

    // Hypervisor overlay: writes to $8000-$BFFF go to hypervisor RAM
    if (m_hyperRam && addr >= m_hyperBase && addr < m_hyperBase + m_hyperSize &&
        m_hyperActive && m_hyperActive()) {
        m_hyperRam[addr - m_hyperBase] = val;
        if (m_logMemAccess) mapLog()->info("WRITE ${:04X} = ${:02X} (was ${:02X}) -> HYPERVISOR", addr, val, before);
        if (m_observer) m_observer->onMemoryWrite(this, addr, before, val);
        return;
    }

    // Check I/O hooks first (virtual address space)
    if (m_ioWrite && m_ioWrite(this, addr, val)) {
        if (m_logMemAccess) mapLog()->info("WRITE ${:04X} = ${:02X} (was ${:02X}) -> I/O", addr, val, before);
        if (m_observer) m_observer->onMemoryWrite(this, addr, before, val);
        return;
    }

    uint32_t physAddr = translate(addr);
    if (m_logMemAccess)
        mapLog()->info("WRITE ${:04X} = ${:02X} (was ${:02X}) -> PHYS ${:07X}", addr, val, before, physAddr);
    m_physBus->write8(physAddr, val);

    if (m_observer) m_observer->onMemoryWrite(this, addr, before, val);
}

IBus* MapMmu::getPhysBus() const { return m_physBus; }

void MapMmu::setIoHooks(std::function<bool(IBus*, uint32_t, uint8_t*)> readFn,
                        std::function<bool(IBus*, uint32_t, uint8_t)>  writeFn)
{
    m_ioRead = std::move(readFn);
    m_ioWrite = std::move(writeFn);
}

void MapMmu::setHypervisorOverlay(std::function<bool()> isActive,
                                   uint8_t* ram, uint16_t base, uint32_t size) {
    m_hyperActive = std::move(isActive);
    m_hyperRam  = ram;
    m_hyperBase = base;
    m_hyperSize = size;
}

void MapMmu::setMapState(const MapState& state) {
    auto* log = mapLog();
    if (log->should_log(spdlog::level::debug)) {
        log->debug("setMapState: enables=${:02X} mbLow=${:07X} mbHigh=${:07X}",
                   state.enables, state.megabyteLow, state.megabyteHigh);
        for (int i = 0; i < 8; i++)
            if (state.enables & (1 << i)) log->debug("  block {}: offset=${:05X}", i, state.offsets[i]);
    }
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
