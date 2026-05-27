#include "cpu45gs02.h"
#include "libdebug/main/execution_observer.h"
#include "plugins/devices/map_mmu/main/map_mmu.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>

static const RegDescriptor s_regDescriptors[] = {
    { "A",  RegWidth::R8,  REGFLAG_ACCUMULATOR, nullptr, nullptr    },
    { "X",  RegWidth::R8,  0,                   nullptr, nullptr    },
    { "Y",  RegWidth::R8,  0,                   nullptr, nullptr    },
    { "Z",  RegWidth::R8,  0,                   nullptr, nullptr    },
    { "B",  RegWidth::R8,  0,                   nullptr, nullptr    },
    { "SP", RegWidth::R16, REGFLAG_SP,          nullptr, nullptr    },
    { "PC", RegWidth::R16, REGFLAG_PC,          nullptr, nullptr    },
    { "P",  RegWidth::R8,  REGFLAG_STATUS,      "F",     "NV-BDIZC" },
    { "Q",  RegWidth::R32, REGFLAG_INTERNAL,    nullptr, nullptr    }
};

MOS45GS02::MOS45GS02() : m_bus(nullptr) {
    memset(&m_state, 0, sizeof(m_state));
    memset(&m_hyperState, 0, sizeof(m_hyperState));
    MOS45GS02::reset();
}

void MOS45GS02::reset() {
    m_state.a = 0; m_state.x = 0; m_state.y = 0; m_state.z = 0; m_state.b = 0;
    m_state.sp = 0x01FF; m_state.p = FLAG_I; m_state.cycles = 0;
    m_state.irqLine = 0; m_state.nmiLine = 0; m_state.nmiPrev = 0; m_state.haltLine = 0;
    memset(&m_hyperState, 0, sizeof(m_hyperState));

    if (m_hyperRom) {
        // MEGA65: enter hypervisor mode at RESET entry point $8100
        enterHypervisor(0x8100);
    } else if (m_bus) {
        // Fallback: standard 6502 reset vector
        uint16_t lo = read8(0xFFFC); uint16_t hi = read8(0xFFFD);
        m_state.pc = lo | (hi << 8);
    }
}

void MOS45GS02::setHypervisorRom(const uint8_t* data, uint32_t size) {
    m_hyperRom = data;
    m_hyperRomSize = size;
}

void MOS45GS02::enterHypervisor(uint16_t trapAddr) {
    // Save current CPU state into virtualisation control registers
    m_hyperState.regA   = m_state.a;
    m_hyperState.regX   = m_state.x;
    m_hyperState.regY   = 0; // Y not explicitly listed in save, but preserved
    m_hyperState.regZ   = m_state.z;
    m_hyperState.regB   = m_state.b;
    m_hyperState.spl    = m_state.sp & 0xFF;
    m_hyperState.sph    = (m_state.sp >> 8) & 0xFF;
    m_hyperState.pflags = m_state.p;
    m_hyperState.pc     = m_state.pc;

    // Save MAP state
    if (m_mapMmu) {
        MapState ms = m_mapMmu->getMapState();
        m_hyperState.mapLo0  = (ms.offsets[0] >> 0) & 0xFF;
        m_hyperState.mapLo1  = (ms.offsets[0] >> 8) & 0xFF;
        m_hyperState.mapHi0  = (ms.offsets[4] >> 0) & 0xFF;
        m_hyperState.mapHi1  = (ms.offsets[4] >> 8) & 0xFF;
        m_hyperState.mapLoMB = ms.enables & 0x0F;
        m_hyperState.mapHiMB = (ms.enables >> 4) & 0x0F;
    }

    // Enter hypervisor mode
    m_state.hypervisor = true;
    m_state.p |= FLAG_I; // Interrupts disabled
    m_state.pc = trapAddr;

    // In hypervisor mode: stack at $BE00, zero page at $BF00
    m_state.sp = 0xBEFF;
    m_state.b  = 0xBF;  // Base page = $BF00

    // Clear MAP state (hypervisor has flat memory with ROM at $8000)
    if (m_mapMmu) {
        MapState clear;
        memset(&clear, 0, sizeof(clear));
        m_mapMmu->setMapState(clear);
    }
}

void MOS45GS02::exitHypervisor() {
    // Restore CPU state from virtualisation control registers
    m_state.a  = m_hyperState.regA;
    m_state.x  = m_hyperState.regX;
    m_state.z  = m_hyperState.regZ;
    m_state.b  = m_hyperState.regB;
    m_state.sp = m_hyperState.spl | ((uint16_t)m_hyperState.sph << 8);
    m_state.p  = m_hyperState.pflags;
    m_state.pc = m_hyperState.pc;

    // Restore MAP state
    if (m_mapMmu) {
        MapState ms;
        memset(&ms, 0, sizeof(ms));
        ms.offsets[0] = m_hyperState.mapLo0 | ((uint16_t)m_hyperState.mapLo1 << 8);
        ms.offsets[4] = m_hyperState.mapHi0 | ((uint16_t)m_hyperState.mapHi1 << 8);
        ms.enables    = m_hyperState.mapLoMB | (m_hyperState.mapHiMB << 4);
        m_mapMmu->setMapState(ms);
    }

    // Exit hypervisor mode
    m_state.hypervisor = false;
}

const RegDescriptor* MOS45GS02::regDescriptor(int idx) const { return (idx >= 0 && idx < 9) ? &s_regDescriptors[idx] : nullptr; }

uint32_t MOS45GS02::regRead(int idx) const {
    switch (idx) {
        case 0: return m_state.a; case 1: return m_state.x; case 2: return m_state.y; case 3: return m_state.z;
        case 4: return m_state.b; case 5: return m_state.sp; case 6: return m_state.pc; case 7: return m_state.p;
        case 8: return (uint32_t)m_state.a | ((uint32_t)m_state.x << 8) | ((uint32_t)m_state.y << 16) | ((uint32_t)m_state.z << 24);
        default: return 0;
    }
}

void MOS45GS02::regWrite(int idx, uint32_t val) {
    switch (idx) {
        case 0: m_state.a = (uint8_t)val; break; case 1: m_state.x = (uint8_t)val; break;
        case 2: m_state.y = (uint8_t)val; break; case 3: m_state.z = (uint8_t)val; break;
        case 4: m_state.b = (uint8_t)val; break; case 5: m_state.sp = (uint16_t)val; break;
        case 6: m_state.pc = (uint16_t)val; break; case 7: m_state.p = (uint8_t)val; break;
        case 8: 
            m_state.a = (uint8_t)(val & 0xFF);
            m_state.x = (uint8_t)((val >> 8) & 0xFF);
            m_state.y = (uint8_t)((val >> 16) & 0xFF);
            m_state.z = (uint8_t)((val >> 24) & 0xFF);
            break;
    }
}

uint32_t MOS45GS02::translate(uint16_t addr) {
    // If MapMmu is in use, it handles all translation via setMapState()
    // The CPU's internal translate() is bypassed for MapMmu integration
    if (m_mapMmu) return addr;
    return addr;  // No internal translation without MapMmu
}

uint32_t MOS45GS02::read32(uint16_t addr) {
    return (uint32_t)read8(addr) | ((uint32_t)read8(addr + 1) << 8) |
           ((uint32_t)read8(addr + 2) << 16) | ((uint32_t)read8(addr + 3) << 24);
}

void MOS45GS02::write32(uint16_t addr, uint32_t val) {
    write8(addr, (uint8_t)(val & 0xFF));
    write8(addr + 1, (uint8_t)((val >> 8) & 0xFF));
    write8(addr + 2, (uint8_t)((val >> 16) & 0xFF));
    write8(addr + 3, (uint8_t)((val >> 24) & 0xFF));
}

uint32_t MOS45GS02::read32_phys(uint32_t physAddr) {
    return (uint32_t)read8_phys(physAddr) | ((uint32_t)read8_phys(physAddr + 1) << 8) |
           ((uint32_t)read8_phys(physAddr + 2) << 16) | ((uint32_t)read8_phys(physAddr + 3) << 24);
}

void MOS45GS02::write32_phys(uint32_t physAddr, uint32_t val) {
    write8_phys(physAddr, (uint8_t)(val & 0xFF));
    write8_phys(physAddr + 1, (uint8_t)((val >> 8) & 0xFF));
    write8_phys(physAddr + 2, (uint8_t)((val >> 16) & 0xFF));
    write8_phys(physAddr + 3, (uint8_t)((val >> 24) & 0xFF));
}

uint8_t MOS45GS02::read8(uint16_t addr) {
    // Hypervisor mode: $8000-$BFFF reads from hypervisor ROM
    if (m_state.hypervisor && m_hyperRom && addr >= 0x8000 && addr <= 0xBFFF) {
        uint16_t off = addr - 0x8000;
        return (off < m_hyperRomSize) ? m_hyperRom[off] : 0xFF;
    }
    return m_bus ? m_bus->read8(translate(addr)) : 0xFF;
}
void MOS45GS02::write8(uint16_t addr, uint8_t val) {
    // Hypervisor mode: $8000-$BFFF is ROM (writes ignored)
    if (m_state.hypervisor && addr >= 0x8000 && addr <= 0xBFFF) return;
    if (m_bus) m_bus->write8(translate(addr), val);
}
uint8_t MOS45GS02::read8_phys(uint32_t physAddr) { return m_bus ? m_bus->read8(physAddr & 0x0FFFFFFF) : 0xFF; }
void MOS45GS02::write8_phys(uint32_t physAddr, uint8_t val) { if (m_bus) m_bus->write8(physAddr & 0x0FFFFFFF, val); }
void MOS45GS02::updateNZ(uint8_t val) { m_state.p &= ~(FLAG_N | FLAG_Z); if (!val) m_state.p |= FLAG_Z; if (val & 0x80) m_state.p |= FLAG_N; }
void MOS45GS02::updateNZ16(uint16_t val) { m_state.p &= ~(FLAG_N | FLAG_Z); if (!val) m_state.p |= FLAG_Z; if (val & 0x8000) m_state.p |= FLAG_N; }
void MOS45GS02::updateNZ32(uint32_t val) { m_state.p &= ~(FLAG_N | FLAG_Z); if (!val) m_state.p |= FLAG_Z; if (val & 0x80000000) m_state.p |= FLAG_N; }

void MOS45GS02::doAdc8(uint8_t v) {
    uint8_t a = m_state.a;
    if (m_state.p & FLAG_D) {
        // BCD mode (65CE02-style: N/Z set from binary result)
        int lo = (a & 0x0F) + (v & 0x0F) + (m_state.p & FLAG_C);
        if (lo > 9) lo += 6;
        int hi = (a >> 4) + (v >> 4) + (lo > 0x0F ? 1 : 0);
        if (hi > 9) hi += 6;
        uint16_t bin = (uint16_t)a + v + (m_state.p & FLAG_C);
        m_state.p &= ~(FLAG_V | FLAG_C);
        if (~(a ^ v) & (a ^ (uint8_t)bin) & 0x80) m_state.p |= FLAG_V;
        if (hi > 0x0F) m_state.p |= FLAG_C;
        m_state.a = (uint8_t)(((hi & 0x0F) << 4) | (lo & 0x0F));
        updateNZ(m_state.a);
    } else {
        uint16_t s = (uint16_t)a + v + (m_state.p & FLAG_C);
        m_state.p &= ~(FLAG_V | FLAG_C);
        if (~(a ^ v) & (a ^ s) & 0x80) m_state.p |= FLAG_V;
        if (s > 0xFF) m_state.p |= FLAG_C;
        m_state.a = (uint8_t)s;
        updateNZ(m_state.a);
    }
}

void MOS45GS02::doSbc8(uint8_t v) {
    uint8_t a = m_state.a;
    if (m_state.p & FLAG_D) {
        int borrow = (m_state.p & FLAG_C) ? 0 : 1;
        int lo = (a & 0x0F) - (v & 0x0F) - borrow;
        if (lo < 0) lo -= 6;
        int hi = (a >> 4) - (v >> 4) - (lo < 0 ? 1 : 0);
        if (hi < 0) hi -= 6;
        uint16_t bin = (uint16_t)a - v - borrow;
        m_state.p &= ~(FLAG_V | FLAG_C);
        if ((a ^ v) & (a ^ (uint8_t)bin) & 0x80) m_state.p |= FLAG_V;
        if (bin <= 0xFF) m_state.p |= FLAG_C;
        m_state.a = (uint8_t)(((hi & 0x0F) << 4) | (lo & 0x0F));
        updateNZ(m_state.a);
    } else {
        uint16_t d = (uint16_t)a - v - ((m_state.p & FLAG_C) ? 0 : 1);
        m_state.p &= ~(FLAG_V | FLAG_C);
        if ((a ^ v) & (a ^ d) & 0x80) m_state.p |= FLAG_V;
        if (d <= 0xFF) m_state.p |= FLAG_C;
        m_state.a = (uint8_t)d;
        updateNZ(m_state.a);
    }
}

void MOS45GS02::push8(uint8_t v) {
    write8(m_state.sp, v);
    if (m_state.p & FLAG_E) {
        m_state.sp = ((uint16_t)m_state.b << 8) | ((m_state.sp - 1) & 0xFF);
    } else {
        m_state.sp--;
    }
}
uint8_t MOS45GS02::pull8() {
    if (m_state.p & FLAG_E) {
        m_state.sp = ((uint16_t)m_state.b << 8) | ((m_state.sp + 1) & 0xFF);
    } else {
        m_state.sp++;
    }
    return read8(m_state.sp);
}
void MOS45GS02::push16(uint16_t v) { push8(v >> 8); push8(v & 0xFF); }
uint16_t MOS45GS02::pull16() { uint8_t l = pull8(); uint8_t h = pull8(); return l | (h << 8); }

