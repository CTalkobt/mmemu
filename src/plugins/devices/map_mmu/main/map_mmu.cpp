#include "map_mmu.h"
#include "libmem/main/sparse_memory_bus.h"
#include "libdebug/main/execution_observer.h"
#include <cstring>
#include <cstdio>

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

    static uint64_t translateCount = 0;
    bool shouldLog = false;

    // Log translations if enabled, or for specific addresses during initial boot
    if (m_logTranslations) {
        shouldLog = true;
    } else if ((vaddr >= 0xFF80 && vaddr <= 0xFFFF) || (vaddr >= 0xE4B0 && vaddr <= 0xE4D8) ||
               (vaddr >= 0xC800 && vaddr <= 0xC820) || translateCount < 20) {
        shouldLog = true;
    }

    if (m_mapState.enables & (1 << block)) {
        uint32_t offset = m_mapState.offsets[block] & 0xFFFFF;  // 20-bit offset
        uint32_t megabyte = (block < 4) ? m_mapState.megabyteLow
                                        : m_mapState.megabyteHigh;

        // HARDWARE-ACCURATE ADDRESSING:
        // VHDL hardware does 12-bit addition on bits 19:8 with NO carry propagation into megabyte field.
        // Bits 19:8 = (offset[19:8] + vaddr[15:8]) with 12-bit wrap (overflow discarded)
        // Bits 7:0 = vaddr[7:0]
        // This prevents spurious address wrapping when offset + vaddr overflows bit 19.
        uint32_t offsetHigh12 = (offset >> 8) & 0xFFF;      // Extract bits 19:8 of offset
        uint32_t vaddrHigh = (vaddr >> 8) & 0xFF;           // Extract bits 15:8 of vaddr
        uint32_t sum12bit = (offsetHigh12 + vaddrHigh) & 0xFFF;  // 12-bit wrap, no carry to bit 20
        uint32_t offsetLow8 = offset & 0xFF;                // Bits 7:0 of offset
        uint32_t vaddrLow8 = vaddr & 0xFF;                  // Bits 7:0 of vaddr

        // Combine: bits 19:8 (wrapped) | bits 7:0 (from vaddr or offset+vaddr)
        // Since hardware takes vaddr[7:0] directly, we use that
        uint32_t physAddr = (sum12bit << 8) | vaddrLow8;
        uint32_t result = megabyte + physAddr;

        if (shouldLog) {
            fprintf(stderr, "[MapMmu] MAPPED #%llu: vaddr=$%04X (block %d) -> phys=$%06X "
                "(offset=$%05X mb=$%06X)\n",
                (unsigned long long)translateCount, vaddr, block, result, offset, megabyte);
            fflush(stderr);
        }
        translateCount++;
        return result;
    }

    if (shouldLog) {
        fprintf(stderr, "[MapMmu] PASSTHRU #%llu: vaddr=$%04X (block %d) -> phys=$%04X (MAP disabled)\n",
            (unsigned long long)translateCount, vaddr, block, vaddr);
        fflush(stderr);
    }
    translateCount++;
    return vaddr;  // Passthrough: physical address = virtual address (C64 mode)
}

uint8_t MapMmu::read8(uint32_t addr) {
    addr &= 0xFFFF;

    static uint64_t readCount = 0;
    bool logThisRead = m_logMemAccess || (addr >= 0xFF80 && addr <= 0xFFFF) ||
                       (addr >= 0xE4B0 && addr <= 0xE4D8) ||
                       readCount < 20;
    if (logThisRead) {
        fprintf(stderr, "[MapMmu] READ #%llu: addr=$%04X\n", (unsigned long long)readCount, addr);
        fflush(stderr);
    }
    readCount++;

    // Hypervisor overlay: $8000-$BFFF reads from hypervisor RAM when active
    if (m_hyperActive && m_hyperActive() && m_hyperRam &&
        addr >= m_hyperBase && addr < m_hyperBase + m_hyperSize) {
        uint8_t val = m_hyperRam[addr - m_hyperBase];
        if (logThisRead) {
            fprintf(stderr, "  -> HYPERVISOR: val=$%02X\n", val);
            fflush(stderr);
        }
        if (m_observer) m_observer->onMemoryRead(this, addr, val);
        return val;
    }

    // Check I/O hooks first (virtual address space)
    uint8_t ioVal = 0;
    if (m_ioRead && m_ioRead(this, addr, &ioVal)) {
        if (logThisRead) {
            fprintf(stderr, "  -> I/O: val=$%02X\n", ioVal);
            fflush(stderr);
        }
        if (m_observer) {
            m_observer->onMemoryRead(this, addr, ioVal);
        }
        return ioVal;
    }

    uint32_t physAddr = translate(addr);
    uint8_t val = m_physBus->read8(physAddr);
    if (logThisRead) {
        fprintf(stderr, "  -> PHYS $%06X: val=$%02X\n", physAddr, val);
        fflush(stderr);
    }
    if (m_observer) {
        m_observer->onMemoryRead(this, addr, val);
    }
    return val;
}

uint8_t MapMmu::peek8(uint32_t addr) {
    addr &= 0xFFFF;

    // Hypervisor overlay
    if (m_hyperActive && m_hyperActive() && m_hyperRam &&
        addr >= m_hyperBase && addr < m_hyperBase + m_hyperSize) {
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
    uint8_t before = peek8(addr);

    static uint64_t writeCount = 0;
    bool logThisWrite = m_logMemAccess || (addr >= 0xFF80 && addr <= 0xFFFF) ||
                        (addr >= 0xE4B0 && addr <= 0xE4D8) ||
                        writeCount < 20;
    if (logThisWrite) {
        fprintf(stderr, "[MapMmu] WRITE #%llu: addr=$%04X val=$%02X (was $%02X)\n",
            (unsigned long long)writeCount, addr, val, before);
        fflush(stderr);
    }
    writeCount++;

    // Hypervisor overlay: writes to $8000-$BFFF go to hypervisor RAM
    if (m_hyperActive && m_hyperActive() && m_hyperRam &&
        addr >= m_hyperBase && addr < m_hyperBase + m_hyperSize) {
        m_hyperRam[addr - m_hyperBase] = val;
        if (logThisWrite) {
            fprintf(stderr, "  -> HYPERVISOR\n");
            fflush(stderr);
        }
        if (m_observer) m_observer->onMemoryWrite(this, addr, before, val);
        return;
    }

    // Check I/O hooks first (virtual address space)
    if (m_ioWrite && m_ioWrite(this, addr, val)) {
        if (logThisWrite) {
            fprintf(stderr, "  -> I/O\n");
            fflush(stderr);
        }
        if (m_observer) {
            m_observer->onMemoryWrite(this, addr, before, val);
        }
        return;
    }

    uint32_t physAddr = translate(addr);
    if (logThisWrite) {
        fprintf(stderr, "  -> PHYS $%06X\n", physAddr);
        fflush(stderr);
    }
    m_physBus->write8(physAddr, val);

    if (m_observer) {
        m_observer->onMemoryWrite(this, addr, before, val);
    }
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
    fprintf(stderr, "[MapMmu] setMapState: enables=$%02X mbLow=$%06X mbHigh=$%06X\n",
        state.enables, state.megabyteLow, state.megabyteHigh);
    for (int i = 0; i < 8; i++) {
        if (state.enables & (1 << i)) {
            fprintf(stderr, "  Block %d: offset=$%05X\n", i, state.offsets[i]);
        }
    }
    fflush(stderr);
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
