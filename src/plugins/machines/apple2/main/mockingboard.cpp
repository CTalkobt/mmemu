#include "mockingboard.h"
#include <cstdio>

namespace apple2 {

namespace { constexpr unsigned AY_STEP = 16; }     // AY clocks (= CPU cycles) a step

Mockingboard::Mockingboard(int slot) : m_slot(slot) {
    m_name = "Mockingboard (slot " + std::to_string(slot) + ")";
    for (int i = 0; i < 2; ++i) {
        m_via[i].setName(i ? "VIA2" : "VIA1");
        m_via[i].setBaseAddr(0);
        m_via[i].setIrqLine(&m_irq[i]);
        m_bus[i].mb = this;
        m_bus[i].chip = i;
        m_via[i].setPortADevice(&m_bus[i]);
    }
    reset();
}

void Mockingboard::reset() {
    for (int i = 0; i < 2; ++i) {
        m_via[i].reset();
        m_ay[i].reset();
        m_reading[i] = false;
        m_irq[i].level = false;
    }
    m_ayTime = 0;                                   // the bus's cycles start again at power-on
    m_sum = 0;
    m_count = 0;
    m_last = 0;
}

void Mockingboard::tick(uint64_t cycles) {
    m_via[0].tick(cycles);
    m_via[1].tick(cycles);
}

// VIA registers: ORB 0, ORA 1, DDRB 2, DDRA 3
// The AY's data bus: port A's output latch where DDRA is 1, pulled high elsewhere
uint8_t Mockingboard::portA(int chip) {
    uint8_t ddra = 0;
    m_via[chip].ioPeek(nullptr, 0x3, &ddra);
    return (uint8_t)((m_oraLatch[chip] & ddra) | (uint8_t)~ddra);
}

void Mockingboard::control(int chip, uint8_t pb, uint64_t at) {
    uint8_t ddrb = 0;
    m_via[chip].ioPeek(nullptr, 0x2, &ddrb);
    const uint8_t pins = (uint8_t)((pb & ddrb) | (uint8_t)~ddrb);
    m_reading[chip] = false;
    if (!(pins & 4)) {                              // /RESET
        sync(at);
        m_ay[chip].reset();
        return;
    }
    switch (pins & 3) {
        case 1: m_reading[chip] = true; break;     // read
        case 2: sync(at); m_ay[chip].write(portA(chip)); break;
        case 3: m_ay[chip].selectRegister(portA(chip) & 0x0F); break;
        default: break;                             // inactive
    }
}

uint8_t Mockingboard::read(uint16_t addr, uint64_t) {
    uint8_t v = 0xFF;
    m_via[(addr >> 7) & 1].ioRead(nullptr, addr & 0x0F, &v);
    return v;
}

uint8_t Mockingboard::peek(uint16_t addr) {
    uint8_t v = 0xFF;
    m_via[(addr >> 7) & 1].ioPeek(nullptr, addr & 0x0F, &v);
    return v;
}

void Mockingboard::write(uint16_t addr, uint8_t v, uint64_t at) {
    const int chip = (addr >> 7) & 1;
    const int reg = addr & 0x0F;
    m_via[chip].ioWrite(nullptr, (uint32_t)reg, v);
    if (reg == 0x1 || reg == 0xF) m_oraLatch[chip] = v;
    if (reg == 0x0) control(chip, v, at);         // ORB: the AY's BC1, BDIR, /RESET
}

void Mockingboard::sync(uint64_t to) {
    while (m_ayTime + AY_STEP <= to) {
        m_ayTime += AY_STEP;
        m_sum += (double)(m_ay[0].step() + m_ay[1].step());
        ++m_count;
    }
}

float Mockingboard::sample(uint64_t until) {
    sync(until);
    if (m_count) {
        m_last = (float)(m_sum / m_count);          // both chips: six voices at most ~1.4
        m_sum = 0;
        m_count = 0;
    }
    return m_last;
}

void Mockingboard::getDeviceInfo(DeviceInfo& out) const {
    out.name = m_name;
    out.baseAddr = 0xC000u + (uint32_t)m_slot * 0x100u;
    out.addrMask = 0xFF;
    for (int c = 0; c < 2; ++c)
        for (int r = 0; r < 14; ++r) {
            char n[16];
            std::snprintf(n, sizeof n, "AY%d_R%d", c + 1, r);
            out.registers.push_back({n, (uint32_t)(c * 16 + r), m_ay[c].reg(r), "", {}});
        }
    out.state.push_back({"IRQ", irq() ? "asserted" : "clear"});
}

}  // namespace apple2
