#include "via6522.h"
#include <cstring>

VIA6522::VIA6522(const std::string& name, uint32_t baseAddr)
    : m_name(name), m_baseAddr(baseAddr)
{
    m_ca1Conduit.m_via = this;
    m_cb1Conduit.m_via = this;
    m_ca1Line = &m_ca1Conduit;
    m_ca2Line = &m_ca2Conduit;
    m_cb1Line = &m_cb1Conduit;
    m_cb2Line = &m_cb2Conduit;
    m_pb7Proxy.m_reg = &m_regs[ORB];
    m_pb7Proxy.m_bit = 7;
    for (int i = 0; i < 7; ++i) { m_pbProxy[i].m_reg = &m_regs[ORB]; m_pbProxy[i].m_bit = i; }
    VIA6522::reset();
}

void VIA6522::CA1Conduit::set(bool level) {
    bool prev = m_level;
    m_level = level;
    if (!m_via) return;
    bool activeEdge = (m_via->m_regs[VIA6522::PCR] & 0x01)
        ? (!prev && level)   // positive edge
        : (prev && !level);  // negative edge
    if (activeEdge) {
        m_via->m_regs[VIA6522::IFR] |= 0x02;
        if ((m_via->m_regs[VIA6522::PCR] & 0x0E) == 0x08) m_via->driveCA2(true); // handshake release
        m_via->updateIrq();
    }
    m_via->m_ca1Prev = level;
}

void VIA6522::CB1Conduit::set(bool level) {
    bool prev = m_level;
    m_level = level;
    if (!m_via) return;
    bool activeEdge = (m_via->m_regs[VIA6522::PCR] & 0x10)
        ? (!prev && level)
        : (prev && !level);
    if (activeEdge) {
        m_via->m_regs[VIA6522::IFR] |= 0x10;
        if ((m_via->m_regs[VIA6522::PCR] & 0xE0) == 0x80) m_via->driveCB2(true);
        m_via->updateIrq();
    }
    m_via->m_cb1Prev = level;
}

void VIA6522::reset() {
    std::memset(m_regs, 0, sizeof(m_regs));
    m_t1Latch = 0xFFFF;
    m_t1Counter = 0;
    m_t1Active = false;
    m_t2Counter = 0;
    m_t2Active = false;
    m_ca1Prev = true;
    m_ca2Prev = true;
    m_cb1Prev = true;
    m_cb2Prev = true;
    m_lastIrq = false;
    if (m_irqLine) m_irqLine->set(false);
    driveCA2(true);
    driveCB2(true);
}

bool VIA6522::ioRead(IBus* bus, uint32_t addr, uint8_t* val) {
    (void)bus;
    if ((addr & ~addrMask()) != m_baseAddr) return false;

    uint8_t reg = addr & 0xF;
    switch (reg) {
        case ORA:
        case IORA2:
            *val = m_regs[ORA];
            if (m_portADevice) {
                uint8_t pins = m_portADevice->readPort();
                *val = (m_regs[ORA] & m_regs[DDRA]) | (pins & ~m_regs[DDRA]);
            }
            if (reg == ORA) {
                m_regs[IFR] &= ~0x02; // Clear CA1 flag
                // Clear CA2 flag if CA2 is input in non-independent mode (PCR[3]=0, PCR[1]=0)
                if (!(m_regs[PCR] & 0x08) && !(m_regs[PCR] & 0x02))
                    m_regs[IFR] &= ~0x01;
                // Drive CA2 for handshake/pulse output modes
                uint8_t ca2mode = (m_regs[PCR] >> 1) & 0x07;
                if      (ca2mode == 0x04) driveCA2(false);                     // handshake: assert
                else if (ca2mode == 0x05) { driveCA2(false); driveCA2(true); } // pulse
                updateIrq();
            }
            break;

        case ORB:
            *val = m_regs[ORB];
            if (m_portBDevice) {
                uint8_t pins = m_portBDevice->readPort();
                *val = (m_regs[ORB] & m_regs[DDRB]) | (pins & ~m_regs[DDRB]);
            }
            m_regs[IFR] &= ~0x10; // Clear CB1 flag
            // Clear CB2 flag if CB2 is input in non-independent mode (PCR[7]=0, PCR[5]=0)
            if (!(m_regs[PCR] & 0x80) && !(m_regs[PCR] & 0x20))
                m_regs[IFR] &= ~0x08;
            {
                uint8_t cb2mode = (m_regs[PCR] >> 5) & 0x07;
                if      (cb2mode == 0x04) driveCB2(false);
                else if (cb2mode == 0x05) { driveCB2(false); driveCB2(true); }
            }
            updateIrq();
            break;

        case T1CL:
            *val = m_t1Counter & 0xFF;
            m_regs[IFR] &= ~0x40; // Clear T1 interrupt
            updateIrq();
            break;

        case T1CH:
            *val = (m_t1Counter >> 8) & 0xFF;
            break;

        case T2CL:
            *val = m_t2Counter & 0xFF;
            m_regs[IFR] &= ~0x20; // Clear T2 interrupt
            updateIrq();
            break;

        case T2CH:
            *val = (m_t2Counter >> 8) & 0xFF;
            break;

        case IFR:
            *val = m_regs[IFR];
            break;

        default:
            *val = m_regs[reg];
            break;
    }
    return true;
}

