// The Tube and the 6502 Second Processor's memory, after b-em (tube.c,
// 6502tube.c; GPL-2.0-or-later)

#include "bbc_tube.h"
#include "libdebug/main/execution_observer.h"
#include <algorithm>

namespace bbc {

// ---------------------------------------------------------------------------
// TubeUla
// ---------------------------------------------------------------------------
void TubeUla::resetMost() {
    m_ph1count = m_ph1head = m_ph1tail = 0;
    m_ph3pos = 1;
    m_hp3pos = 0;
    m_hstat[0] = m_hstat[1] = m_hstat[3] = SPACE;
    m_pstat[0] = SPACE;
    m_pstat[1] = m_pstat[3] = 0x7F;
    m_pstat[2] = 0x3F;
    m_hstat[2] = BOTH;
}

void TubeUla::reset() {
    resetMost();
    m_r1stat = 0;
    update();
}

void TubeUla::update() {
    // tube_updateints
    m_hostIrq = (m_r1stat & STAT_Q) && (m_hstat[3] & DATA);
    int irq = 0;
    if (((m_r1stat & STAT_I) && (m_pstat[0] & DATA)) || ((m_r1stat & STAT_J) && (m_pstat[3] & DATA))) irq |= 1;
    if ((m_r1stat & STAT_M) && (m_pstat[2] & DATA)) irq |= 2;
    m_parIrq = irq;
}

uint8_t TubeUla::hostRead(uint16_t addr) {
    uint8_t v = 0;
    switch (addr & 7) {
        case 0: v = (uint8_t)((m_hstat[0] & BOTH) | m_r1stat); break;     // register 1 status
        case 1:                                                            // register 1
            if (m_ph1count > 0) {
                v = m_ph1[m_ph1head];
                if (--m_ph1count == 0) m_hstat[0] &= (uint8_t)~DATA;
                if (++m_ph1head == PH1_SIZE) m_ph1head = 0;
                m_pstat[0] |= SPACE;
            } else {
                v = m_phl;
            }
            break;
        case 2: v = m_hstat[1]; break;
        case 3:
            v = m_ph2;
            m_hstat[1] &= (uint8_t)~DATA;
            m_pstat[1] |= SPACE;
            break;
        case 4: v = m_hstat[2]; break;
        case 5:
            if (m_ph3pos > 0) {
                v = m_ph3[0];
                m_ph3[0] = m_ph3[1];
                if (--m_ph3pos == 0) {
                    m_hstat[2] &= (uint8_t)~DATA;
                    m_pstat[2] |= BOTH;
                }
            } else {
                v = m_phl;
            }
            break;
        case 6: v = m_hstat[3]; break;
        case 7:
            v = m_ph4;
            m_hstat[3] &= (uint8_t)~DATA;
            m_pstat[3] |= SPACE;
            break;
    }
    update();
    return v;
}

void TubeUla::hostWrite(uint16_t addr, uint8_t v) {
    m_hpl = v;
    switch (addr & 7) {
        case 0:                                                            // control
            if (v & STAT_S) {
                if ((v & STAT_T) && !(m_r1stat & STAT_T)) resetMost();
                m_r1stat |= v & 0x3F;
            } else {
                if (!(v & STAT_P) && (m_r1stat & STAT_P) && onParasiteReset) onParasiteReset();
                m_r1stat &= (uint8_t)~(v & 0x3F);
            }
            m_hstat[0] = (uint8_t)((m_hstat[0] & BOTH) | (v & 0x3F));
            break;
        case 1:
            m_hp1 = v;
            m_pstat[0] |= DATA;
            m_hstat[0] &= (uint8_t)~SPACE;
            break;
        case 3:
            m_hp2 = v;
            m_pstat[1] |= DATA;
            m_hstat[1] &= (uint8_t)~SPACE;
            break;
        case 5:
            if (m_hp3pos < 2) {
                m_hp3[m_hp3pos++] = v;
                if (m_hp3pos >= ((m_r1stat & STAT_V) ? 2 : 1)) {
                    m_pstat[2] |= DATA;
                    m_hstat[2] &= (uint8_t)~SPACE;
                }
            }
            break;
        case 7:
            m_hp4 = v;
            m_pstat[3] |= DATA;
            m_hstat[3] &= (uint8_t)~SPACE;
            break;
    }
    update();
}

uint8_t TubeUla::parasiteRead(uint16_t addr) {
    uint8_t v = 0;
    switch (addr & 7) {
        case 0:
            if (onParasiteStatusRead) onParasiteStatusRead();
            v = (uint8_t)(m_pstat[0] | m_r1stat);
            break;
        case 1:
            v = m_hp1;
            m_pstat[0] &= (uint8_t)~DATA;
            m_hstat[0] |= SPACE;
            break;
        case 2: v = m_pstat[1]; break;
        case 3:
            v = m_hp2;
            m_pstat[1] &= (uint8_t)~DATA;
            m_hstat[1] |= SPACE;
            break;
        case 4: v = m_pstat[2]; break;
        case 5:
            if (m_hp3pos > 0) {
                v = m_hp3[0];
                m_hp3[0] = m_hp3[1];
                if (--m_hp3pos == 0) {
                    m_hstat[2] |= SPACE;
                    m_pstat[2] &= (uint8_t)~DATA;
                }
            } else {
                v = m_hpl;
            }
            break;
        case 6: v = m_pstat[3]; break;
        case 7:
            v = m_hp4;
            m_pstat[3] &= (uint8_t)~DATA;
            m_hstat[3] |= SPACE;
            break;
    }
    update();
    return v;
}

void TubeUla::parasiteWrite(uint16_t addr, uint8_t v) {
    m_phl = v;
    switch (addr & 7) {
        case 1:
            if (m_ph1count < PH1_SIZE) {
                m_ph1[m_ph1tail++] = v;
                m_hstat[0] |= DATA;
                if (m_ph1tail == PH1_SIZE) m_ph1tail = 0;
                if (++m_ph1count == PH1_SIZE) m_pstat[0] &= (uint8_t)~SPACE;
            }
            break;
        case 3:
            m_ph2 = v;
            m_hstat[1] |= DATA;
            m_pstat[1] &= (uint8_t)~SPACE;
            break;
        case 5:
            if (m_ph3pos < 2) {
                m_ph3[m_ph3pos++] = v;
                if (m_ph3pos >= ((m_r1stat & STAT_V) ? 2 : 1)) {
                    m_hstat[2] |= DATA;
                    m_pstat[2] &= (uint8_t)~BOTH;
                }
            }
            break;
        case 7:
            m_ph4 = v;
            m_hstat[3] |= DATA;
            m_pstat[3] &= (uint8_t)~SPACE;
            break;
    }
    update();
}

// ---------------------------------------------------------------------------
// TubeParasiteBus
// ---------------------------------------------------------------------------
TubeParasiteBus::TubeParasiteBus(TubeUla* tube, std::vector<uint8_t> rom)
    : m_tube(tube), m_rom(std::move(rom)) {
    m_cfg = {16, 8, BusRole::DATA, true, 0xFFFF};
    m_ram.assign(0x10000, 0);
    if (m_rom.empty()) m_rom.assign(0x800, 0);
    tube->onParasiteStatusRead = [this]() { m_romIn = false; };
}

void TubeParasiteBus::powerOn() {
    std::fill(m_ram.begin(), m_ram.end(), 0);
    m_romIn = true;
}

uint8_t TubeParasiteBus::peek8(uint32_t addr) {
    const uint16_t a = addr & 0xFFFF;
    if ((a & ~7) == 0xFEF8) return 0xFF;
    if ((a & 0xF000) == 0xF000 && m_romIn) return m_rom[a & 0x7FF];
    return m_ram[a];
}

uint8_t TubeParasiteBus::read8(uint32_t addr) {
    const uint16_t a = addr & 0xFFFF;
    uint8_t v;
    if ((a & ~7) == 0xFEF8) v = m_tube->parasiteRead(a);
    else if ((a & 0xF000) == 0xF000 && m_romIn) v = m_rom[a & 0x7FF];
    else v = m_ram[a];
    if (m_observer) m_observer->onMemoryRead(this, a, v);
    return v;
}

void TubeParasiteBus::write8(uint32_t addr, uint8_t v) {
    const uint16_t a = addr & 0xFFFF;
    if (m_observer) m_observer->onMemoryWrite(this, a, peek8(a), v);
    if ((a & ~7) == 0xFEF8) m_tube->parasiteWrite(a, v);
    else m_ram[a] = v;
}

// ---------------------------------------------------------------------------
// Z80ParasiteBus (b-em z80.c z80_do_readmem)
// ---------------------------------------------------------------------------
Z80ParasiteBus::Z80ParasiteBus(std::vector<uint8_t> rom) : m_rom(std::move(rom)) {
    m_cfg = {16, 8, BusRole::DATA, true, 0xFFFF};
    m_ram.assign(0x10000, 0);
    if (m_rom.empty()) m_rom.assign(0x1000, 0);
}

void Z80ParasiteBus::powerOn() {
    std::fill(m_ram.begin(), m_ram.end(), 0);
    m_romIn = true;
}

uint8_t Z80ParasiteBus::peek8(uint32_t addr) {
    const uint16_t a = addr & 0xFFFF;
    if (a < 0x1000 && m_romIn) return m_rom[a & 0xFFF];
    return m_ram[a];
}

uint8_t Z80ParasiteBus::read8(uint32_t addr) {
    const uint8_t v = peek8(addr);
    if (m_observer) m_observer->onMemoryRead(this, addr & 0xFFFF, v);
    return v;
}

void Z80ParasiteBus::write8(uint32_t addr, uint8_t v) {
    const uint16_t a = addr & 0xFFFF;
    if (m_observer) m_observer->onMemoryWrite(this, a, peek8(a), v);
    m_ram[a] = v;                                                // writes go to RAM under the ROM
}

// ---------------------------------------------------------------------------
// M6809ParasiteBus (b-em 6809tube.c readmem / writemem)
// ---------------------------------------------------------------------------
M6809ParasiteBus::M6809ParasiteBus(TubeUla* tube, std::vector<uint8_t> rom) : m_tube(tube), m_rom(std::move(rom)) {
    m_cfg = {16, 8, BusRole::DATA, true, 0xFFFF};
    m_ram.assign(0x10000, 0);
    if (m_rom.empty()) m_rom.assign(0x800, 0);
}

void M6809ParasiteBus::powerOn() {
    std::fill(m_ram.begin(), m_ram.end(), 0);
    m_romIn = true;
}

uint8_t M6809ParasiteBus::peek8(uint32_t addr) {
    const uint16_t a = addr & 0xFFFF;
    if ((a & ~7) == 0xFEE0) return 0xFF;
    if (a >= 0xF800 && m_romIn) return m_rom[a & 0x7FF];
    return m_ram[a];
}

uint8_t M6809ParasiteBus::read8(uint32_t addr) {
    const uint16_t a = addr & 0xFFFF;
    uint8_t v;
    if ((a & ~7) == 0xFEE0) { v = m_tube->parasiteRead(a); m_romIn = false; }
    else if (a >= 0xF800 && m_romIn) v = m_rom[a & 0x7FF];
    else v = m_ram[a];
    if (m_observer) m_observer->onMemoryRead(this, a, v);
    return v;
}

void M6809ParasiteBus::write8(uint32_t addr, uint8_t v) {
    const uint16_t a = addr & 0xFFFF;
    if (m_observer) m_observer->onMemoryWrite(this, a, peek8(a), v);
    if ((a & ~7) == 0xFEE0) { m_romIn = false; m_tube->parasiteWrite(a, v); }
    else m_ram[a] = v;
}

// ---------------------------------------------------------------------------
// ArmParasiteBus (b-em arm.c readarmb / readarml / writearmb / writearml)
// ---------------------------------------------------------------------------
ArmParasiteBus::ArmParasiteBus(TubeUla* tube, std::vector<uint8_t> rom) : m_tube(tube), m_rom(std::move(rom)) {
    m_cfg = {26, 8, BusRole::DATA, true, 0x03FFFFFF};
    m_ram.assign(RAM_SIZE, 0);
    if (m_rom.empty()) m_rom.assign(0x4000, 0);
}

void ArmParasiteBus::powerOn() {
    std::fill(m_ram.begin(), m_ram.end(), 0);
    reset();
}

uint8_t ArmParasiteBus::peek8(uint32_t addr) {
    const uint32_t a = addr & 0x03FFFFFF;
    if (a < RAM_SIZE) return m_ram[a];
    if (a >= ROM_BASE && a < ROM_BASE + m_rom.size()) return m_rom[a - ROM_BASE];
    return 0xFF;
}

uint8_t ArmParasiteBus::read8(uint32_t addr) {
    const uint32_t a = addr & 0x03FFFFFF;
    uint8_t v;
    if ((a & ~0x1Fu) == 0x1000000) v = m_tube->parasiteRead((uint16_t)((a & 0x1C) >> 2));
    else v = peek8(a);
    if (m_observer) m_observer->onMemoryRead(this, a, v);
    return v;
}

void ArmParasiteBus::write8(uint32_t addr, uint8_t v) {
    const uint32_t a = addr & 0x03FFFFFF;
    if (m_observer) m_observer->onMemoryWrite(this, a, peek8(a), v);
    if (a < RAM_SIZE) m_ram[a] = v;
    else if ((a & ~0x1Fu) == 0x1000000) m_tube->parasiteWrite((uint16_t)((a & 0x1C) >> 2), v);
}

uint32_t ArmParasiteBus::read32(uint32_t addr) {
    const uint32_t a = addr & 0x03FFFFFC;
    if (a < RAM_SIZE || (a >= ROM_BASE && a < ROM_BASE + m_rom.size()))
        return (uint32_t)peek8(a) | (uint32_t)peek8(a + 1) << 8 | (uint32_t)peek8(a + 2) << 16 | (uint32_t)peek8(a + 3) << 24;
    return 0xFFFFFFFF;                                                  // the Tube and nothing: no side effects
}

void ArmParasiteBus::write32(uint32_t addr, uint32_t v) {
    const uint32_t a = addr & 0x03FFFFFC;
    if (a >= RAM_SIZE) return;
    for (int i = 0; i < 4; ++i) m_ram[a + (uint32_t)i] = (uint8_t)(v >> (8 * i));
}

}  // namespace bbc