int MOS45GS02::step() {
    if (m_state.haltLine) return 1;
    if (m_bus && m_bus->isHaltRequested()) { m_state.haltLine = 1; return 1; }

    // NMI handling (edge-sensitive: triggers on 0→1 transition)
    if (m_state.nmiLine && !m_state.nmiPrev) {
        m_state.nmiPrev = 1;
        push8((uint8_t)(m_state.pc >> 8));
        push8((uint8_t)(m_state.pc & 0xFF));
        push8((m_state.p & ~FLAG_B) | FLAG_E);
        m_state.p |= FLAG_I;
        uint8_t lo = read8(0xFFFA);
        uint8_t hi = read8(0xFFFB);
        m_state.pc = (uint16_t)lo | ((uint16_t)hi << 8);
        m_state.cycles += 7;
        return 7;
    }
    m_state.nmiPrev = m_state.nmiLine;

    // IRQ handling (level-sensitive: triggers while line asserted and I flag clear)
    if (m_state.irqLine && !(m_state.p & FLAG_I)) {
        push8((uint8_t)(m_state.pc >> 8));
        push8((uint8_t)(m_state.pc & 0xFF));
        push8((m_state.p & ~FLAG_B) | FLAG_E);
        m_state.p |= FLAG_I;
        uint8_t lo = read8(0xFFFE);
        uint8_t hi = read8(0xFFFF);
        m_state.pc = (uint16_t)lo | ((uint16_t)hi << 8);
        m_state.cycles += 7;
        return 7;
    }

    uint16_t startPc = m_state.pc;
    bool isQuad = false;
    bool is32BitInd = false;

    // Reset PC to startPc if observer fails
    if (m_observer) {
        if (m_observer->needsDisasm()) {
            DisasmEntry entry; disassembleEntry(m_bus, startPc, &entry);
            if (!m_observer->onStep(this, m_bus, entry)) {
                return 1;
            }
        } else {
            m_observer->onStepLite(this, startPc);
        }
    }

    uint8_t op;
    while (true) {
        op = read8(m_state.pc++);
        m_state.cycles++;
        if (op == 0x42) {
            if (read8(m_state.pc) == 0x42) {
                m_state.pc++;
                m_state.cycles++;
                isQuad = true;
                continue;
            }
            break;
        } else if (op == 0xEA) {
            uint8_t next = read8(m_state.pc);
            if (next == 0x12 || next == 0x32 || next == 0x52 || next == 0x72 ||
                next == 0x92 || next == 0xB2 || next == 0xD2 || next == 0xF2) {
                is32BitInd = true;
                continue;
            }
            break;
        }
        break;
    }

    auto getAbs = [&]() { uint16_t l=read8(m_state.pc++); uint16_t h=read8(m_state.pc++); m_state.cycles+=2; return (uint16_t)((h<<8)|l); };
    auto getZp  = [&]() { uint8_t z=read8(m_state.pc++); m_state.cycles++; return (uint16_t)((m_state.b << 8) | z); };
    auto getAbsX = [&]() { return getAbs() + m_state.x; };
    auto getAbsY = [&]() { return getAbs() + m_state.y; };
    auto getZpX  = [&]() { uint8_t z=read8(m_state.pc++); m_state.cycles++; return (uint16_t)((m_state.b << 8) | (uint8_t)(z + m_state.x)); };
    auto getZpY  = [&]() { uint8_t z=read8(m_state.pc++); m_state.cycles++; return (uint16_t)((m_state.b << 8) | (uint8_t)(z + m_state.y)); };

    auto getIndX = [&]() {
        uint8_t z = read8(m_state.pc++);
        uint16_t ptrAddr = (uint16_t)((m_state.b << 8) | (uint8_t)(z + m_state.x));
        uint16_t ptr = (uint16_t)read8(ptrAddr) | ((uint16_t)read8(ptrAddr + 1) << 8);
        m_state.cycles += 4;
        return ptr;
    };

    auto getIndY = [&]() {
        uint8_t z = read8(m_state.pc++);
        uint16_t ptrAddr = (uint16_t)((m_state.b << 8) | z);
        uint16_t ptr = (uint16_t)read8(ptrAddr) | ((uint16_t)read8(ptrAddr + 1) << 8);
        m_state.cycles += 3;
        return ptr + m_state.y;
    };

    auto getStackRelIndY = [&]() {
        uint8_t offset = read8(m_state.pc++);
        uint16_t ptrAddr = m_state.sp + offset;
        uint16_t ptr = (uint16_t)read8(ptrAddr) | ((uint16_t)read8(ptrAddr + 1) << 8);
        m_state.cycles += 4;
        return ptr + m_state.y;
    };

    auto getIndPhys = [&]() {
        uint16_t zpAddr = getZp();
        uint32_t ptr;
        if (is32BitInd) {
            ptr = (uint32_t)read8_phys(translate(zpAddr)) |
                  ((uint32_t)read8_phys(translate(zpAddr + 1)) << 8) |
                  ((uint32_t)read8_phys(translate(zpAddr + 2)) << 16) |
                  ((uint32_t)read8_phys(translate(zpAddr + 3)) << 24);
            ptr &= 0x0FFFFFFF; // 28-bit
            m_state.cycles += 4;
        } else {
            ptr = (uint32_t)read8(zpAddr) | ((uint32_t)read8(zpAddr + 1) << 8);
            m_state.cycles += 2;
        }
        return ptr;
    };

    auto doAdc32 = [&](uint32_t v) {
        uint32_t q = regRead(8);
        uint64_t res = (uint64_t)q + v + (m_state.p & FLAG_C);
        m_state.p &= ~(FLAG_V | FLAG_C);
        if (~(q ^ v) & (q ^ (uint32_t)res) & 0x80000000) m_state.p |= FLAG_V;
        if (res > 0xFFFFFFFF) m_state.p |= FLAG_C;
        regWrite(8, (uint32_t)res);
        updateNZ32((uint32_t)res);
        m_state.cycles += 4;
    };

    auto doSbc32 = [&](uint32_t v) {
        uint32_t q = regRead(8);
        uint64_t d = (uint64_t)q - v - ((m_state.p & FLAG_C) ? 0 : 1);
        m_state.p &= ~(FLAG_V | FLAG_C);
        if ((q ^ v) & (q ^ (uint32_t)d) & 0x80000000) m_state.p |= FLAG_V;
        if (d <= 0xFFFFFFFF) m_state.p |= FLAG_C;
        regWrite(8, (uint32_t)d);
        updateNZ32((uint32_t)d);
        m_state.cycles += 4;
    };

    auto doCmp32 = [&](uint32_t v) {
        uint32_t q = regRead(8);
        uint64_t d = (uint64_t)q - v;
        m_state.p &= ~FLAG_C;
        if (q >= v) m_state.p |= FLAG_C;
        updateNZ32((uint32_t)d);
        m_state.cycles += 4;
    };

    auto doAnd32 = [&](uint32_t v) { uint32_t q = regRead(8) & v; regWrite(8, q); updateNZ32(q); m_state.cycles += 4; };
    auto doOra32 = [&](uint32_t v) { uint32_t q = regRead(8) | v; regWrite(8, q); updateNZ32(q); m_state.cycles += 4; };
    auto doEor32 = [&](uint32_t v) { uint32_t q = regRead(8) ^ v; regWrite(8, q); updateNZ32(q); m_state.cycles += 4; };

    switch (op) {
        case 0x00: m_state.haltLine = 1; break; // BRK
        case 0x02: m_state.p &= ~FLAG_E; break; // CLE
        case 0x03: m_state.p |=  FLAG_E; break; // SEE
        case 0x0B: m_state.y = (uint8_t)((m_state.sp >> 8) & 0xFF); updateNZ(m_state.y); break; // TSY
        case 0x2B: m_state.sp = (m_state.sp & 0x00FF) | ((uint16_t)m_state.y << 8); break; // TYS
        case 0xEA: break; // NOP (naked NOP)
        
        // --- 1. Load / Store ---
        case 0xA9: { // LDA #imm / LDQ #imm32
            if (isQuad) {
                uint32_t v = (uint32_t)read8(m_state.pc++) | ((uint32_t)read8(m_state.pc++) << 8) |
                             ((uint32_t)read8(m_state.pc++) << 16) | ((uint32_t)read8(m_state.pc++) << 24);
                regWrite(8, v); updateNZ32(v); m_state.cycles += 4;
            } else { m_state.a = read8(m_state.pc++); updateNZ(m_state.a); m_state.cycles++; }
            break;
        }
        case 0xAD: { // LDA abs / LDQ abs
            uint16_t addr = getAbs();
            if (isQuad) {
                m_state.a = read8(addr); m_state.x = read8(addr + 1); m_state.y = read8(addr + 2); m_state.z = read8(addr + 3);
                updateNZ32(regRead(8)); m_state.cycles += 4; 
            } else {
                m_state.a = read8(addr); updateNZ(m_state.a); m_state.cycles++;
            }
            break;
        }
        case 0xBD: m_state.a = read8(getAbsX()); updateNZ(m_state.a); m_state.cycles++; break;
        case 0xB9: m_state.a = read8(getAbsY()); updateNZ(m_state.a); m_state.cycles++; break;
        case 0xA5: { // LDA zp / LDQ zp
            uint16_t addr = getZp();
            if (isQuad) {
                m_state.a = read8(addr); m_state.x = read8(addr + 1); m_state.y = read8(addr + 2); m_state.z = read8(addr + 3);
                updateNZ32(regRead(8)); m_state.cycles += 4;
            } else {
                m_state.a = read8(addr); updateNZ(m_state.a); m_state.cycles++;
            }
            break;
        }
        case 0xB5: { // LDA zp,X / LDQ zp,X
            uint16_t addr = getZpX();
            if (isQuad) {
                m_state.a = read8(addr); m_state.x = read8(addr + 1); m_state.y = read8(addr + 2); m_state.z = read8(addr + 3);
                updateNZ32(regRead(8)); m_state.cycles += 4;
            } else {
                m_state.a = read8(addr); updateNZ(m_state.a); m_state.cycles++;
            }
            break;
        }
        case 0xE2: m_state.a = read8(getStackRelIndY()); updateNZ(m_state.a); break; // LDA (d,S),Y

        case 0xA1: m_state.a = read8(getIndX()); updateNZ(m_state.a); break; // LDA (zp,X)
        case 0xB1: m_state.a = read8(getIndY()); updateNZ(m_state.a); break; // LDA (zp),Y
        case 0xB2: { // LDA (zp),z / LDQ (zp),z
            uint32_t base = getIndPhys();
            uint32_t addr = base + m_state.z;
            if (isQuad) {
                uint32_t v = is32BitInd ? read32_phys(addr) : read32((uint16_t)addr);
                regWrite(8, v);
                updateNZ32(v); m_state.cycles += 4;
            } else {
                m_state.a = is32BitInd ? read8_phys(addr) : read8((uint16_t)addr);
                updateNZ(m_state.a); m_state.cycles++;
            }
            break;
        }

        // LDX
        case 0xA2: m_state.x = read8(m_state.pc++); updateNZ(m_state.x); m_state.cycles++; break;
        case 0xAE: m_state.x = read8(getAbs()); updateNZ(m_state.x); m_state.cycles++; break;
        case 0xBE: m_state.x = read8(getAbsY()); updateNZ(m_state.x); m_state.cycles++; break;
        case 0xA6: m_state.x = read8(getZp()); updateNZ(m_state.x); m_state.cycles++; break;
        case 0xB6: m_state.x = read8(getZpY()); updateNZ(m_state.x); m_state.cycles++; break;

        // LDY
        case 0xA0: m_state.y = read8(m_state.pc++); updateNZ(m_state.y); m_state.cycles++; break;
        case 0xAC: m_state.y = read8(getAbs()); updateNZ(m_state.y); m_state.cycles++; break;
        case 0xBC: m_state.y = read8(getAbsX()); updateNZ(m_state.y); m_state.cycles++; break;
        case 0xA4: m_state.y = read8(getZp()); updateNZ(m_state.y); m_state.cycles++; break;
        case 0xB4: m_state.y = read8(getZpX()); updateNZ(m_state.y); m_state.cycles++; break;

        // LDZ
        case 0xA3: m_state.z = read8(m_state.pc++); updateNZ(m_state.z); m_state.cycles++; break;
        case 0xAB: m_state.z = read8(getAbs()); updateNZ(m_state.z); m_state.cycles++; break;
        case 0xBB: m_state.z = read8(getAbsX()); updateNZ(m_state.z); m_state.cycles++; break;

        case 0x8D: { // STA abs / STQ abs
            uint16_t addr = getAbs();
            if (isQuad) { write32(addr, regRead(8)); m_state.cycles += 4; }
            else { write8(addr, m_state.a); m_state.cycles++; }
            break;
        }
        case 0x9D: write8(getAbsX(), m_state.a); m_state.cycles++; break;
        case 0x99: write8(getAbsY(), m_state.a); m_state.cycles++; break;
        case 0x85: { // STA zp / STQ zp
            uint16_t addr = getZp();
            if (isQuad) { write32(addr, regRead(8)); m_state.cycles += 4; }
            else { write8(addr, m_state.a); m_state.cycles++; }
            break;
        }
        case 0x95: { // STA zp,X / STQ zp,X
            uint16_t addr = getZpX();
            if (isQuad) { write32(addr, regRead(8)); m_state.cycles += 4; }
            else { write8(addr, m_state.a); m_state.cycles++; }
            break;
        }
        case 0x82: write8(getStackRelIndY(), m_state.a); break; // STA (d,S),Y

        case 0x81: write8(getIndX(), m_state.a); break; // STA (zp,X)
        case 0x91: write8(getIndY(), m_state.a); break; // STA (zp),Y
        case 0x92: { // STA (zp),z / STQ (zp),z
            uint32_t base = getIndPhys();
            uint32_t addr = base + m_state.z;
            if (isQuad) {
                uint32_t q = regRead(8);
                if (is32BitInd) write32_phys(addr, q);
                else write32((uint16_t)addr, q);
                m_state.cycles += 4;
            } else {
                if (is32BitInd) write8_phys(addr, m_state.a);
                else write8((uint16_t)addr, m_state.a);
                m_state.cycles++;
            }
            break;
        }

        // STX, STY, STZ
        case 0x8E: write8(getAbs(), m_state.x); m_state.cycles++; break;
        case 0x86: write8(getZp(), m_state.x); m_state.cycles++; break;
        case 0x96: write8(getZpY(), m_state.x); m_state.cycles++; break;
        case 0x9B: write8(getAbsY(), m_state.x); m_state.cycles++; break; // STX abs,Y
        case 0x8C: write8(getAbs(), m_state.y); m_state.cycles++; break;
        case 0x84: write8(getZp(), m_state.y); m_state.cycles++; break;
        case 0x94: write8(getZpX(), m_state.y); m_state.cycles++; break;
        case 0x9C: write8(getAbs(), m_state.z); m_state.cycles++; break;
        case 0x64: write8(getZp(), m_state.z); m_state.cycles++; break;
        case 0x74: write8(getZpX(), m_state.z); m_state.cycles++; break;
        case 0x9E: write8(getAbsX(), m_state.z); m_state.cycles++; break;

        // --- 2. Arithmetic / Logic ---
        case 0x69: { // ADC #imm / ADCQ #imm32
            if (isQuad) {
                uint32_t v = (uint32_t)read8(m_state.pc++) | ((uint32_t)read8(m_state.pc++) << 8) |
                             ((uint32_t)read8(m_state.pc++) << 16) | ((uint32_t)read8(m_state.pc++) << 24);
                doAdc32(v);
            } else { uint8_t v=read8(m_state.pc++); doAdc8(v); m_state.cycles++; }
            break;
        }
        case 0x6D: { // ADC abs / ADCQ abs
            uint16_t addr = getAbs();
            if (isQuad) doAdc32(read32(addr));
            else { uint8_t v=read8(addr); doAdc8(v); m_state.cycles++; }
            break;
        }
        case 0x7D: { // ADC abs,X / ADCQ abs,X
            uint16_t addr = getAbsX();
            if (isQuad) doAdc32(read32(addr));
            else { uint8_t v=read8(addr); doAdc8(v); m_state.cycles++; }
            break;
        }
        case 0x79: { // ADC abs,Y / ADCQ abs,Y
            uint16_t addr = getAbsY();
            if (isQuad) doAdc32(read32(addr));
            else { uint8_t v=read8(addr); doAdc8(v); m_state.cycles++; }
            break;
        }
        case 0x65: { // ADC zp / ADCQ zp
            uint16_t addr = getZp();
            if (isQuad) doAdc32(read32(addr));
            else { uint8_t v=read8(addr); doAdc8(v); m_state.cycles++; }
            break;
        }
        case 0x75: { // ADC zp,X / ADCQ zp,X
            uint16_t addr = getZpX();
            if (isQuad) doAdc32(read32(addr));
            else { uint8_t v=read8(addr); doAdc8(v); m_state.cycles++; }
            break;
        }
        case 0x61: m_state.a = (uint8_t)(regRead(0)); { uint8_t v=read8(getIndX()); doAdc8(v); break; }
        case 0x71: m_state.a = (uint8_t)(regRead(0)); { uint8_t v=read8(getIndY()); doAdc8(v); break; }
        case 0x72: { // ADC (zp),z / ADCQ (zp),z
            uint32_t base = getIndPhys(); uint32_t addr = base + m_state.z;
            if (isQuad) doAdc32(is32BitInd ? read32_phys(addr) : read32((uint16_t)addr));
            else { uint8_t v = is32BitInd ? read8_phys(addr) : read8((uint16_t)addr); doAdc8(v); m_state.cycles++; }
            break;
        }

        case 0xE9: { // SBC #imm / SBCQ #imm32
            if (isQuad) {
                uint32_t v = (uint32_t)read8(m_state.pc++) | ((uint32_t)read8(m_state.pc++) << 8) |
                             ((uint32_t)read8(m_state.pc++) << 16) | ((uint32_t)read8(m_state.pc++) << 24);
                doSbc32(v);
            } else { uint8_t v=read8(m_state.pc++); doSbc8(v); m_state.cycles++; }
            break;
        }
        case 0xED: { // SBC abs / SBCQ abs
            uint16_t addr = getAbs();
            if (isQuad) doSbc32(read32(addr));
            else { uint8_t v=read8(addr); doSbc8(v); m_state.cycles++; }
            break;
        }
        case 0xFD: { // SBC abs,X / SBCQ abs,X
            uint16_t addr = getAbsX();
            if (isQuad) doSbc32(read32(addr));
            else { uint8_t v=read8(addr); doSbc8(v); m_state.cycles++; }
            break;
        }
        case 0xF9: { // SBC abs,Y / SBCQ abs,Y
            uint16_t addr = getAbsY();
            if (isQuad) doSbc32(read32(addr));
            else { uint8_t v=read8(addr); doSbc8(v); m_state.cycles++; }
            break;
        }
        case 0xE5: { // SBC zp / SBCQ zp
            uint16_t addr = getZp();
            if (isQuad) doSbc32(read32(addr));
            else { uint8_t v=read8(addr); doSbc8(v); m_state.cycles++; }
            break;
        }
        case 0xF5: { // SBC zp,X / SBCQ zp,X
            uint16_t addr = getZpX();
            if (isQuad) doSbc32(read32(addr));
            else { uint8_t v=read8(addr); doSbc8(v); m_state.cycles++; }
            break;
        }
        case 0xE1: m_state.a = (uint8_t)(regRead(0)); { uint8_t v=read8(getIndX()); doSbc8(v); break; }
        case 0xF1: m_state.a = (uint8_t)(regRead(0)); { uint8_t v=read8(getIndY()); doSbc8(v); break; }
        case 0xF2: { // SBC (zp),z / SBCQ (zp),z
            uint32_t base = getIndPhys(); uint32_t addr = base + m_state.z;
            if (isQuad) doSbc32(is32BitInd ? read32_phys(addr) : read32((uint16_t)addr));
            else { uint8_t v = is32BitInd ? read8_phys(addr) : read8((uint16_t)addr); doSbc8(v); m_state.cycles++; }
            break;
        }
        
        case 0xC9: { // CMP #imm / CMPQ #imm32
            if (isQuad) {
                uint32_t v = (uint32_t)read8(m_state.pc++) | ((uint32_t)read8(m_state.pc++) << 8) |
                             ((uint32_t)read8(m_state.pc++) << 16) | ((uint32_t)read8(m_state.pc++) << 24);
                doCmp32(v);
            } else { uint8_t v=read8(m_state.pc++); uint16_t d=(uint16_t)m_state.a-v; m_state.p &= ~FLAG_C; if(m_state.a>=v) m_state.p|=FLAG_C; updateNZ((uint8_t)d); m_state.cycles++; }
            break;
        }
        case 0xCD: { // CMP abs / CMPQ abs
            uint16_t addr = getAbs();
            if (isQuad) doCmp32(read32(addr));
            else { uint8_t v=read8(addr); uint16_t d=(uint16_t)m_state.a-v; m_state.p &= ~FLAG_C; if(m_state.a>=v) m_state.p|=FLAG_C; updateNZ((uint8_t)d); m_state.cycles++; }
            break;
        }
        case 0xDD: { // CMP abs,X / CMPQ abs,X
            uint16_t addr = getAbsX();
            if (isQuad) doCmp32(read32(addr));
            else { uint8_t v=read8(addr); uint16_t d=(uint16_t)m_state.a-v; m_state.p &= ~FLAG_C; if(m_state.a>=v) m_state.p|=FLAG_C; updateNZ((uint8_t)d); m_state.cycles++; }
            break;
        }
        case 0xD9: { // CMP abs,Y / CMPQ abs,Y
            uint16_t addr = getAbsY();
            if (isQuad) doCmp32(read32(addr));
            else { uint8_t v=read8(addr); uint16_t d=(uint16_t)m_state.a-v; m_state.p &= ~FLAG_C; if(m_state.a>=v) m_state.p|=FLAG_C; updateNZ((uint8_t)d); m_state.cycles++; }
            break;
        }
        case 0xC5: { // CMP zp / CMPQ zp
            uint16_t addr = getZp();
            if (isQuad) doCmp32(read32(addr));
            else { uint8_t v=read8(addr); uint16_t d=(uint16_t)m_state.a-v; m_state.p &= ~FLAG_C; if(m_state.a>=v) m_state.p|=FLAG_C; updateNZ((uint8_t)d); m_state.cycles++; }
            break;
        }
        case 0xD5: { // CMP zp,X / CMPQ zp,X
            uint16_t addr = getZpX();
            if (isQuad) doCmp32(read32(addr));
            else { uint8_t v=read8(addr); uint16_t d=(uint16_t)m_state.a-v; m_state.p &= ~FLAG_C; if(m_state.a>=v) m_state.p|=FLAG_C; updateNZ((uint8_t)d); m_state.cycles++; }
            break;
        }
        case 0xC1: { uint8_t v=read8(getIndX()); uint16_t d=(uint16_t)m_state.a-v; m_state.p &= ~FLAG_C; if(m_state.a>=v) m_state.p|=FLAG_C; updateNZ((uint8_t)d); break; }
        case 0xD1: { uint8_t v=read8(getIndY()); uint16_t d=(uint16_t)m_state.a-v; m_state.p &= ~FLAG_C; if(m_state.a>=v) m_state.p|=FLAG_C; updateNZ((uint8_t)d); break; }
        case 0xD2: { // CMP (zp),z / CMPQ (zp),z
            uint32_t base = getIndPhys(); uint32_t addr = base + m_state.z;
            if (isQuad) doCmp32(is32BitInd ? read32_phys(addr) : read32((uint16_t)addr));
            else { uint8_t v = is32BitInd ? read8_phys(addr) : read8((uint16_t)addr); uint16_t d=(uint16_t)m_state.a-v; m_state.p &= ~FLAG_C; if(m_state.a>=v) m_state.p|=FLAG_C; updateNZ((uint8_t)d); m_state.cycles++; }
            break;
        }

        case 0x29: { // AND #imm / ANDQ #imm32
            if (isQuad) {
                uint32_t v = (uint32_t)read8(m_state.pc++) | ((uint32_t)read8(m_state.pc++) << 8) |
                             ((uint32_t)read8(m_state.pc++) << 16) | ((uint32_t)read8(m_state.pc++) << 24);
                doAnd32(v);
            } else { m_state.a &= read8(m_state.pc++); updateNZ(m_state.a); m_state.cycles++; }
            break;
        }
        case 0x2D: { // AND abs / ANDQ abs
            uint16_t addr = getAbs();
            if (isQuad) doAnd32(read32(addr));
            else { m_state.a &= read8(addr); updateNZ(m_state.a); m_state.cycles++; }
            break;
        }
        case 0x3D: { // AND abs,X / ANDQ abs,X
            uint16_t addr = getAbsX();
            if (isQuad) doAnd32(read32(addr));
            else { m_state.a &= read8(addr); updateNZ(m_state.a); m_state.cycles++; }
            break;
        }
        case 0x39: { // AND abs,Y / ANDQ abs,Y
            uint16_t addr = getAbsY();
            if (isQuad) doAnd32(read32(addr));
            else { m_state.a &= read8(addr); updateNZ(m_state.a); m_state.cycles++; }
            break;
        }
        case 0x25: { // AND zp / ANDQ zp
            uint16_t addr = getZp();
            if (isQuad) doAnd32(read32(addr));
            else { m_state.a &= read8(addr); updateNZ(m_state.a); m_state.cycles++; }
            break;
        }
        case 0x35: { // AND zp,X / ANDQ zp,X
            uint16_t addr = getZpX();
            if (isQuad) doAnd32(read32(addr));
            else { m_state.a &= read8(addr); updateNZ(m_state.a); m_state.cycles++; }
            break;
        }
        case 0x21: m_state.a &= read8(getIndX()); updateNZ(m_state.a); break;
        case 0x31: m_state.a &= read8(getIndY()); updateNZ(m_state.a); break;
        case 0x32: { // AND (zp),z / ANDQ (zp),z
            uint32_t base = getIndPhys(); uint32_t addr = base + m_state.z;
            if (isQuad) doAnd32(is32BitInd ? read32_phys(addr) : read32((uint16_t)addr));
            else { m_state.a &= (is32BitInd ? read8_phys(addr) : read8((uint16_t)addr)); updateNZ(m_state.a); m_state.cycles++; }
            break;
        }

        case 0x09: { // ORA #imm / ORAQ #imm32
            if (isQuad) {
                uint32_t v = (uint32_t)read8(m_state.pc++) | ((uint32_t)read8(m_state.pc++) << 8) |
                             ((uint32_t)read8(m_state.pc++) << 16) | ((uint32_t)read8(m_state.pc++) << 24);
                doOra32(v);
            } else { m_state.a |= read8(m_state.pc++); updateNZ(m_state.a); m_state.cycles++; }
            break;
        }
        case 0x0D: { // ORA abs / ORAQ abs
            uint16_t addr = getAbs();
            if (isQuad) doOra32(read32(addr));
            else { m_state.a |= read8(addr); updateNZ(m_state.a); m_state.cycles++; }
            break;
        }
        case 0x1D: { // ORA abs,X / ORAQ abs,X
            uint16_t addr = getAbsX();
            if (isQuad) doOra32(read32(addr));
            else { m_state.a |= read8(addr); updateNZ(m_state.a); m_state.cycles++; }
            break;
        }
        case 0x19: { // ORA abs,Y / ORAQ abs,Y
            uint16_t addr = getAbsY();
            if (isQuad) doOra32(read32(addr));
            else { m_state.a |= read8(addr); updateNZ(m_state.a); m_state.cycles++; }
            break;
        }
        case 0x05: { // ORA zp / ORAQ zp
            uint16_t addr = getZp();
            if (isQuad) doOra32(read32(addr));
            else { m_state.a |= read8(addr); updateNZ(m_state.a); m_state.cycles++; }
            break;
        }
        case 0x15: { // ORA zp,X / ORAQ zp,X
            uint16_t addr = getZpX();
            if (isQuad) doOra32(read32(addr));
            else { m_state.a |= read8(addr); updateNZ(m_state.a); m_state.cycles++; }
            break;
        }
        case 0x01: m_state.a |= read8(getIndX()); updateNZ(m_state.a); break;
        case 0x11: m_state.a |= read8(getIndY()); updateNZ(m_state.a); break;
        case 0x12: { // ORA (zp),z / ORAQ (zp),z
            uint32_t base = getIndPhys(); uint32_t addr = base + m_state.z;
            if (isQuad) doOra32(is32BitInd ? read32_phys(addr) : read32((uint16_t)addr));
            else { m_state.a |= (is32BitInd ? read8_phys(addr) : read8((uint16_t)addr)); updateNZ(m_state.a); m_state.cycles++; }
            break;
        }

        case 0x49: { // EOR #imm / EORQ #imm32
            if (isQuad) {
                uint32_t v = (uint32_t)read8(m_state.pc++) | ((uint32_t)read8(m_state.pc++) << 8) |
                             ((uint32_t)read8(m_state.pc++) << 16) | ((uint32_t)read8(m_state.pc++) << 24);
                doEor32(v);
            } else { m_state.a ^= read8(m_state.pc++); updateNZ(m_state.a); m_state.cycles++; }
            break;
        }
        case 0x4D: { // EOR abs / EORQ abs
            uint16_t addr = getAbs();
            if (isQuad) doEor32(read32(addr));
            else { m_state.a ^= read8(addr); updateNZ(m_state.a); m_state.cycles++; }
            break;
        }
        case 0x5D: { // EOR abs,X / EORQ abs,X
            uint16_t addr = getAbsX();
            if (isQuad) doEor32(read32(addr));
            else { m_state.a ^= read8(addr); updateNZ(m_state.a); m_state.cycles++; }
            break;
        }
        case 0x59: { // EOR abs,Y / EORQ abs,Y
            uint16_t addr = getAbsY();
            if (isQuad) doEor32(read32(addr));
            else { m_state.a ^= read8(addr); updateNZ(m_state.a); m_state.cycles++; }
            break;
        }
        case 0x45: { // EOR zp / EORQ zp
            uint16_t addr = getZp();
            if (isQuad) doEor32(read32(addr));
            else { m_state.a ^= read8(addr); updateNZ(m_state.a); m_state.cycles++; }
            break;
        }
        case 0x55: { // EOR zp,X / EORQ zp,X
            uint16_t addr = getZpX();
            if (isQuad) doEor32(read32(addr));
            else { m_state.a ^= read8(addr); updateNZ(m_state.a); m_state.cycles++; }
            break;
        }
        case 0x41: m_state.a ^= read8(getIndX()); updateNZ(m_state.a); break;
        case 0x51: m_state.a ^= read8(getIndY()); updateNZ(m_state.a); break;
        case 0x52: { // EOR (zp),z / EORQ (zp),z
            uint32_t base = getIndPhys(); uint32_t addr = base + m_state.z;
            if (isQuad) doEor32(is32BitInd ? read32_phys(addr) : read32((uint16_t)addr));
            else { m_state.a ^= (is32BitInd ? read8_phys(addr) : read8((uint16_t)addr)); updateNZ(m_state.a); m_state.cycles++; }
            break;
        }

        case 0x04: { uint16_t a=getZp(); uint8_t v=read8(a); if(!(m_state.a&v)) m_state.p|=FLAG_Z; else m_state.p&=~FLAG_Z; write8(a,v|m_state.a); m_state.cycles++; break; } // TSB zp
        case 0x0C: { uint16_t a=getAbs(); uint8_t v=read8(a); if(!(m_state.a&v)) m_state.p|=FLAG_Z; else m_state.p&=~FLAG_Z; write8(a,v|m_state.a); m_state.cycles++; break; } // TSB abs
        case 0x14: { uint16_t a=getZp(); uint8_t v=read8(a); if(!(m_state.a&v)) m_state.p|=FLAG_Z; else m_state.p&=~FLAG_Z; write8(a,v&~m_state.a); m_state.cycles++; break; } // TRB zp
        case 0x1C: { uint16_t a=getAbs(); uint8_t v=read8(a); if(!(m_state.a&v)) m_state.p|=FLAG_Z; else m_state.p&=~FLAG_Z; write8(a,v&~m_state.a); m_state.cycles++; break; } // TRB abs

        case 0x43: { // ASR A / ASRQ A
            if (isQuad) {
                uint32_t q = regRead(8); m_state.p = (m_state.p & ~FLAG_C) | (q & 1);
                q = (q & 0x80000000) | (q >> 1); regWrite(8, q); updateNZ32(q);
            } else {
                m_state.p = (m_state.p & ~FLAG_C) | (m_state.a & 1);
                m_state.a = (m_state.a & 0x80) | (m_state.a >> 1);
                updateNZ(m_state.a);
            }
            break;
        }
        case 0x44: { // ASR zp / ASRQ zp
            uint16_t a=getZp();
            if (isQuad) {
                uint32_t q = read32(a); m_state.p = (m_state.p & ~FLAG_C) | (q & 1);
                q = (q & 0x80000000) | (q >> 1); write32(a, q); updateNZ32(q);
            } else {
                uint8_t v=read8(a); m_state.p = (m_state.p & ~FLAG_C) | (v & 1);
                v = (v & 0x80) | (v >> 1); write8(a, v); updateNZ(v);
            }
            break;
        }
        case 0x54: { // ASR zp,X / ASRQ zp,X
            uint16_t a=getZpX();
            if (isQuad) {
                uint32_t q = read32(a); m_state.p = (m_state.p & ~FLAG_C) | (q & 1);
                q = (q & 0x80000000) | (q >> 1); write32(a, q); updateNZ32(q);
            } else {
                uint8_t v=read8(a); m_state.p = (m_state.p & ~FLAG_C) | (v & 1);
                v = (v & 0x80) | (v >> 1); write8(a, v); updateNZ(v);
            }
            break;
        }

        // --- 3. Increments / Decrements ---
        case 0xE8: m_state.x++; updateNZ(m_state.x); break;
        case 0xCA: m_state.x--; updateNZ(m_state.x); break;
        case 0xC8: m_state.y++; updateNZ(m_state.y); break;
        case 0x88: m_state.y--; updateNZ(m_state.y); break;
        case 0x1B: m_state.z++; updateNZ(m_state.z); break; // INZ
        case 0x3B: m_state.z--; updateNZ(m_state.z); break; // DEZ
        case 0x1A: { // INC A / INQ A
            if (isQuad) { uint32_t q = regRead(8) + 1; regWrite(8, q); updateNZ32(q); }
            else { m_state.a++; updateNZ(m_state.a); }
            break;
        }
        case 0x3A: { // DEC A / DEQ A
            if (isQuad) { uint32_t q = regRead(8) - 1; regWrite(8, q); updateNZ32(q); }
            else { m_state.a--; updateNZ(m_state.a); }
            break;
        }
        case 0xEE: { // INC abs / INQ abs
            uint16_t a=getAbs();
            if (isQuad) { uint32_t q = read32(a) + 1; write32(a, q); updateNZ32(q); }
            else { uint8_t v=read8(a)+1; write8(a,v); updateNZ(v); }
            m_state.cycles++; break;
        }
        case 0xFE: { // INC abs,X / INQ abs,X
            uint16_t a=getAbsX();
            if (isQuad) { uint32_t q = read32(a) + 1; write32(a, q); updateNZ32(q); }
            else { uint8_t v=read8(a)+1; write8(a,v); updateNZ(v); }
            m_state.cycles++; break;
        }
        case 0xE6: { // INC zp / INQ zp
            uint16_t a=getZp();
            if (isQuad) { uint32_t q = read32(a) + 1; write32(a, q); updateNZ32(q); }
            else { uint8_t v=read8(a)+1; write8(a,v); updateNZ(v); }
            m_state.cycles++; break;
        }
        case 0xF6: { // INC zp,X / INQ zp,X
            uint16_t a=getZpX();
            if (isQuad) { uint32_t q = read32(a) + 1; write32(a, q); updateNZ32(q); }
            else { uint8_t v=read8(a)+1; write8(a,v); updateNZ(v); }
            m_state.cycles++; break;
        }
        case 0xCE: { // DEC abs / DEQ abs
            uint16_t a=getAbs();
            if (isQuad) { uint32_t q = read32(a) - 1; write32(a, q); updateNZ32(q); }
            else { uint8_t v=read8(a)-1; write8(a,v); updateNZ(v); }
            m_state.cycles++; break;
        }
        case 0xDE: { // DEC abs,X / DEQ abs,X
            uint16_t a=getAbsX();
            if (isQuad) { uint32_t q = read32(a) - 1; write32(a, q); updateNZ32(q); }
            else { uint8_t v=read8(a)-1; write8(a,v); updateNZ(v); }
            m_state.cycles++; break;
        }
        case 0xC6: { // DEC zp / DEQ zp
            uint16_t a=getZp();
            if (isQuad) { uint32_t q = read32(a) - 1; write32(a, q); updateNZ32(q); }
            else { uint8_t v=read8(a)-1; write8(a,v); updateNZ(v); }
            m_state.cycles++; break;
        }
        case 0xD6: { // DEC zp,X / DEQ zp,X
            uint16_t a=getZpX();
            if (isQuad) { uint32_t q = read32(a) - 1; write32(a, q); updateNZ32(q); }
            else { uint8_t v=read8(a)-1; write8(a,v); updateNZ(v); }
            m_state.cycles++; break;
        }

        // --- 4. Transfers ---
        case 0xAA: m_state.x = m_state.a; updateNZ(m_state.x); break;
        case 0x8A: m_state.a = m_state.x; updateNZ(m_state.a); break;
        case 0xA8: m_state.y = m_state.a; updateNZ(m_state.y); break;
        case 0x98: m_state.a = m_state.y; updateNZ(m_state.a); break;
        case 0x4B: m_state.z = m_state.a; updateNZ(m_state.z); break;
        case 0x6B: m_state.a = m_state.z; updateNZ(m_state.a); break;
        case 0x5B: m_state.b = m_state.a; updateNZ(m_state.b); break;
        case 0x7B: m_state.a = m_state.b; updateNZ(m_state.a); break;
        case 0xBA: m_state.x = (uint8_t)(m_state.sp & 0xFF); updateNZ(m_state.x); break;
        case 0x9A: m_state.sp = (m_state.sp & 0xFF00) | m_state.x; break;

        // --- 5. Flags ---
        case 0x18: m_state.p &= ~FLAG_C; break;
        case 0x38: m_state.p |=  FLAG_C; break;
        case 0x58: m_state.p &= ~FLAG_I; break;
        case 0x78: m_state.p |=  FLAG_I; break;
        case 0xD8: m_state.p &= ~FLAG_D; break;
        case 0xF8: m_state.p |=  FLAG_D; break;
        case 0xB8: m_state.p &= ~FLAG_V; break;

        // --- 6. Stack ---
        case 0x48: push8(m_state.a); break;
        case 0x68: m_state.a = pull8(); updateNZ(m_state.a); break;
        case 0x08: push8(m_state.p); break;
        case 0x28: { uint8_t pulled = pull8(); m_state.p = (pulled & ~(FLAG_B|FLAG_E)) | (m_state.p & (FLAG_B|FLAG_E)); break; } // PLP
        case 0xDA: push8(m_state.x); break;
        case 0xFA: m_state.x = pull8(); updateNZ(m_state.x); break;
        case 0x5A: push8(m_state.y); break;
        case 0x7A: m_state.y = pull8(); updateNZ(m_state.y); break;
        case 0xDB: push8(m_state.z); break;
        case 0xFB: m_state.z = pull8(); updateNZ(m_state.z); break;

        case 0x22: { uint16_t a=getAbs(); uint16_t t=read8(a)|(read8(a+1)<<8); push16(m_state.pc-1); m_state.pc=t; m_state.cycles+=2; break; } // JSR (abs)
        case 0x23: { uint16_t a=(getAbs()+m_state.x); uint16_t t=read8(a)|(read8(a+1)<<8); push16(m_state.pc-1); m_state.pc=t; m_state.cycles+=2; break; } // JSR (abs,X)

        case 0xF4: { uint16_t l=read8(m_state.pc++); uint16_t h=read8(m_state.pc++); uint16_t v=l|(h<<8); m_state.cycles+=2; push16(v); break; } // PHW #imm16
        case 0xFC: { uint16_t a=getAbs(); uint16_t v=read8(a)|(read8(a+1)<<8); m_state.cycles+=2; push16(v); break; } // PHW abs

        // --- 7. Flow Control ---
        case 0x4C: m_state.pc = getAbs(); break;
        case 0x6C: { uint16_t a=getAbs(); m_state.pc = read8(a)|(read8(a+1)<<8); m_state.cycles += 2; break; }
        case 0x20: { uint16_t t=getAbs(); uint16_t r=m_state.pc-1; push16(r); m_state.pc=t; m_state.cycles++; break; } // JSR
        case 0x60: { m_state.pc = pull16() + 1; m_state.cycles += 3; break; } // RTS
        case 0x40: { uint8_t pulled = pull8(); m_state.p = (pulled & ~(FLAG_B|FLAG_E)) | (m_state.p & (FLAG_B|FLAG_E)); m_state.pc = pull16(); m_state.cycles += 3; break; } // RTI
        case 0x80: { int8_t r=(int8_t)read8(m_state.pc++); m_state.pc+=r; m_state.cycles++; break; } // BRA
        case 0x63: { int16_t r=(int16_t)read8(m_state.pc)|((int16_t)read8(m_state.pc+1)<<8); m_state.pc+=2; uint16_t ret=m_state.pc-1; push16(ret); m_state.pc+=r; m_state.cycles+=4; break; } // BSR (16-bit)
        case 0x62: { uint16_t ret = pull16(); uint8_t imm = read8(m_state.pc++); m_state.pc = ret + 1; m_state.sp += imm; m_state.cycles += 4; break; } // RTN (RTS imm)

        case 0xF0: { int8_t r=(int8_t)read8(m_state.pc++); if(m_state.p&FLAG_Z) { m_state.pc+=r; m_state.cycles++; } break; }
        case 0xD0: { int8_t r=(int8_t)read8(m_state.pc++); if(!(m_state.p&FLAG_Z)) { m_state.pc+=r; m_state.cycles++; } break; }
        case 0xB0: { int8_t r=(int8_t)read8(m_state.pc++); if(m_state.p&FLAG_C) { m_state.pc+=r; m_state.cycles++; } break; }
        case 0x90: { int8_t r=(int8_t)read8(m_state.pc++); if(!(m_state.p&FLAG_C)) { m_state.pc+=r; m_state.cycles++; } break; }
        case 0x10: { int8_t r=(int8_t)read8(m_state.pc++); if(!(m_state.p&FLAG_N)) { m_state.pc+=r; m_state.cycles++; } break; }
        case 0x30: { int8_t r=(int8_t)read8(m_state.pc++); if(m_state.p&FLAG_N) { m_state.pc+=r; m_state.cycles++; } break; }
        case 0x50: { int8_t r=(int8_t)read8(m_state.pc++); if(!(m_state.p&FLAG_V)) { m_state.pc+=r; m_state.cycles++; } break; }
        case 0x70: { int8_t r=(int8_t)read8(m_state.pc++); if(m_state.p&FLAG_V) { m_state.pc+=r; m_state.cycles++; } break; }

        // Long branches (relfar)
        case 0x83: { int16_t r=(int16_t)read8(m_state.pc)|((int16_t)read8(m_state.pc+1)<<8); m_state.pc+=2+r; m_state.cycles+=3; break; } // LBRA
        case 0xD3: { int16_t r=(int16_t)read8(m_state.pc)|((int16_t)read8(m_state.pc+1)<<8); m_state.pc+=2; if(!(m_state.p&FLAG_Z)) { m_state.pc+=r; m_state.cycles++; } m_state.cycles+=2; break; } // LBNE
        case 0xF3: { int16_t r=(int16_t)read8(m_state.pc)|((int16_t)read8(m_state.pc+1)<<8); m_state.pc+=2; if(m_state.p&FLAG_Z) { m_state.pc+=r; m_state.cycles++; } m_state.cycles+=2; break; } // LBEQ
        case 0x93: { int16_t r=(int16_t)read8(m_state.pc)|((int16_t)read8(m_state.pc+1)<<8); m_state.pc+=2; if(!(m_state.p&FLAG_C)) { m_state.pc+=r; m_state.cycles++; } m_state.cycles+=2; break; } // LBCC
        case 0xB3: { int16_t r=(int16_t)read8(m_state.pc)|((int16_t)read8(m_state.pc+1)<<8); m_state.pc+=2; if(m_state.p&FLAG_C) { m_state.pc+=r; m_state.cycles++; } m_state.cycles+=2; break; } // LBCS
        case 0x13: { int16_t r=(int16_t)read8(m_state.pc)|((int16_t)read8(m_state.pc+1)<<8); m_state.pc+=2; if(!(m_state.p&FLAG_N)) { m_state.pc+=r; m_state.cycles++; } m_state.cycles+=2; break; } // LBPL
        case 0x33: { int16_t r=(int16_t)read8(m_state.pc)|((int16_t)read8(m_state.pc+1)<<8); m_state.pc+=2; if(m_state.p&FLAG_N) { m_state.pc+=r; m_state.cycles++; } m_state.cycles+=2; break; } // LBMI
        case 0x53: { int16_t r=(int16_t)read8(m_state.pc)|((int16_t)read8(m_state.pc+1)<<8); m_state.pc+=2; if(!(m_state.p&FLAG_V)) { m_state.pc+=r; m_state.cycles++; } m_state.cycles+=2; break; } // LBVC
        case 0x73: { int16_t r=(int16_t)read8(m_state.pc)|((int16_t)read8(m_state.pc+1)<<8); m_state.pc+=2; if(m_state.p&FLAG_V) { m_state.pc+=r; m_state.cycles++; } m_state.cycles+=2; break; } // LBVS
        
        // CPX/CPY/CPZ (additional modes)
        case 0xE0: { uint8_t v=read8(m_state.pc++); uint16_t d=(uint16_t)m_state.x-v; m_state.p &= ~FLAG_C; if(m_state.x>=v) m_state.p|=FLAG_C; updateNZ((uint8_t)(d&0xFF)); m_state.cycles++; break; } // CPX #imm
        case 0xEC: { uint8_t v=read8(getAbs()); uint16_t d=(uint16_t)m_state.x-v; m_state.p &= ~FLAG_C; if(m_state.x>=v) m_state.p|=FLAG_C; updateNZ((uint8_t)(d&0xFF)); m_state.cycles++; break; } // CPX abs
        case 0xE4: { uint8_t v=read8(getZp()); uint16_t d=(uint16_t)m_state.x-v; m_state.p &= ~FLAG_C; if(m_state.x>=v) m_state.p|=FLAG_C; updateNZ((uint8_t)(d&0xFF)); m_state.cycles++; break; } // CPX zp
        case 0xC0: { uint8_t v=read8(m_state.pc++); uint16_t d=(uint16_t)m_state.y-v; m_state.p &= ~FLAG_C; if(m_state.y>=v) m_state.p|=FLAG_C; updateNZ((uint8_t)(d&0xFF)); m_state.cycles++; break; } // CPY #imm
        case 0xCC: { uint8_t v=read8(getAbs()); uint16_t d=(uint16_t)m_state.y-v; m_state.p &= ~FLAG_C; if(m_state.y>=v) m_state.p|=FLAG_C; updateNZ((uint8_t)(d&0xFF)); m_state.cycles++; break; } // CPY abs
        case 0xC4: { uint8_t v=read8(getZp()); uint16_t d=(uint16_t)m_state.y-v; m_state.p &= ~FLAG_C; if(m_state.y>=v) m_state.p|=FLAG_C; updateNZ((uint8_t)(d&0xFF)); m_state.cycles++; break; } // CPY zp
        case 0xC2: { uint8_t v=read8(m_state.pc++); uint16_t d=(uint16_t)m_state.z-v; m_state.p &= ~FLAG_C; if(m_state.z>=v) m_state.p|=FLAG_C; updateNZ((uint8_t)(d&0xFF)); m_state.cycles++; break; } // CPZ #imm (45GS02)
        case 0xD4: { uint8_t v=read8(getZp()); uint16_t d=(uint16_t)m_state.z-v; m_state.p &= ~FLAG_C; if(m_state.z>=v) m_state.p|=FLAG_C; updateNZ((uint8_t)(d&0xFF)); m_state.cycles++; break; } // CPZ zp (45GS02)
        case 0xDC: { uint8_t v=read8(getAbs()); uint16_t d=(uint16_t)m_state.z-v; m_state.p &= ~FLAG_C; if(m_state.z>=v) m_state.p|=FLAG_C; updateNZ((uint8_t)(d&0xFF)); m_state.cycles++; break; } // CPZ abs (45GS02)

        // --- 8. Bit Manipulation ---
        case 0x24: { // BIT zp / BITQ zp
            uint16_t addr = getZp();
            if (isQuad) { uint32_t v=read32(addr); m_state.p &= ~(FLAG_Z|FLAG_N|FLAG_V); if(!(regRead(8)&v)) m_state.p|=FLAG_Z; if(v&0x80000000) m_state.p|=FLAG_N; if(v&0x40000000) m_state.p|=FLAG_V; }
            else { uint8_t v=read8(addr); m_state.p &= ~(FLAG_Z|FLAG_N|FLAG_V); if(!(m_state.a&v)) m_state.p|=FLAG_Z; if(v&0x80) m_state.p|=FLAG_N; if(v&0x40) m_state.p|=FLAG_V; }
            break;
        }
        case 0x2C: { // BIT abs / BITQ abs
            uint16_t addr = getAbs();
            if (isQuad) { uint32_t v=read32(addr); m_state.p &= ~(FLAG_Z|FLAG_N|FLAG_V); if(!(regRead(8)&v)) m_state.p|=FLAG_Z; if(v&0x80000000) m_state.p|=FLAG_N; if(v&0x40000000) m_state.p|=FLAG_V; }
            else { uint8_t v=read8(addr); m_state.p &= ~(FLAG_Z|FLAG_N|FLAG_V); if(!(m_state.a&v)) m_state.p|=FLAG_Z; if(v&0x80) m_state.p|=FLAG_N; if(v&0x40) m_state.p|=FLAG_V; }
            break;
        }
        case 0x34: { uint8_t v=read8(getZpX()); m_state.p &= ~(FLAG_Z|FLAG_N|FLAG_V); if(!(m_state.a&v)) m_state.p|=FLAG_Z; if(v&0x80) m_state.p|=FLAG_N; if(v&0x40) m_state.p|=FLAG_V; break; }
        case 0x3C: { uint8_t v=read8(getAbsX()); m_state.p &= ~(FLAG_Z|FLAG_N|FLAG_V); if(!(m_state.a&v)) m_state.p|=FLAG_Z; if(v&0x80) m_state.p|=FLAG_N; if(v&0x40) m_state.p|=FLAG_V; break; }
        case 0x89: { uint8_t v=read8(m_state.pc++); m_state.cycles++; updateNZ(m_state.a&v); break; }
        
        // RMBx / SMBx (Rockwell/W65C02/45GS02)
        case 0x07: case 0x17: case 0x27: case 0x37: case 0x47: case 0x57: case 0x67: case 0x77: {
            uint16_t a = getZp(); uint8_t v = read8(a); uint8_t bit = (op >> 4); 
            v &= ~(1 << bit); write8(a, v); break;
        }
        case 0x87: case 0x97: case 0xA7: case 0xB7: case 0xC7: case 0xD7: case 0xE7: case 0xF7: {
            uint16_t a = getZp(); uint8_t v = read8(a); uint8_t bit = ((op - 0x87) >> 4); 
            v |= (1 << bit); write8(a, v); break;
        }
        // BBRx / BBSx
        case 0x0F: case 0x1F: case 0x2F: case 0x3F: case 0x4F: case 0x5F: case 0x6F: case 0x7F: {
            uint16_t a = getZp(); uint8_t v = read8(a); int8_t r = (int8_t)read8(m_state.pc++); 
            uint8_t bit = (op >> 4);
            if (!(v & (1 << bit))) m_state.pc += r; 
            break;
        }
        case 0x8F: case 0x9F: case 0xAF: case 0xBF: case 0xCF: case 0xDF: case 0xEF: case 0xFF: {
            uint16_t a = getZp(); uint8_t v = read8(a); int8_t r = (int8_t)read8(m_state.pc++); 
            uint8_t bit = ((op - 0x8F) >> 4);
            if (v & (1 << bit)) m_state.pc += r; 
            break;
        }

        // --- 9. Shifts ---
        case 0x0A: { // ASL A / ASLQ
            if (isQuad) {
                uint32_t q = regRead(8); m_state.p = (m_state.p & ~FLAG_C) | (q >> 31);
                q <<= 1; regWrite(8, q); updateNZ32(q); m_state.cycles++;
            } else {
                uint8_t c=m_state.a>>7; m_state.a<<=1; m_state.p=(m_state.p&~FLAG_C)|c; updateNZ(m_state.a);
            }
            break;
        }
        case 0x0E: { // ASL abs / ASLQ abs
            uint16_t a=getAbs();
            if (isQuad) { uint32_t q = read32(a); m_state.p = (m_state.p & ~FLAG_C) | (q >> 31); q <<= 1; write32(a, q); updateNZ32(q); }
            else { uint8_t v=read8(a); uint8_t c=v>>7; v<<=1; m_state.p=(m_state.p&~FLAG_C)|c; write8(a,v); updateNZ(v); }
            m_state.cycles++; break;
        }
        case 0x1E: { // ASL abs,X / ASLQ abs,X
            uint16_t a=getAbsX();
            if (isQuad) { uint32_t q = read32(a); m_state.p = (m_state.p & ~FLAG_C) | (q >> 31); q <<= 1; write32(a, q); updateNZ32(q); }
            else { uint8_t v=read8(a); uint8_t c=v>>7; v<<=1; m_state.p=(m_state.p&~FLAG_C)|c; write8(a,v); updateNZ(v); }
            m_state.cycles++; break;
        }
        case 0x06: { // ASL zp / ASLQ zp
            uint16_t a=getZp();
            if (isQuad) { uint32_t q = read32(a); m_state.p = (m_state.p & ~FLAG_C) | (q >> 31); q <<= 1; write32(a, q); updateNZ32(q); }
            else { uint8_t v=read8(a); uint8_t c=v>>7; v<<=1; m_state.p=(m_state.p&~FLAG_C)|c; write8(a,v); updateNZ(v); }
            m_state.cycles++; break;
        }
        case 0x16: { // ASL zp,X / ASLQ zp,X
            uint16_t a=getZpX();
            if (isQuad) { uint32_t q = read32(a); m_state.p = (m_state.p & ~FLAG_C) | (q >> 31); q <<= 1; write32(a, q); updateNZ32(q); }
            else { uint8_t v=read8(a); uint8_t c=v>>7; v<<=1; m_state.p=(m_state.p&~FLAG_C)|c; write8(a,v); updateNZ(v); }
            m_state.cycles++; break;
        }

        case 0x4A: { // LSR A / LSRQ
            if (isQuad) {
                uint32_t q = regRead(8); m_state.p = (m_state.p & ~FLAG_C) | (q & 1);
                q >>= 1; regWrite(8, q); updateNZ32(q); m_state.cycles++;
            } else {
                uint8_t c=m_state.a&1; m_state.a>>=1; m_state.p=(m_state.p&~FLAG_C)|c; updateNZ(m_state.a);
            }
            break;
        }
        case 0x4E: { // LSR abs / LSRQ abs
            uint16_t a=getAbs();
            if (isQuad) { uint32_t q = read32(a); m_state.p = (m_state.p & ~FLAG_C) | (q & 1); q >>= 1; write32(a, q); updateNZ32(q); }
            else { uint8_t v=read8(a); uint8_t c=v&1; v>>=1; m_state.p=(m_state.p&~FLAG_C)|c; write8(a,v); updateNZ(v); }
            m_state.cycles++; break;
        }
        case 0x5E: { // LSR abs,X / LSRQ abs,X
            uint16_t a=getAbsX();
            if (isQuad) { uint32_t q = read32(a); m_state.p = (m_state.p & ~FLAG_C) | (q & 1); q >>= 1; write32(a, q); updateNZ32(q); }
            else { uint8_t v=read8(a); uint8_t c=v&1; v>>=1; m_state.p=(m_state.p&~FLAG_C)|c; write8(a,v); updateNZ(v); }
            m_state.cycles++; break;
        }
        case 0x46: { // LSR zp / LSRQ zp
            uint16_t a=getZp();
            if (isQuad) { uint32_t q = read32(a); m_state.p = (m_state.p & ~FLAG_C) | (q & 1); q >>= 1; write32(a, q); updateNZ32(q); }
            else { uint8_t v=read8(a); uint8_t c=v&1; v>>=1; m_state.p=(m_state.p&~FLAG_C)|c; write8(a,v); updateNZ(v); }
            m_state.cycles++; break;
        }
        case 0x56: { // LSR zp,X / LSRQ zp,X
            uint16_t a=getZpX();
            if (isQuad) { uint32_t q = read32(a); m_state.p = (m_state.p & ~FLAG_C) | (q & 1); q >>= 1; write32(a, q); updateNZ32(q); }
            else { uint8_t v=read8(a); uint8_t c=v&1; v>>=1; m_state.p=(m_state.p&~FLAG_C)|c; write8(a,v); updateNZ(v); }
            m_state.cycles++; break;
        }

        case 0x2A: { // ROL A / ROLQ
            if (isQuad) {
                uint32_t q = regRead(8); uint32_t c = (m_state.p & FLAG_C) ? 1 : 0;
                m_state.p = (m_state.p & ~FLAG_C) | (q >> 31);
                q = (q << 1) | c; regWrite(8, q); updateNZ32(q); m_state.cycles++;
            } else {
                uint8_t c=(m_state.p&FLAG_C)?1:0; uint8_t oldC = m_state.a>>7; m_state.a=(m_state.a<<1)|c; m_state.p=(m_state.p&~FLAG_C)|oldC; updateNZ(m_state.a);
            }
            break;
        }
        case 0x2E: { // ROL abs / ROLQ abs
            uint16_t a=getAbs();
            if (isQuad) { uint32_t q = read32(a); uint32_t c = (m_state.p & FLAG_C)?1:0; m_state.p=(m_state.p&~FLAG_C)|(q>>31); q=(q<<1)|c; write32(a,q); updateNZ32(q); }
            else { uint8_t v=read8(a); uint8_t c=(m_state.p&FLAG_C)?1:0; uint8_t oldC=v>>7; v=(v<<1)|c; m_state.p=(m_state.p&~FLAG_C)|oldC; write8(a,v); updateNZ(v); }
            m_state.cycles++; break;
        }
        case 0x3E: { // ROL abs,X / ROLQ abs,X
            uint16_t a=getAbsX();
            if (isQuad) { uint32_t q = read32(a); uint32_t c = (m_state.p & FLAG_C)?1:0; m_state.p=(m_state.p&~FLAG_C)|(q>>31); q=(q<<1)|c; write32(a,q); updateNZ32(q); }
            else { uint8_t v=read8(a); uint8_t c=(m_state.p&FLAG_C)?1:0; uint8_t oldC=v>>7; v=(v<<1)|c; m_state.p=(m_state.p&~FLAG_C)|oldC; write8(a,v); updateNZ(v); }
            m_state.cycles++; break;
        }
        case 0x26: { // ROL zp / ROLQ zp
            uint16_t a=getZp();
            if (isQuad) { uint32_t q = read32(a); uint32_t c = (m_state.p & FLAG_C)?1:0; m_state.p=(m_state.p&~FLAG_C)|(q>>31); q=(q<<1)|c; write32(a,q); updateNZ32(q); }
            else { uint8_t v=read8(a); uint8_t c=(m_state.p&FLAG_C)?1:0; uint8_t oldC=v>>7; v=(v<<1)|c; m_state.p=(m_state.p&~FLAG_C)|oldC; write8(a,v); updateNZ(v); }
            m_state.cycles++; break;
        }
        case 0x36: { // ROL zp,X / ROLQ zp,X
            uint16_t a=getZpX();
            if (isQuad) { uint32_t q = read32(a); uint32_t c = (m_state.p & FLAG_C)?1:0; m_state.p=(m_state.p&~FLAG_C)|(q>>31); q=(q<<1)|c; write32(a,q); updateNZ32(q); }
            else { uint8_t v=read8(a); uint8_t c=(m_state.p&FLAG_C)?1:0; uint8_t oldC=v>>7; v=(v<<1)|c; m_state.p=(m_state.p&~FLAG_C)|oldC; write8(a,v); updateNZ(v); }
            m_state.cycles++; break;
        }

        case 0x6A: { // ROR A / RORQ
            if (isQuad) {
                uint32_t q = regRead(8); uint32_t c = (m_state.p & FLAG_C) ? 0x80000000 : 0;
                m_state.p = (m_state.p & ~FLAG_C) | (q & 1);
                q = (q >> 1) | c; regWrite(8, q); updateNZ32(q); m_state.cycles++;
            } else {
                uint8_t c=(m_state.p&FLAG_C)?0x80:0; uint8_t oldC=m_state.a&1; m_state.a=(m_state.a>>1)|c; m_state.p=(m_state.p&~FLAG_C)|oldC; updateNZ(m_state.a);
            }
            break;
        }
        case 0x6E: { // ROR abs / RORQ abs
            uint16_t a=getAbs();
            if (isQuad) { uint32_t q = read32(a); uint32_t c=(m_state.p&FLAG_C)?0x80000000:0; m_state.p=(m_state.p&~FLAG_C)|(q&1); q=(q>>1)|c; write32(a,q); updateNZ32(q); }
            else { uint8_t v=read8(a); uint8_t c=(m_state.p&FLAG_C)?0x80:0; uint8_t oldC=v&1; v=(v>>1)|c; m_state.p=(m_state.p&~FLAG_C)|oldC; write8(a,v); updateNZ(v); }
            m_state.cycles++; break;
        }
        case 0x7E: { // ROR abs,X / RORQ abs,X
            uint16_t a=getAbsX();
            if (isQuad) { uint32_t q = read32(a); uint32_t c=(m_state.p&FLAG_C)?0x80000000:0; m_state.p=(m_state.p&~FLAG_C)|(q&1); q=(q>>1)|c; write32(a,q); updateNZ32(q); }
            else { uint8_t v=read8(a); uint8_t c=(m_state.p&FLAG_C)?0x80:0; uint8_t oldC=v&1; v=(v>>1)|c; m_state.p=(m_state.p&~FLAG_C)|oldC; write8(a,v); updateNZ(v); }
            m_state.cycles++; break;
        }
        case 0x66: { // ROR zp / RORQ zp
            uint16_t a=getZp();
            if (isQuad) { uint32_t q = read32(a); uint32_t c=(m_state.p&FLAG_C)?0x80000000:0; m_state.p=(m_state.p&~FLAG_C)|(q&1); q=(q>>1)|c; write32(a,q); updateNZ32(q); }
            else { uint8_t v=read8(a); uint8_t c=(m_state.p&FLAG_C)?0x80:0; uint8_t oldC=v&1; v=(v>>1)|c; m_state.p=(m_state.p&~FLAG_C)|oldC; write8(a,v); updateNZ(v); }
            m_state.cycles++; break;
        }
        case 0x76: { // ROR zp,X / RORQ zp,X
            uint16_t a=getZpX();
            if (isQuad) { uint32_t q = read32(a); uint32_t c=(m_state.p&FLAG_C)?0x80000000:0; m_state.p=(m_state.p&~FLAG_C)|(q&1); q=(q>>1)|c; write32(a,q); updateNZ32(q); }
            else { uint8_t v=read8(a); uint8_t c=(m_state.p&FLAG_C)?0x80:0; uint8_t oldC=v&1; v=(v>>1)|c; m_state.p=(m_state.p&~FLAG_C)|oldC; write8(a,v); updateNZ(v); }
            m_state.cycles++; break;
        }

        // --- 10. 45GS02 Extensions ---
        case 0x42: { // NEG A / NEGQ A
            if (isQuad) { uint32_t q = -(int32_t)regRead(8); regWrite(8, q); updateNZ32(q); }
            else { m_state.a = -(int8_t)m_state.a; updateNZ(m_state.a); }
            m_state.cycles++; break;
        }
        case 0x5C: { // MAP - Set memory mapping
            // MEGA65 MAP register encoding:
            //   Lower 32KB: mb_offset = ((X & 0x0F) << 8) | A   (12-bit)
            //                enables  = (X >> 4) & 0x0F  (blocks 0-3)
            //   Upper 32KB: mb_offset = ((Z & 0x0F) << 8) | Y   (12-bit)
            //                enables  = (Z >> 4) & 0x0F  (blocks 4-7)
            //
            // Physical address for an enabled block b:
            //   phys = vaddr + (mb_offset << 8)
            //
            // MapMmu stores per-block absolute offsets where:
            //   phys = (stored_offset << 8) | (vaddr & 0x1FFF)
            // So: stored_offset = (block_base >> 8) + mb_offset
            if (m_mapMmu) {
                MapState state = {};

                uint32_t loOff = ((m_state.x & 0x0F) << 8) | m_state.a;
                uint32_t hiOff = ((m_state.z & 0x0F) << 8) | m_state.y;

                // Compute per-block absolute offsets
                for (int i = 0; i < 4; i++)
                    state.offsets[i] = (i * 0x20) + loOff;
                for (int i = 4; i < 8; i++)
                    state.offsets[i] = (i * 0x20) + hiOff;

                // Enable bits: X[7:4] for lower half, Z[7:4] for upper half
                state.enables = ((m_state.x >> 4) & 0x0F) | (((m_state.z >> 4) & 0x0F) << 4);

                m_mapMmu->setMapState(state);
            }
            m_state.cycles++;
            break;
        }
        case 0x7C: { // EOM - End of mapping sequence (NOP)
            // EOM does NOT clear map state — it only re-enables interrupts
            // after a MAP instruction. MAP state persists until another MAP or RESET.
            m_state.cycles++;
            break;
        }

        // 16-bit word operations
        case 0xE3: { uint16_t a = getZp(); uint16_t v = (uint16_t)read8(a) | ((uint16_t)read8(a+1) << 8); v++; write8(a, (uint8_t)(v&0xFF)); write8(a+1, (uint8_t)(v>>8)); updateNZ16(v); break; } // INW
        case 0xC3: { uint16_t a = getZp(); uint16_t v = (uint16_t)read8(a) | ((uint16_t)read8(a+1) << 8); v--; write8(a, (uint8_t)(v&0xFF)); write8(a+1, (uint8_t)(v>>8)); updateNZ16(v); break; } // DEW
        case 0xCB: { uint16_t a = getAbs(); uint16_t v = (uint16_t)read8(a) | ((uint16_t)read8(a+1) << 8); m_state.p = (m_state.p & ~FLAG_C) | (v >> 15); v <<= 1; write8(a, (uint8_t)(v&0xFF)); write8(a+1, (uint8_t)(v>>8)); updateNZ16(v); break; } // ASW
        case 0xEB: { uint16_t a = getAbs(); uint16_t v = (uint16_t)read8(a) | ((uint16_t)read8(a+1) << 8); uint16_t c = (m_state.p & FLAG_C) ? 1 : 0; m_state.p = (m_state.p & ~FLAG_C) | (v >> 15); v = (v << 1) | c; write8(a, (uint8_t)(v&0xFF)); write8(a+1, (uint8_t)(v>>8)); updateNZ16(v); break; } // ROW

        default: m_state.haltLine = 1; break;
    }
    return 1;
}

