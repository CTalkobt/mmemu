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

    // C65 ROM banking via $D030 (VIC-III) takes priority over C64 $01 port.
    // MAP block mapping overrides both — if block is MAP'd, ROM is not visible.
    uint8_t d030val = d030();

    // BASIC ROM: $A000-$BFFF
    //   C65: $D030 bit 4 (ROMA) set
    //   C64: HIRAM=1 && LORAM=1
    if (addr >= 0xA000 && addr <= 0xBFFF && m_basicRom && !isBlockMapped(5)) {
        if ((d030val & 0x10) || (hiram() && loram())) {
            uint32_t off = addr - 0xA000;
            if (off < m_basicSize) { *val = m_basicRom[off]; return true; }
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

    // KERNAL ROM: $E000-$FFFF
    //   C65: $D030 bit 7 (ROME) set
    //   C64: HIRAM=1
    if (addr >= 0xE000 && addr <= 0xFFFF && m_kernalRom && !isBlockMapped(7)) {
        if ((d030val & 0x80) || hiram()) {
            uint32_t off = addr - 0xE000;
            if (off < m_kernalSize) { *val = m_kernalRom[off]; return true; }
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
