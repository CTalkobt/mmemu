#include "vic4.h"
#include <cstring>

VIC4::VIC4()
    : VIC3("VIC-IV", 0xD000)
{
    m_colorRamExt.resize(32768, 0); // 32 KB internal color RAM
    reset();
}

void VIC4::reset() {
    VIC3::reset();
    std::memset(m_extRegs, 0, sizeof(m_extRegs));

    // Default VIC-IV extended registers
    m_extRegs[0x0C] = 0x00; // Screen RAM base ($D04C-$D04F)
    m_extRegs[0x0D] = 0x00;
    m_extRegs[0x0E] = 0x00;
    m_extRegs[0x0F] = 0x00;

    m_extRegs[0x10] = 0x00; // Char base ($D050-$D053)
    m_extRegs[0x11] = 0x00;
    m_extRegs[0x12] = 0x00;
    m_extRegs[0x13] = 0x00;

    m_extRegs[0x18] = 0x00; // Color RAM base ($D058-$D05B) default $FF80000
    m_extRegs[0x19] = 0x80;
    m_extRegs[0x1A] = 0xF8;
    m_extRegs[0x1B] = 0x0F;
}

void VIC4::tick(uint64_t cycles) {
    VIC3::tick(cycles);
}

bool VIC4::ioRead(IBus* bus, uint32_t addr, uint8_t* val) {
    if ((addr & ~addrMask()) != baseAddr()) return false;

    uint16_t offset = addr & 0x03FF;

    // $D048-$D07F: VIC-IV specific extended registers
    if (offset >= 0x0048 && offset <= 0x007F) {
        if (isLocked()) { *val = 0xFF; return true; }
        *val = m_extRegs[offset - 0x0040];
        return true;
    }

    // Everything else ($D000-$D047, $D080-$D3FF) handled by VIC3
    return VIC3::ioRead(bus, addr, val);
}

bool VIC4::ioWrite(IBus* bus, uint32_t addr, uint8_t val) {
    if ((addr & ~addrMask()) != baseAddr()) return false;

    uint16_t offset = addr & 0x03FF;

    // $D048-$D07F: VIC-IV specific extended registers
    if (offset >= 0x0048 && offset <= 0x007F) {
        if (isLocked()) return true;
        m_extRegs[offset - 0x0040] = val;
        return true;
    }

    // Everything else handled by VIC3
    return VIC3::ioWrite(bus, addr, val);
}

uint32_t VIC4::getScreenBase() const {
    if (isLocked()) return VIC2::screenBase();
    return (m_extRegs[0x0F] << 24) | (m_extRegs[0x0E] << 16) |
           (m_extRegs[0x0D] << 8)  | m_extRegs[0x0C];
}

uint32_t VIC4::getCharBase() const {
    if (isLocked()) return VIC2::charBitmapBase();
    return (m_extRegs[0x13] << 24) | (m_extRegs[0x12] << 16) |
           (m_extRegs[0x11] << 8)  | m_extRegs[0x10];
}

void VIC4::renderFrame(uint32_t* buffer) {
    if (isLocked()) {
        VIC2::renderFrame(buffer);
        return;
    }

    // MEGA65 unlocked mode: use VIC-III rendering as baseline
    // VIC-IV specific modes (FCM, NCM, etc.) will override this later
    VIC3::renderFrame(buffer);
}
