// MOS 6532 RIOT, after Stella (src/emucore/M6532.cxx), GPL-2.0-or-later,
// Stella team.

#include "riot6532.h"
#include <cstring>

Riot6532::Riot6532() { Riot6532::reset(); }

void Riot6532::reset() {
    std::memset(m_ram, 0, sizeof m_ram);
    m_timer = 0xAA;                             // Stella: random; fixed here for repeatable runs
    m_divider = 1024;
    m_shift = 10;
    m_subTimer = 0;
    m_wrappedThisCycle = false;
    m_ddrA = m_ddrB = m_outA = m_outB = 0;
    m_flags = 0;
    m_edgePositive = false;
    m_pa7Sync = m_pa7Stable = true;
}

bool Riot6532::pa7() const {
    if (m_ddrA & 0x80) return m_outA & 0x80;
    return portAPins() & 0x80;
}

// M6532::updateEmulation
void Riot6532::sync(uint64_t cpuCycle) {
    if (cpuCycle <= m_lastCycle) return;
    uint32_t cycles = (uint32_t)(cpuCycle - m_lastCycle);
    const uint32_t sub = m_subTimer;
    m_wrappedThisCycle = false;
    m_subTimer = (cycles + m_subTimer) & (m_divider - 1);
    if (!(m_flags & TIMER_BIT)) {
        const uint32_t ticks = (cycles + sub) >> m_shift;
        if (ticks > m_timer) {
            cycles -= (((uint32_t)m_timer + 1) << m_shift) - sub;
            m_wrappedThisCycle = cycles == 0;
            m_timer = 0xFF;
            m_flags |= TIMER_BIT;
        } else {
            m_timer = (uint8_t)(m_timer - ticks);
            cycles = 0;
        }
    }
    if (m_flags & TIMER_BIT) {
        m_timer = (uint8_t)(m_timer - cycles);
        m_wrappedThisCycle = m_timer == 0xFF;
    }
    // PA7 edge detect (two-stage synchroniser)
    const bool stable = m_pa7Sync;
    m_pa7Sync = pa7();
    if (m_pa7Stable != stable && stable == m_edgePositive) m_flags |= PA7_BIT;
    m_pa7Stable = stable;
    m_lastCycle = cpuCycle;
}

uint8_t Riot6532::peek(uint16_t addr) const {
    if (!(addr & 0x0200)) return m_ram[addr & 0x7F];
    switch (addr & 0x07) {
        case 0: return (uint8_t)((m_outA | ~m_ddrA) & portAPins());
        case 1: return m_ddrA;
        case 2: return (uint8_t)((m_outB | ~m_ddrB) & ((m_portB ? m_portB() : 0xFF) | m_ddrB));
        case 3: return m_ddrB;
        case 4: case 6: return m_timer;
        default: return m_flags;
    }
}

uint8_t Riot6532::read(uint16_t addr) {
    const uint8_t v = peek(addr);
    if (addr & 0x0200) {
        switch (addr & 0x07) {
            case 4: case 6:                     // INTIM: clears the timer flag
                if (!m_wrappedThisCycle) m_flags &= (uint8_t)~TIMER_BIT;
                break;
            case 5: case 7:                     // the flags: PA7 cleared
                m_flags &= (uint8_t)~PA7_BIT;
                break;
            default: break;
        }
    }
    return v;
}

void Riot6532::setTimer(uint8_t value, int interval) {
    static const uint32_t DIV[4] = {1, 8, 64, 1024}, SHIFT[4] = {0, 3, 6, 10};
    m_divider = DIV[interval];
    m_shift = SHIFT[interval];
    m_timer = value;
    m_subTimer = m_divider - 1;
    if (!m_wrappedThisCycle) m_flags &= (uint8_t)~TIMER_BIT;
}

void Riot6532::write(uint16_t addr, uint8_t value) {
    if (!(addr & 0x0200)) { m_ram[addr & 0x7F] = value; return; }
    if (addr & 0x04) {
        if (addr & 0x10) setTimer(value, addr & 0x03);
        else m_edgePositive = addr & 0x01;
        return;
    }
    switch (addr & 0x03) {
        case 0: m_outA = value; break;
        case 1: m_ddrA = value; break;
        case 2: m_outB = value; break;
        default: m_ddrB = value; break;
    }
}
