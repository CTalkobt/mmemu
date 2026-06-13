#include "c64_bank_controller.h"
#include "map_mmu.h"
#include "libmem/main/sparse_memory_bus.h"

C64BankController::C64BankController(SparseMemoryBus* physBus)
    : m_physBus(physBus)
{
}

void C64BankController::setBasicRom(const uint8_t* data, uint32_t size) {
    m_basicRom  = data;
    m_basicSize = size;
    updateOverlays();
}

void C64BankController::setKernalRom(const uint8_t* data, uint32_t size) {
    m_kernalRom  = data;
    m_kernalSize = size;
    updateOverlays();
}

void C64BankController::setCharRom(const uint8_t* data, uint32_t size) {
    m_charRom  = data;
    m_charSize = size;
    // Char ROM overlay at $D000 is handled via ioRead, not SparseMemoryBus overlays,
    // because it conflicts with I/O device handlers in the same region.
}

void C64BankController::reset() {
    m_portDDR = 0x2F;
    m_portOut = 0x37;  // LORAM=1, HIRAM=1, CHAREN=1 — KERNAL+BASIC visible, I/O active
    updateOverlays();
}

bool C64BankController::isBlockMapped(int block) const {
    if (!m_mapMmu) return false;
    return m_mapMmu->getMapState().enables & (1 << block);
}

void C64BankController::updateOverlays() {
    // BASIC ROM overlay handled via ioRead() — see above.

    // KERNAL and BASIC ROM overlays are now handled via ioRead() interception,
    // not SparseMemoryBus overlays.  This ensures bank switching (changing $01)
    // doesn't destroy ROM data — reads dynamically select ROM vs RAM.

    // Note: Char ROM at $D000 is NOT managed via overlays because it conflicts
    // with I/O handlers. It's served via ioRead() when CHAREN=0.
}

bool C64BankController::ioRead(IBus* /*bus*/, uint32_t addr, uint8_t* val) {
    // CPU I/O port: $00 (DDR), $01 (port)
    if (addr == 0x0000) {
        *val = m_portDDR;
        return true;
    }
    if (addr == 0x0001) {
        // Output bits come from latch, input bits float high
        *val = effectivePort();
        return true;
    }

    // In hypervisor mode, ROM banking is disabled — hypervisor sees raw RAM.
    if (inHypervisor()) return false;

    // C65 ROM banking via $D030 (VIC-III) and C64 $01 port.
    // MAP block mapping overrides both — if block is MAP'd, ROM is not visible.
    // The C65 ROM is a flat 128KB image; CPU address maps 1:1 to file offset.
    uint8_t d030val = d030();

    // $D030 ROM banking regions (C65/MEGA65):
    //   ROM8  (bit 3): $8000-$9FFF → rom offset $8000
    //   ROMA  (bit 4): $A000-$BFFF → rom offset $A000
    //   ROMC  (bit 5): $C000-$CFFF → rom offset $C000
    //   ROME  (bit 7): $E000-$FFFF → rom offset $E000

    // ROM8: $8000-$9FFF (block 4)
    if (addr >= 0x8000 && addr <= 0x9FFF && !isBlockMapped(4)) {
        if (d030val & 0x08) {
            uint32_t off = addr - 0x8000;
            if (m_fullRom && off + 0x8000 < m_fullRomSize)
                { *val = m_fullRom[off + 0x8000]; return true; }
        }
    }

    // BASIC / ROMA: $A000-$BFFF (block 5)
    if (addr >= 0xA000 && addr <= 0xBFFF && !isBlockMapped(5)) {
        if ((d030val & 0x10) || (hiram() && loram())) {
            uint32_t off = addr - 0xA000;
            if (m_fullRom && off + 0xA000 < m_fullRomSize)
                { *val = m_fullRom[off + 0xA000]; return true; }
            if (m_basicRom && off < m_basicSize)
                { *val = m_basicRom[off]; return true; }
        }
    }

    // ROMC: $C000-$CFFF (block 6, lower half)
    if (addr >= 0xC000 && addr <= 0xCFFF && !isBlockMapped(6)) {
        if (d030val & 0x20) {
            uint32_t off = addr - 0xC000;
            if (m_fullRom && off + 0xC000 < m_fullRomSize)
                { *val = m_fullRom[off + 0xC000]; return true; }
        }
    }

    // Character ROM: $D000-$DFFF when HIRAM=1 && CHAREN=0 && block 6 not MAP'd
    if (addr >= 0xD000 && addr <= 0xDFFF) {
        if (hiram() && !charen() && m_charRom && !isBlockMapped(6)) {
            uint32_t off = (addr - 0xD000) & (m_charSize - 1);
            *val = m_charRom[off];
            return true;
        }
    }

    // KERNAL / ROME: $E000-$FFFF (block 7)
    if (addr >= 0xE000 && addr <= 0xFFFF && !isBlockMapped(7)) {
        if ((d030val & 0x80) || hiram()) {
            uint32_t off = addr - 0xE000;
            if (m_fullRom && off + 0xE000 < m_fullRomSize)
                { *val = m_fullRom[off + 0xE000]; return true; }
            if (m_kernalRom && off < m_kernalSize)
                { *val = m_kernalRom[off]; return true; }
        }
    }

    return false;
}

bool C64BankController::ioWrite(IBus* /*bus*/, uint32_t addr, uint8_t val) {
    // CPU I/O port: $00 (DDR), $01 (port)
    if (addr == 0x0000) {
        m_portDDR = val;
        updateOverlays();
        return true;
    }
    if (addr == 0x0001) {
        m_portOut = val;
        updateOverlays();
        return true;
    }

    // All other writes fall through (ROM writes go to underlying RAM,
    // I/O writes go to device handlers)
    return false;
}