void MOS45GS02::triggerIrq() {
    m_state.irqLine = 1;
}
void MOS45GS02::triggerNmi() {
    m_state.nmiLine = 1;
}

int MOS45GS02::disassembleOne(IBus* bus, uint32_t addr, char* buf, int bufsz) {
    uint32_t currentAddr = addr;
    bool isQuad = false;
    bool is32BitInd = false;

    while (true) {
        uint8_t op = bus->peek8(currentAddr);
        if (op == 0x42 && bus->peek8(currentAddr + 1) == 0x42) {
            isQuad = true;
            currentAddr += 2;
            continue;
        }
        if (op == 0xEA) {
            uint8_t next = bus->peek8(currentAddr + 1);
            if (next == 0x12 || next == 0x32 || next == 0x52 || next == 0x72 ||
                next == 0x92 || next == 0xB2 || next == 0xD2 || next == 0xF2) {
                is32BitInd = true;
                currentAddr++;
                continue;
            }
        }
        break;
    }

    uint8_t op = bus->peek8(currentAddr++);
    const char* prefix = isQuad ? "LDQ" : "LDA";
    const char* s_prefix = isQuad ? "STQ" : "STA";
    const char* adc_pfx = isQuad ? "ADCQ" : "ADC";
    const char* sbc_pfx = isQuad ? "SBCQ" : "SBC";
    const char* cmp_pfx = isQuad ? "CMPQ" : "CMP";
    const char* and_pfx = isQuad ? "ANDQ" : "AND";
    const char* ora_pfx = isQuad ? "ORAQ" : "ORA";
    const char* eor_pfx = isQuad ? "EORQ" : "EOR";
    const char* asl_pfx = isQuad ? "ASLQ" : "ASL";
    const char* lsr_pfx = isQuad ? "LSRQ" : "LSR";
    const char* rol_pfx = isQuad ? "ROLQ" : "ROL";
    const char* ror_pfx = isQuad ? "RORQ" : "ROR";
    const char* bit_pfx = isQuad ? "BITQ" : "BIT";
    const char* inc_pfx = isQuad ? "INQ" : "INC";
    const char* dec_pfx = isQuad ? "DEQ" : "DEC";
    const char* asr_pfx = isQuad ? "ASRQ" : "ASR";
    const char* neg_pfx = isQuad ? "NEGQ" : "NEG";

    switch (op) {
        case 0x00: snprintf(buf, bufsz, "BRK"); return (int)(currentAddr - addr);
        case 0x02: snprintf(buf, bufsz, "CLE"); return (int)(currentAddr - addr);
        case 0x03: snprintf(buf, bufsz, "SEE"); return (int)(currentAddr - addr);
        case 0x0B: snprintf(buf, bufsz, "TSY"); return (int)(currentAddr - addr);
        case 0x2B: snprintf(buf, bufsz, "TYS"); return (int)(currentAddr - addr);
        case 0xAA: snprintf(buf, bufsz, "TAX"); return (int)(currentAddr - addr);
        case 0x8A: snprintf(buf, bufsz, "TXA"); return (int)(currentAddr - addr);
        case 0xA8: snprintf(buf, bufsz, "TAY"); return (int)(currentAddr - addr);
        case 0x98: snprintf(buf, bufsz, "TYA"); return (int)(currentAddr - addr);
        case 0xBA: snprintf(buf, bufsz, "TSX"); return (int)(currentAddr - addr);
        case 0x9A: snprintf(buf, bufsz, "TXS"); return (int)(currentAddr - addr);
        case 0x4B: snprintf(buf, bufsz, "TAZ"); return (int)(currentAddr - addr);
        case 0x6B: snprintf(buf, bufsz, "TZA"); return (int)(currentAddr - addr);
        case 0x5B: snprintf(buf, bufsz, "TAB"); return (int)(currentAddr - addr);
        case 0x7B: snprintf(buf, bufsz, "TBA"); return (int)(currentAddr - addr);
        case 0x18: snprintf(buf, bufsz, "CLC"); return (int)(currentAddr - addr);
        case 0x38: snprintf(buf, bufsz, "SEC"); return (int)(currentAddr - addr);
        case 0x58: snprintf(buf, bufsz, "CLI"); return (int)(currentAddr - addr);
        case 0x78: snprintf(buf, bufsz, "SEI"); return (int)(currentAddr - addr);
        case 0xD8: snprintf(buf, bufsz, "CLD"); return (int)(currentAddr - addr);
        case 0xF8: snprintf(buf, bufsz, "SED"); return (int)(currentAddr - addr);
        case 0xB8: snprintf(buf, bufsz, "CLV"); return (int)(currentAddr - addr);
        case 0x08: snprintf(buf, bufsz, "PHP"); return (int)(currentAddr - addr);
        case 0x28: snprintf(buf, bufsz, "PLP"); return (int)(currentAddr - addr);
        case 0x48: snprintf(buf, bufsz, "PHA"); return (int)(currentAddr - addr);
        case 0x68: snprintf(buf, bufsz, "PLA"); return (int)(currentAddr - addr);
        case 0xDA: snprintf(buf, bufsz, "PHX"); return (int)(currentAddr - addr);
        case 0xFA: snprintf(buf, bufsz, "PLX"); return (int)(currentAddr - addr);
        case 0x5A: snprintf(buf, bufsz, "PHY"); return (int)(currentAddr - addr);
        case 0x7A: snprintf(buf, bufsz, "PLY"); return (int)(currentAddr - addr);
        case 0xDB: snprintf(buf, bufsz, "PHZ"); return (int)(currentAddr - addr);
        case 0xFB: snprintf(buf, bufsz, "PLZ"); return (int)(currentAddr - addr);
        case 0xEA: snprintf(buf, bufsz, "NOP"); return (int)(currentAddr - addr);

        case 0x88: snprintf(buf, bufsz, "DEY"); return (int)(currentAddr - addr);
        case 0xC8: snprintf(buf, bufsz, "INY"); return (int)(currentAddr - addr);
        case 0xCA: snprintf(buf, bufsz, "DEX"); return (int)(currentAddr - addr);
        case 0xE8: snprintf(buf, bufsz, "INX"); return (int)(currentAddr - addr);
        
        case 0x07: case 0x17: case 0x27: case 0x37: case 0x47: case 0x57: case 0x67: case 0x77:
            snprintf(buf, bufsz, "RMB%d $%02X", (op >> 4), bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0x87: case 0x97: case 0xA7: case 0xB7: case 0xC7: case 0xD7: case 0xE7: case 0xF7:
            snprintf(buf, bufsz, "SMB%d $%02X", ((op - 0x87) >> 4), bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0x0F: case 0x1F: case 0x2F: case 0x3F: case 0x4F: case 0x5F: case 0x6F: case 0x7F:
            snprintf(buf, bufsz, "BBR%d $%02X,$%04X", (op >> 4), bus->peek8(currentAddr), (uint16_t)(currentAddr + 2 + (int8_t)bus->peek8(currentAddr+1))); return (int)(currentAddr + 2 - addr);
        case 0x8F: case 0x9F: case 0xAF: case 0xBF: case 0xCF: case 0xDF: case 0xEF: case 0xFF:
            snprintf(buf, bufsz, "BBS%d $%02X,$%04X", ((op - 0x8F) >> 4), bus->peek8(currentAddr), (uint16_t)(currentAddr + 2 + (int8_t)bus->peek8(currentAddr+1))); return (int)(currentAddr + 2 - addr);
        
        case 0xA9:
            if (isQuad) { snprintf(buf, bufsz, "LDQ #$%08X", bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)|(bus->peek8(currentAddr+2)<<16)|(bus->peek8(currentAddr+3)<<24)); return (int)(currentAddr + 4 - addr); }
            snprintf(buf, bufsz, "LDA #$%02X", bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0xAD: snprintf(buf, bufsz, "%s $%04X", prefix, bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)); return (int)(currentAddr + 2 - addr);
        case 0xBD: snprintf(buf, bufsz, "LDA $%04X,X", bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)); return (int)(currentAddr + 2 - addr);
        case 0xB9: snprintf(buf, bufsz, "LDA $%04X,Y", bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)); return (int)(currentAddr + 2 - addr);
        case 0xA5: snprintf(buf, bufsz, "%s $%02X", prefix, bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0xB5: snprintf(buf, bufsz, "%s $%02X,X", prefix, bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0xE2: snprintf(buf, bufsz, "LDA ($%02X,SP),Y", bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0xA1: snprintf(buf, bufsz, "LDA ($%02X,X)", bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0xB1: snprintf(buf, bufsz, "LDA ($%02X),Y", bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0xB2: 
            if (is32BitInd) snprintf(buf, bufsz, "%s [$%02X],Z", prefix, bus->peek8(currentAddr));
            else snprintf(buf, bufsz, "%s ($%02X),Z", prefix, bus->peek8(currentAddr));
            return (int)(currentAddr + 1 - addr);
        
        case 0x8D: snprintf(buf, bufsz, "%s $%04X", s_prefix, bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)); return (int)(currentAddr + 2 - addr);
        case 0x9D: snprintf(buf, bufsz, "STA $%04X,X", bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)); return (int)(currentAddr + 2 - addr);
        case 0x99: snprintf(buf, bufsz, "STA $%04X,Y", bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)); return (int)(currentAddr + 2 - addr);
        case 0x85: snprintf(buf, bufsz, "%s $%02X", s_prefix, bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0x95: snprintf(buf, bufsz, "%s $%02X,X", s_prefix, bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0x82: snprintf(buf, bufsz, "STA ($%02X,SP),Y", bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0x81: snprintf(buf, bufsz, "STA ($%02X,X)", bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0x91: snprintf(buf, bufsz, "STA ($%02X),Y", bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0x92:
            if (is32BitInd) snprintf(buf, bufsz, "%s [$%02X],Z", s_prefix, bus->peek8(currentAddr));
            else snprintf(buf, bufsz, "%s ($%02X),Z", s_prefix, bus->peek8(currentAddr));
            return (int)(currentAddr + 1 - addr);

        case 0x69:
            if (isQuad) { snprintf(buf, bufsz, "ADCQ #$%08X", bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)|(bus->peek8(currentAddr+2)<<16)|(bus->peek8(currentAddr+3)<<24)); return (int)(currentAddr + 4 - addr); }
            snprintf(buf, bufsz, "ADC #$%02X", bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0x6D: snprintf(buf, bufsz, "%s $%04X", adc_pfx, bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)); return (int)(currentAddr + 2 - addr);
        case 0x7D: snprintf(buf, bufsz, "%s $%04X,X", adc_pfx, bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)); return (int)(currentAddr + 2 - addr);
        case 0x79: snprintf(buf, bufsz, "%s $%04X,Y", adc_pfx, bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)); return (int)(currentAddr + 2 - addr);
        case 0x65: snprintf(buf, bufsz, "%s $%02X", adc_pfx, bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0x75: snprintf(buf, bufsz, "%s $%02X,X", adc_pfx, bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0x61: snprintf(buf, bufsz, "ADC ($%02X,X)", bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0x71: snprintf(buf, bufsz, "ADC ($%02X),Y", bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0x72: 
            if (is32BitInd) snprintf(buf, bufsz, "%s [$%02X],Z", adc_pfx, bus->peek8(currentAddr));
            else snprintf(buf, bufsz, "%s ($%02X),Z", adc_pfx, bus->peek8(currentAddr));
            return (int)(currentAddr + 1 - addr);

        case 0xE9:
            if (isQuad) { snprintf(buf, bufsz, "SBCQ #$%08X", bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)|(bus->peek8(currentAddr+2)<<16)|(bus->peek8(currentAddr+3)<<24)); return (int)(currentAddr + 4 - addr); }
            snprintf(buf, bufsz, "SBC #$%02X", bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0xED: snprintf(buf, bufsz, "%s $%04X", sbc_pfx, bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)); return (int)(currentAddr + 2 - addr);
        case 0xFD: snprintf(buf, bufsz, "%s $%04X,X", sbc_pfx, bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)); return (int)(currentAddr + 2 - addr);
        case 0xF9: snprintf(buf, bufsz, "%s $%04X,Y", sbc_pfx, bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)); return (int)(currentAddr + 2 - addr);
        case 0xE5: snprintf(buf, bufsz, "%s $%02X", sbc_pfx, bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0xF5: snprintf(buf, bufsz, "%s $%02X,X", sbc_pfx, bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0xE1: snprintf(buf, bufsz, "SBC ($%02X,X)", bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0xF1: snprintf(buf, bufsz, "SBC ($%02X),Y", bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0xF2:
            if (is32BitInd) snprintf(buf, bufsz, "%s [$%02X],Z", sbc_pfx, bus->peek8(currentAddr));
            else snprintf(buf, bufsz, "%s ($%02X),Z", sbc_pfx, bus->peek8(currentAddr));
            return (int)(currentAddr + 1 - addr);

        case 0xC9:
            if (isQuad) { snprintf(buf, bufsz, "CMPQ #$%08X", bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)|(bus->peek8(currentAddr+2)<<16)|(bus->peek8(currentAddr+3)<<24)); return (int)(currentAddr + 4 - addr); }
            snprintf(buf, bufsz, "CMP #$%02X", bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0xCD: snprintf(buf, bufsz, "%s $%04X", cmp_pfx, bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)); return (int)(currentAddr + 2 - addr);
        case 0xDD: snprintf(buf, bufsz, "%s $%04X,X", cmp_pfx, bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)); return (int)(currentAddr + 2 - addr);
        case 0xD9: snprintf(buf, bufsz, "%s $%04X,Y", cmp_pfx, bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)); return (int)(currentAddr + 2 - addr);
        case 0xC5: snprintf(buf, bufsz, "%s $%02X", cmp_pfx, bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0xD5: snprintf(buf, bufsz, "%s $%02X,X", cmp_pfx, bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0xC1: snprintf(buf, bufsz, "CMP ($%02X,X)", bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0xD1: snprintf(buf, bufsz, "CMP ($%02X),Y", bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0xD2:
            if (is32BitInd) snprintf(buf, bufsz, "%s [$%02X],Z", cmp_pfx, bus->peek8(currentAddr));
            else snprintf(buf, bufsz, "%s ($%02X),Z", cmp_pfx, bus->peek8(currentAddr));
            return (int)(currentAddr + 1 - addr);

        case 0x29:
            if (isQuad) { snprintf(buf, bufsz, "ANDQ #$%08X", bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)|(bus->peek8(currentAddr+2)<<16)|(bus->peek8(currentAddr+3)<<24)); return (int)(currentAddr + 4 - addr); }
            snprintf(buf, bufsz, "AND #$%02X", bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0x2D: snprintf(buf, bufsz, "%s $%04X", and_pfx, bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)); return (int)(currentAddr + 2 - addr);
        case 0x3D: snprintf(buf, bufsz, "%s $%04X,X", and_pfx, bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)); return (int)(currentAddr + 2 - addr);
        case 0x39: snprintf(buf, bufsz, "%s $%04X,Y", and_pfx, bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)); return (int)(currentAddr + 2 - addr);
        case 0x25: snprintf(buf, bufsz, "%s $%02X", and_pfx, bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0x35: snprintf(buf, bufsz, "%s $%02X,X", and_pfx, bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0x21: snprintf(buf, bufsz, "AND ($%02X,X)", bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0x31: snprintf(buf, bufsz, "AND ($%02X),Y", bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0x32:
            if (is32BitInd) snprintf(buf, bufsz, "%s [$%02X],Z", and_pfx, bus->peek8(currentAddr));
            else snprintf(buf, bufsz, "%s ($%02X),Z", and_pfx, bus->peek8(currentAddr));
            return (int)(currentAddr + 1 - addr);

        case 0x09:
            if (isQuad) { snprintf(buf, bufsz, "ORAQ #$%08X", bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)|(bus->peek8(currentAddr+2)<<16)|(bus->peek8(currentAddr+3)<<24)); return (int)(currentAddr + 4 - addr); }
            snprintf(buf, bufsz, "ORA #$%02X", bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0x0D: snprintf(buf, bufsz, "%s $%04X", ora_pfx, bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)); return (int)(currentAddr + 2 - addr);
        case 0x1D: snprintf(buf, bufsz, "%s $%04X,X", ora_pfx, bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)); return (int)(currentAddr + 2 - addr);
        case 0x19: snprintf(buf, bufsz, "%s $%04X,Y", ora_pfx, bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)); return (int)(currentAddr + 2 - addr);
        case 0x05: snprintf(buf, bufsz, "%s $%02X", ora_pfx, bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0x15: snprintf(buf, bufsz, "%s $%02X,X", ora_pfx, bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0x01: snprintf(buf, bufsz, "ORA ($%02X,X)", bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0x11: snprintf(buf, bufsz, "ORA ($%02X),Y", bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0x12:
            if (is32BitInd) snprintf(buf, bufsz, "%s [$%02X],Z", ora_pfx, bus->peek8(currentAddr));
            else snprintf(buf, bufsz, "%s ($%02X),Z", ora_pfx, bus->peek8(currentAddr));
            return (int)(currentAddr + 1 - addr);

        case 0x49:
            if (isQuad) { snprintf(buf, bufsz, "EORQ #$%08X", bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)|(bus->peek8(currentAddr+2)<<16)|(bus->peek8(currentAddr+3)<<24)); return (int)(currentAddr + 4 - addr); }
            snprintf(buf, bufsz, "EOR #$%02X", bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0x4D: snprintf(buf, bufsz, "%s $%04X", eor_pfx, bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)); return (int)(currentAddr + 2 - addr);
        case 0x5D: snprintf(buf, bufsz, "%s $%04X,X", eor_pfx, bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)); return (int)(currentAddr + 2 - addr);
        case 0x59: snprintf(buf, bufsz, "%s $%04X,Y", eor_pfx, bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)); return (int)(currentAddr + 2 - addr);
        case 0x45: snprintf(buf, bufsz, "%s $%02X", eor_pfx, bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0x55: snprintf(buf, bufsz, "%s $%02X,X", eor_pfx, bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0x41: snprintf(buf, bufsz, "EOR ($%02X,X)", bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0x51: snprintf(buf, bufsz, "EOR ($%02X),Y", bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0x52:
            if (is32BitInd) snprintf(buf, bufsz, "%s [$%02X],Z", eor_pfx, bus->peek8(currentAddr));
            else snprintf(buf, bufsz, "%s ($%02X),Z", eor_pfx, bus->peek8(currentAddr));
            return (int)(currentAddr + 1 - addr);

        case 0x0A: snprintf(buf, bufsz, "%s A", asl_pfx); return (int)(currentAddr - addr);
        case 0x0E: snprintf(buf, bufsz, "%s $%04X", asl_pfx, bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)); return (int)(currentAddr + 2 - addr);
        case 0x1E: snprintf(buf, bufsz, "%s $%04X,X", asl_pfx, bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)); return (int)(currentAddr + 2 - addr);
        case 0x06: snprintf(buf, bufsz, "%s $%02X", asl_pfx, bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0x16: snprintf(buf, bufsz, "%s $%02X,X", asl_pfx, bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);

        case 0x4A: snprintf(buf, bufsz, "%s A", lsr_pfx); return (int)(currentAddr - addr);
        case 0x4E: snprintf(buf, bufsz, "%s $%04X", lsr_pfx, bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)); return (int)(currentAddr + 2 - addr);
        case 0x5E: snprintf(buf, bufsz, "%s $%04X,X", lsr_pfx, bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)); return (int)(currentAddr + 2 - addr);
        case 0x46: snprintf(buf, bufsz, "%s $%02X", lsr_pfx, bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0x56: snprintf(buf, bufsz, "%s $%02X,X", lsr_pfx, bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);

        case 0x2A: snprintf(buf, bufsz, "%s A", rol_pfx); return (int)(currentAddr - addr);
        case 0x2E: snprintf(buf, bufsz, "%s $%04X", rol_pfx, bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)); return (int)(currentAddr + 2 - addr);
        case 0x3E: snprintf(buf, bufsz, "%s $%04X,X", rol_pfx, bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)); return (int)(currentAddr + 2 - addr);
        case 0x26: snprintf(buf, bufsz, "%s $%02X", rol_pfx, bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0x36: snprintf(buf, bufsz, "%s $%02X,X", rol_pfx, bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);

        case 0x6A: snprintf(buf, bufsz, "%s A", ror_pfx); return (int)(currentAddr - addr);
        case 0x6E: snprintf(buf, bufsz, "%s $%04X", ror_pfx, bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)); return (int)(currentAddr + 2 - addr);
        case 0x7E: snprintf(buf, bufsz, "%s $%04X,X", ror_pfx, bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)); return (int)(currentAddr + 2 - addr);
        case 0x66: snprintf(buf, bufsz, "%s $%02X", ror_pfx, bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0x76: snprintf(buf, bufsz, "%s $%02X,X", ror_pfx, bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);

        case 0x04: snprintf(buf, bufsz, "TSB $%02X", bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0x0C: snprintf(buf, bufsz, "TSB $%04X", bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)); return (int)(currentAddr + 2 - addr);
        case 0x14: snprintf(buf, bufsz, "TRB $%02X", bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0x1C: snprintf(buf, bufsz, "TRB $%04X", bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)); return (int)(currentAddr + 2 - addr);
        case 0x43: snprintf(buf, bufsz, "%s A", asr_pfx); return (int)(currentAddr - addr);
        case 0x44: snprintf(buf, bufsz, "%s $%02X", asr_pfx, bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0x54: snprintf(buf, bufsz, "%s $%02X,X", asr_pfx, bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);

        // LDX
        case 0xA2: snprintf(buf, bufsz, "LDX #$%02X", bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0xAE: snprintf(buf, bufsz, "LDX $%04X", bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)); return (int)(currentAddr + 2 - addr);
        case 0xBE: snprintf(buf, bufsz, "LDX $%04X,Y", bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)); return (int)(currentAddr + 2 - addr);
        case 0xA6: snprintf(buf, bufsz, "LDX $%02X", bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0xB6: snprintf(buf, bufsz, "LDX $%02X,Y", bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);

        // LDY
        case 0xA0: snprintf(buf, bufsz, "LDY #$%02X", bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0xAC: snprintf(buf, bufsz, "LDY $%04X", bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)); return (int)(currentAddr + 2 - addr);
        case 0xBC: snprintf(buf, bufsz, "LDY $%04X,X", bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)); return (int)(currentAddr + 2 - addr);
        case 0xA4: snprintf(buf, bufsz, "LDY $%02X", bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0xB4: snprintf(buf, bufsz, "LDY $%02X,X", bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);

        // LDZ
        case 0xA3: snprintf(buf, bufsz, "LDZ #$%02X", bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0xAB: snprintf(buf, bufsz, "LDZ $%04X", bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)); return (int)(currentAddr + 2 - addr);
        case 0xBB: snprintf(buf, bufsz, "LDZ $%04X,X", bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)); return (int)(currentAddr + 2 - addr);

        // STX, STY, STZ
        case 0x8E: snprintf(buf, bufsz, "STX $%04X", bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)); return (int)(currentAddr + 2 - addr);
        case 0x86: snprintf(buf, bufsz, "STX $%02X", bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0x96: snprintf(buf, bufsz, "STX $%02X,Y", bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0x9B: snprintf(buf, bufsz, "STX $%04X,Y", bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)); return (int)(currentAddr + 2 - addr);
        case 0x8C: snprintf(buf, bufsz, "STY $%04X", bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)); return (int)(currentAddr + 2 - addr);
        case 0x84: snprintf(buf, bufsz, "STY $%02X", bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0x94: snprintf(buf, bufsz, "STY $%02X,X", bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0x9C: snprintf(buf, bufsz, "STZ $%04X", bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)); return (int)(currentAddr + 2 - addr);
        case 0x64: snprintf(buf, bufsz, "STZ $%02X", bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0x74: snprintf(buf, bufsz, "STZ $%02X,X", bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0x9E: snprintf(buf, bufsz, "STZ $%04X,X", bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)); return (int)(currentAddr + 2 - addr);

        case 0xE0: snprintf(buf, bufsz, "CPX #$%02X", bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0xEC: snprintf(buf, bufsz, "CPX $%04X", bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)); return (int)(currentAddr + 2 - addr);
        case 0xE4: snprintf(buf, bufsz, "CPX $%02X", bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0xC0: snprintf(buf, bufsz, "CPY #$%02X", bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0xCC: snprintf(buf, bufsz, "CPY $%04X", bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)); return (int)(currentAddr + 2 - addr);
        case 0xC4: snprintf(buf, bufsz, "CPY $%02X", bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0xC2: snprintf(buf, bufsz, "CPZ #$%02X", bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0xD4: snprintf(buf, bufsz, "CPZ $%02X", bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0xDC: snprintf(buf, bufsz, "CPZ $%04X", bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)); return (int)(currentAddr + 2 - addr);

        case 0xE3: snprintf(buf, bufsz, "INW $%02X", bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0xC3: snprintf(buf, bufsz, "DEW $%02X", bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0xEE: snprintf(buf, bufsz, "%s $%04X", inc_pfx, bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)); return (int)(currentAddr + 2 - addr);
        case 0xFE: snprintf(buf, bufsz, "%s $%04X,X", inc_pfx, bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)); return (int)(currentAddr + 2 - addr);
        case 0xE6: snprintf(buf, bufsz, "%s $%02X", inc_pfx, bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0xF6: snprintf(buf, bufsz, "%s $%02X,X", inc_pfx, bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0xCE: snprintf(buf, bufsz, "%s $%04X", dec_pfx, bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)); return (int)(currentAddr + 2 - addr);
        case 0xDE: snprintf(buf, bufsz, "%s $%04X,X", dec_pfx, bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)); return (int)(currentAddr + 2 - addr);
        case 0xC6: snprintf(buf, bufsz, "%s $%02X", dec_pfx, bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0xD6: snprintf(buf, bufsz, "%s $%02X,X", dec_pfx, bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);

        case 0x1B: snprintf(buf, bufsz, "INZ"); return (int)(currentAddr - addr);
        case 0x3B: snprintf(buf, bufsz, "DEZ"); return (int)(currentAddr - addr);

        case 0x1A: snprintf(buf, bufsz, "%s A", inc_pfx); return (int)(currentAddr - addr);
        case 0x3A: snprintf(buf, bufsz, "%s A", dec_pfx); return (int)(currentAddr - addr);

        case 0x20: snprintf(buf, bufsz, "JSR $%04X", bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)); return (int)(currentAddr + 2 - addr);
        case 0x22: snprintf(buf, bufsz, "JSR ($%04X)", bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)); return (int)(currentAddr + 2 - addr);
        case 0x23: snprintf(buf, bufsz, "JSR ($%04X,X)", bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)); return (int)(currentAddr + 2 - addr);
        case 0x4C: snprintf(buf, bufsz, "JMP $%04X", bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)); return (int)(currentAddr + 2 - addr);
        case 0x6C: snprintf(buf, bufsz, "JMP ($%04X)", bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)); return (int)(currentAddr + 2 - addr);
        case 0x60: snprintf(buf, bufsz, "RTS"); return (int)(currentAddr - addr);
        case 0x40: snprintf(buf, bufsz, "RTI"); return (int)(currentAddr - addr);

        case 0x63: { int16_t r=(int16_t)bus->peek8(currentAddr)|((int16_t)bus->peek8(currentAddr+1)<<8); snprintf(buf, bufsz, "BSR $%04X", (uint16_t)(currentAddr + 2 + r)); return (int)(currentAddr + 2 - addr); }
        case 0x62: snprintf(buf, bufsz, "RTN #$%02X", bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);

        case 0x80: snprintf(buf, bufsz, "BRA $%04X", (uint16_t)(currentAddr + 1 + (int8_t)bus->peek8(currentAddr))); return (int)(currentAddr + 1 - addr);
        case 0xF0: snprintf(buf, bufsz, "BEQ $%04X", (uint16_t)(currentAddr + 1 + (int8_t)bus->peek8(currentAddr))); return (int)(currentAddr + 1 - addr);
        case 0xD0: snprintf(buf, bufsz, "BNE $%04X", (uint16_t)(currentAddr + 1 + (int8_t)bus->peek8(currentAddr))); return (int)(currentAddr + 1 - addr);
        case 0xB0: snprintf(buf, bufsz, "BCS $%04X", (uint16_t)(currentAddr + 1 + (int8_t)bus->peek8(currentAddr))); return (int)(currentAddr + 1 - addr);
        case 0x90: snprintf(buf, bufsz, "BCC $%04X", (uint16_t)(currentAddr + 1 + (int8_t)bus->peek8(currentAddr))); return (int)(currentAddr + 1 - addr);
        case 0x10: snprintf(buf, bufsz, "BPL $%04X", (uint16_t)(currentAddr + 1 + (int8_t)bus->peek8(currentAddr))); return (int)(currentAddr + 1 - addr);
        case 0x30: snprintf(buf, bufsz, "BMI $%04X", (uint16_t)(currentAddr + 1 + (int8_t)bus->peek8(currentAddr))); return (int)(currentAddr + 1 - addr);
        case 0x50: snprintf(buf, bufsz, "BVC $%04X", (uint16_t)(currentAddr + 1 + (int8_t)bus->peek8(currentAddr))); return (int)(currentAddr + 1 - addr);
        case 0x70: snprintf(buf, bufsz, "BVS $%04X", (uint16_t)(currentAddr + 1 + (int8_t)bus->peek8(currentAddr))); return (int)(currentAddr + 1 - addr);

        case 0x42: snprintf(buf, bufsz, "%s", neg_pfx); return (int)(currentAddr - addr);
        case 0x5C: snprintf(buf, bufsz, "MAP"); return (int)(currentAddr - addr);
        case 0x7C: snprintf(buf, bufsz, "EOM"); return (int)(currentAddr - addr);

        case 0x89: snprintf(buf, bufsz, "BIT #$%02X", bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0x24: snprintf(buf, bufsz, "%s $%02X", bit_pfx, bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0x2C: snprintf(buf, bufsz, "%s $%04X", bit_pfx, bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)); return (int)(currentAddr + 2 - addr);
        case 0x34: snprintf(buf, bufsz, "BIT $%02X,X", bus->peek8(currentAddr)); return (int)(currentAddr + 1 - addr);
        case 0x3C: snprintf(buf, bufsz, "BIT $%04X,X", bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)); return (int)(currentAddr + 2 - addr);

        case 0x83: { int16_t r=(int16_t)bus->peek8(currentAddr)|((int16_t)bus->peek8(currentAddr+1)<<8); snprintf(buf, bufsz, "LBRA $%04X", (uint16_t)(currentAddr + 2 + r)); return (int)(currentAddr + 2 - addr); }
        case 0xD3: { int16_t r=(int16_t)bus->peek8(currentAddr)|((int16_t)bus->peek8(currentAddr+1)<<8); snprintf(buf, bufsz, "LBNE $%04X", (uint16_t)(currentAddr + 2 + r)); return (int)(currentAddr + 2 - addr); }
        case 0xF3: { int16_t r=(int16_t)bus->peek8(currentAddr)|((int16_t)bus->peek8(currentAddr+1)<<8); snprintf(buf, bufsz, "LBEQ $%04X", (uint16_t)(currentAddr + 2 + r)); return (int)(currentAddr + 2 - addr); }
        case 0x93: { int16_t r=(int16_t)bus->peek8(currentAddr)|((int16_t)bus->peek8(currentAddr+1)<<8); snprintf(buf, bufsz, "LBCC $%04X", (uint16_t)(currentAddr + 2 + r)); return (int)(currentAddr + 2 - addr); }
        case 0xB3: { int16_t r=(int16_t)bus->peek8(currentAddr)|((int16_t)bus->peek8(currentAddr+1)<<8); snprintf(buf, bufsz, "LBCS $%04X", (uint16_t)(currentAddr + 2 + r)); return (int)(currentAddr + 2 - addr); }
        case 0x13: { int16_t r=(int16_t)bus->peek8(currentAddr)|((int16_t)bus->peek8(currentAddr+1)<<8); snprintf(buf, bufsz, "LBPL $%04X", (uint16_t)(currentAddr + 2 + r)); return (int)(currentAddr + 2 - addr); }
        case 0x33: { int16_t r=(int16_t)bus->peek8(currentAddr)|((int16_t)bus->peek8(currentAddr+1)<<8); snprintf(buf, bufsz, "LBMI $%04X", (uint16_t)(currentAddr + 2 + r)); return (int)(currentAddr + 2 - addr); }
        case 0x53: { int16_t r=(int16_t)bus->peek8(currentAddr)|((int16_t)bus->peek8(currentAddr+1)<<8); snprintf(buf, bufsz, "LBVC $%04X", (uint16_t)(currentAddr + 2 + r)); return (int)(currentAddr + 2 - addr); }
        case 0x73: { int16_t r=(int16_t)bus->peek8(currentAddr)|((int16_t)bus->peek8(currentAddr+1)<<8); snprintf(buf, bufsz, "LBVS $%04X", (uint16_t)(currentAddr + 2 + r)); return (int)(currentAddr + 2 - addr); }

        case 0xF4: snprintf(buf, bufsz, "PHW #$%04X", bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)); return (int)(currentAddr + 2 - addr);
        case 0xFC: snprintf(buf, bufsz, "PHW $%04X", bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)); return (int)(currentAddr + 2 - addr);

        case 0xCB: snprintf(buf, bufsz, "ASW $%04X", bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)); return (int)(currentAddr + 2 - addr);
        case 0xEB: snprintf(buf, bufsz, "ROW $%04X", bus->peek8(currentAddr)|(bus->peek8(currentAddr+1)<<8)); return (int)(currentAddr + 2 - addr);

        default: snprintf(buf, bufsz, "??? ($%02X)", op); return (int)(currentAddr - addr);
    }
}

int MOS45GS02::disassembleEntry(IBus* bus, uint32_t addr, void* entryOut) {
    DisasmEntry* e = (DisasmEntry*)entryOut; e->addr = addr;
    static char s_buf[128]; // Static buffer to avoid dangling pointer, okay for single-threaded simulator
    e->bytes = disassembleOne(bus, addr, s_buf, sizeof(s_buf)); e->complete = s_buf;
    uint8_t op = bus->peek8(addr); 
    e->isCall = (op==0x20 || op==0x63 || op==0x22 || op==0x23); e->isReturn = (op==0x60||op==0x40||op==0x62);
    e->isBranch = (op&0x1F)==0x10 || (op==0x80) || (op==0x83) || (op&0x1F)==0x13; return e->bytes;
}

void MOS45GS02::saveState(uint8_t* buf) const { memcpy(buf, &m_state, sizeof(m_state)); }
void MOS45GS02::loadState(const uint8_t* buf) { memcpy(&m_state, buf, sizeof(m_state)); }
int MOS45GS02::isCallAt(IBus* bus, uint32_t addr) { uint8_t op = bus->peek8(addr); return (op == 0x20 || op == 0x22 || op == 0x23) ? 3 : 0; }
bool MOS45GS02::isReturnAt(IBus* bus, uint32_t addr) { uint8_t op = bus->peek8(addr); return op == 0x60 || op == 0x40 || op == 0x62; }
bool MOS45GS02::isProgramEnd(IBus* bus) {
    if (m_state.haltLine) return true;
    if (bus && bus->isHaltRequested()) return true;

    // Check for RTS/RTI/RTN on empty stack (SP at initial value)
    if (m_state.sp == 0x01FF && bus) {
        uint8_t op = bus->peek8(m_state.pc);
        if (op == 0x60 || op == 0x40 || op == 0x62) return true; // RTS, RTI, RTN
    }

    // Check for JMP * (infinite loop at current PC)
    if (bus) {
        uint32_t addr = m_state.pc;
        // Skip prefixes
        while (true) {
            uint8_t op = bus->peek8(addr);
            if (op == 0x42 && bus->peek8(addr + 1) == 0x42) { addr += 2; continue; }
            if (op == 0xEA) {
                uint8_t next = bus->peek8(addr + 1);
                if (next == 0xB2 || next == 0x92 || next == 0x12) { addr++; continue; }
            }
            break;
        }
        uint8_t op = bus->peek8(addr);
        if (op == 0x4C) { // JMP abs
            uint16_t target = bus->peek8(addr + 1) | (bus->peek8(addr + 2) << 8);
            if (target == m_state.pc) return true;
        }
    }

    return false;
}
int MOS45GS02::writeCallHarness(IBus* bus, uint32_t scratch, uint32_t routineAddr) {
    bus->write8(scratch, 0x20); bus->write8(scratch+1, routineAddr&0xFF); bus->write8(scratch+2, routineAddr>>8); bus->write8(scratch+3, 0x00); return 4;
}