bool VIA6522::ioWrite(IBus* bus, uint32_t addr, uint8_t val) {
    (void)bus;
    if ((addr & ~addrMask()) != m_baseAddr) return false;

    uint8_t reg = addr & 0xF;
    switch (reg) {
        case ORA:
        case IORA2:
            m_regs[ORA] = val;
            if (m_portADevice) m_portADevice->writePort(val);
            if (m_portAWriteCb) m_portAWriteCb(val);
            if (reg == ORA) {
                m_regs[IFR] &= ~0x02; // Clear CA1 flag
                if (!(m_regs[PCR] & 0x08) && !(m_regs[PCR] & 0x02))
                    m_regs[IFR] &= ~0x01;
                uint8_t ca2mode = (m_regs[PCR] >> 1) & 0x07;
                if      (ca2mode == 0x04) driveCA2(false);
                else if (ca2mode == 0x05) { driveCA2(false); driveCA2(true); }
                updateIrq();
            }
            break;

        case ORB:
            m_regs[ORB] = val;
            if (m_portBDevice) m_portBDevice->writePort(val);
            if (m_portBWriteCb) m_portBWriteCb(val);
            m_regs[IFR] &= ~0x10; // Clear CB1 flag
            if (!(m_regs[PCR] & 0x80) && !(m_regs[PCR] & 0x20))
                m_regs[IFR] &= ~0x08;
            {
                uint8_t cb2mode = (m_regs[PCR] >> 5) & 0x07;
                if      (cb2mode == 0x04) driveCB2(false);
                else if (cb2mode == 0x05) { driveCB2(false); driveCB2(true); }
            }
            updateIrq();
            break;

        case DDRA:
            m_regs[DDRA] = val;
            if (m_portADevice) m_portADevice->setDdr(val);
            break;

        case DDRB:
            m_regs[DDRB] = val;
            if (m_portBDevice) m_portBDevice->setDdr(val);
            break;

        case T1CL:
        case T1LL:
            m_t1Latch = (m_t1Latch & 0xFF00) | val;
            break;

        case T1CH:
            m_t1Latch = (m_t1Latch & 0x00FF) | (val << 8);
            m_t1Counter = m_t1Latch;
            m_t1Active = true;
            m_regs[IFR] &= ~0x40; // Clear T1 interrupt
            updateIrq();
            break;

        case T1LH:
            m_t1Latch = (m_t1Latch & 0x00FF) | (val << 8);
            m_regs[IFR] &= ~0x40; // Clear T1 interrupt
            updateIrq();
            break;

        case T2CL:
            m_regs[T2CL] = val; // Actually latches into T2CL latch
            break;

        case T2CH:
            m_t2Counter = (val << 8) | m_regs[T2CL];
            m_t2Active = true;
            m_regs[IFR] &= ~0x20; // Clear T2 interrupt
            updateIrq();
            break;

        case IFR:
            // Bits 0-6 are cleared by writing a 1 to them
            m_regs[IFR] &= ~(val & 0x7F);
            updateIrq();
            break;

        case IER:
            if (val & 0x80) {
                m_regs[IER] |= (val & 0x7F);
            } else {
                m_regs[IER] &= ~(val & 0x7F);
            }
            updateIrq();
            break;

        case PCR:
            m_regs[PCR] = val;
            // For manual output modes, drive the line immediately.
            {
                uint8_t ca2mode = (val >> 1) & 0x07;
                if      (ca2mode == 0x06) driveCA2(false); // manual low
                else if (ca2mode == 0x07) driveCA2(true);  // manual high
                uint8_t cb2mode = (val >> 5) & 0x07;
                if      (cb2mode == 0x06) driveCB2(false);
                else if (cb2mode == 0x07) driveCB2(true);
            }
            break;

        default:
            m_regs[reg] = val;
            break;
    }
    return true;
}

void VIA6522::driveCA2(bool level) {
    if (m_ca2Line) m_ca2Line->set(level);
}

void VIA6522::driveCB2(bool level) {
    if (m_cb2Line) m_cb2Line->set(level);
}

void VIA6522::tick(uint64_t cycles) {
    // CA1 input edge detection → IFR bit 1; in CA2 handshake mode, active CA1 releases CA2.
    // When m_ca1Line == &m_ca1Conduit, CA1Conduit::set() already handled the edge and updated
    // m_ca1Prev, so this tick path becomes a no-op (same level → no active edge).
    // For externally set CA1 lines (unit tests, etc.) this path still detects edges.
    if (m_ca1Line) {
        bool lvl = m_ca1Line->get();
        bool activeEdge = (m_regs[PCR] & 0x01) ? (!m_ca1Prev && lvl)  // positive edge
                                                : (m_ca1Prev && !lvl); // negative edge
        if (activeEdge) {
            m_regs[IFR] |= 0x02;
            if ((m_regs[PCR] & 0x0E) == 0x08) driveCA2(true); // handshake: release
            updateIrq();
        }
        m_ca1Prev = lvl;
    }

    // CA2 input edge detection → IFR bit 0 (only when CA2 is configured as input)
    if (m_ca2Line && !(m_regs[PCR] & 0x08)) {
        bool lvl = m_ca2Line->get();
        bool activeEdge = (m_regs[PCR] & 0x04) ? (!m_ca2Prev && lvl)
                                                : (m_ca2Prev && !lvl);
        if (activeEdge) {
            m_regs[IFR] |= 0x01;
            updateIrq();
        }
        m_ca2Prev = lvl;
    }

    // CB1 input edge detection → IFR bit 4; same dual-path rationale as CA1 above.
    if (m_cb1Line) {
        bool lvl = m_cb1Line->get();
        bool activeEdge = (m_regs[PCR] & 0x10) ? (!m_cb1Prev && lvl)
                                                : (m_cb1Prev && !lvl);
        if (activeEdge) {
            m_regs[IFR] |= 0x10;
            if ((m_regs[PCR] & 0xE0) == 0x80) driveCB2(true); // handshake: release
            updateIrq();
        }
        m_cb1Prev = lvl;
    }

    // CB2 input edge detection → IFR bit 3 (only when CB2 is configured as input)
    if (m_cb2Line && !(m_regs[PCR] & 0x80)) {
        bool lvl = m_cb2Line->get();
        bool activeEdge = (m_regs[PCR] & 0x40) ? (!m_cb2Prev && lvl)
                                                : (m_cb2Prev && !lvl);
        if (activeEdge) {
            m_regs[IFR] |= 0x08;
            updateIrq();
        }
        m_cb2Prev = lvl;
    }

    // Tick Timer 1
    if (m_t1Active) {
        uint16_t prev = m_t1Counter;
        m_t1Counter -= (uint16_t)cycles;
        if (prev < (uint16_t)cycles) { // Underflow
            m_regs[IFR] |= 0x40;
            updateIrq();
            
            // Continuous mode?
            if (m_regs[ACR] & 0x40) {
                m_t1Counter = m_t1Latch;
            } else {
                m_t1Active = false;
            }
        }
    }

    // Tick Timer 2 (Simplified: only one-shot interval mode)
    if (m_t2Active) {
        uint16_t prev = m_t2Counter;
        m_t2Counter -= (uint16_t)cycles;
        if (prev < (uint16_t)cycles) { // Underflow
            m_regs[IFR] |= 0x20;
            updateIrq();
            m_t2Active = false;
        }
    }
}

void VIA6522::updateIrq() {
    bool irq = (m_regs[IFR] & m_regs[IER] & 0x7F) != 0;
    if (irq) {
        m_regs[IFR] |= 0x80;
    } else {
        m_regs[IFR] &= ~0x80;
    }
    
    if (irq != m_lastIrq) {
        if (m_irqLine) m_irqLine->set(irq);
        m_lastIrq = irq;
    }
}

void VIA6522::getDeviceInfo(DeviceInfo& out) const {
    out.name = m_name;
    out.baseAddr = m_baseAddr;
    out.addrMask = addrMask();

    auto addReg = [&](const std::string& name, Reg reg, const std::string& desc = "") {
        out.registers.push_back({name, (uint32_t)reg, m_regs[reg], desc});
    };

    addReg("ORB", ORB);
    addReg("ORA", ORA);
    addReg("DDRB", DDRB);
    addReg("DDRA", DDRA);
    addReg("T1CL", T1CL);
    addReg("T1CH", T1CH);
    addReg("T1LL", T1LL);
    addReg("T1LH", T1LH);
    addReg("T2CL", T2CL);
    addReg("T2CH", T2CH);
    addReg("SR", SR);
    addReg("ACR", ACR);
    addReg("PCR", PCR);
    addReg("IFR", IFR);
    addReg("IER", IER);
    addReg("IORA2", IORA2);

    char buf[32];
    std::sprintf(buf, "%d", (int)m_t1Counter);
    out.state.push_back({"Timer 1 Counter", buf});
    std::sprintf(buf, "%d", (int)m_t2Counter);
    out.state.push_back({"Timer 2 Counter", buf});
    out.state.push_back({"Timer 1 Active", m_t1Active ? "true" : "false"});
    out.state.push_back({"Timer 2 Active", m_t2Active ? "true" : "false"});

    out.dependencies.push_back({"Port A Device", m_portADevice ? "connected" : "none"});
    out.dependencies.push_back({"Port B Device", m_portBDevice ? "connected" : "none"});
    out.dependencies.push_back({"IRQ Line", m_irqLine ? "connected" : "none"});
}
