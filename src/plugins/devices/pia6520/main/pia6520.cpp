#include "pia6520.h"
#include "libdevices/main/isignal_line.h"
#include "include/mmemu_plugin_api.h"
#include <cstdio>

PIA6520::PIA6520() : m_name("6520"), m_baseAddr(0xE810) {
    m_ca1Conduit.m_owner = this;
    m_cb1Conduit.m_owner = this;
    m_ca1Line = &m_ca1Conduit;
    m_cb1Line = &m_cb1Conduit;
    PIA6520::reset();
}

PIA6520::PIA6520(const std::string& name, uint32_t baseAddr) : m_name(name), m_baseAddr(baseAddr) {
    m_ca1Conduit.m_owner = this;
    m_cb1Conduit.m_owner = this;
    m_ca1Line = &m_ca1Conduit;
    m_cb1Line = &m_cb1Conduit;
    PIA6520::reset();
}

void PIA6520::CA1Conduit::set(bool level) {
    m_level = level;
    if (m_owner) m_owner->setCA1(level);
}

void PIA6520::CB1Conduit::set(bool level) {
    m_level = level;
    if (m_owner) m_owner->setCB1(level);
}

void PIA6520::reset() {
    m_ora = 0xFF; // Pull-ups
    m_ddra = 0;
    m_cra = 0;
    m_orb = 0xFF; // Bit 7 high = no diagnostic boot
    m_ddrb = 0;
    m_crb = 0;
    m_ca1Prev = true;
    m_ca2Prev = true;
    m_cb1Prev = true;
    m_cb2Prev = true;
    m_lastIrqA = false;
    m_lastIrqB = false;
}

void PIA6520::updateIrq() {
    bool irqA = (m_cra & 0x80) && (m_cra & 0x01);
    irqA = irqA || ((m_cra & 0x40) && (m_cra & 0x08));
    bool irqB = (m_crb & 0x80) && (m_crb & 0x01);
    irqB = irqB || ((m_crb & 0x40) && (m_crb & 0x08));
    
    // Only call set() if the state has changed to avoid overwriting shared lines
    if (irqA != m_lastIrqA) {
        if (m_irqALine) m_irqALine->set(irqA);
        m_lastIrqA = irqA;
    }
    if (irqB != m_lastIrqB) {
        if (m_irqBLine) m_irqBLine->set(irqB);
        m_lastIrqB = irqB;
    }
}

void PIA6520::setCA1(bool lvl) {
    bool edge = (m_cra & 0x02) ? (!m_ca1Prev && lvl) : (m_ca1Prev && !lvl);
    if (edge) { 
        m_cra |= 0x80; 
        // Handshake: release CA2 if in mode 100
        if ((m_cra & 0x38) == 0x20) driveCA2(true);
        updateIrq(); 
    }
    m_ca1Prev = lvl;
}

void PIA6520::setCA2(bool lvl) {
    if (!(m_cra & 0x20)) { // Input mode
        bool edge = (m_cra & 0x10) ? (!m_ca2Prev && lvl) : (m_ca2Prev && !lvl);
        if (edge) { m_cra |= 0x40; updateIrq(); }
    }
    m_ca2Prev = lvl;
}

void PIA6520::setCB1(bool lvl) {
    bool edge = (m_crb & 0x02) ? (!m_cb1Prev && lvl) : (m_cb1Prev && !lvl);
    if (edge) { 
        m_crb |= 0x80; 
        // Handshake: release CB2 if in mode 100
        if ((m_crb & 0x38) == 0x20) driveCB2(true);
        updateIrq(); 
    }
    m_cb1Prev = lvl;
}

void PIA6520::setCB2(bool lvl) {
    if (!(m_crb & 0x20)) { // Input mode
        bool edge = (m_crb & 0x10) ? (!m_cb2Prev && lvl) : (m_cb2Prev && !lvl);
        if (edge) { m_crb |= 0x40; updateIrq(); }
    }
    m_cb2Prev = lvl;
}

void PIA6520::tick(uint64_t /*cycles*/) {
    if (m_ca1Line) setCA1(m_ca1Line->get());
    if (m_ca2Line) setCA2(m_ca2Line->get());
    if (m_cb1Line) setCB1(m_cb1Line->get());
    if (m_cb2Line) setCB2(m_cb2Line->get());
}

void PIA6520::driveCA2(bool level) {
    if (m_ca2Line) m_ca2Line->set(level);
}

void PIA6520::driveCB2(bool level) {
    if (m_cb2Line) m_cb2Line->set(level);
}

bool PIA6520::ioRead(IBus*, uint32_t addr, uint8_t* val) {
    if ((addr & ~addrMask()) != baseAddr()) return false;

    switch (addr & 0x03) {
        case 0x00: // ORA/DDRA
            if (m_cra & 0x04) {
                if (m_portADevice) {
                    uint8_t pins = m_portADevice->readPort();
                    *val = (m_ora & m_ddra) | (pins & ~m_ddra);
                } else {
                    *val = m_ora;
                }
                if (m_logger && m_logNamed && *val != 0xFF) {
                    char buf[40]; snprintf(buf, sizeof(buf), "ioRead ORA: %02X (orb=%02X)", *val, m_orb);
                    m_logNamed(m_logger, SIM_LOG_DEBUG, buf);
                }
                // Handshake: read ORA drives CA2 low if in mode 100
                if ((m_cra & 0x38) == 0x20) driveCA2(false);

                m_cra &= ~0x80;
                if (!(m_cra & 0x20)) m_cra &= ~0x40;
                updateIrq();
            } else {
                *val = m_ddra;
            }
            break;
        case 0x01: // CRA
            *val = m_cra;
            break;
        case 0x02: // ORB/DDRB
            if (m_crb & 0x04) {
                if (m_portBDevice) {
                    uint8_t pins = m_portBDevice->readPort();
                    *val = (m_orb & m_ddrb) | (pins & ~m_ddrb);
                } else {
                    *val = m_orb;
                }
                m_crb &= ~0x80;
                if (!(m_crb & 0x20)) m_crb &= ~0x40;
                updateIrq();
            } else {
                *val = m_ddrb;
            }
            break;
        case 0x03: // CRB
            *val = m_crb;
            break;
    }
    return true;
}

bool PIA6520::ioWrite(IBus*, uint32_t addr, uint8_t val) {
    if ((addr & ~addrMask()) != baseAddr()) return false;

    switch (addr & 0x03) {
        case 0x00:
            if (m_cra & 0x04) {
                m_ora = val;
                if (m_portADevice) m_portADevice->writePort(val);
                if (m_portAWriteCb) m_portAWriteCb(val);
            } else {
                m_ddra = val;
                if (m_portADevice) m_portADevice->setDdr(val);
            }
            break;
        case 0x01:
            if (m_logger && m_logNamed) {
                char buf[32]; snprintf(buf, sizeof(buf), "ioWrite CRA: %02X", val);
                m_logNamed(m_logger, SIM_LOG_DEBUG, buf);
            }
            m_cra = (m_cra & 0xC0) | (val & 0x3F);
            if ((m_cra & 0x38) == 0x30) driveCA2(false);
            else if ((m_cra & 0x38) == 0x38) driveCA2(true);
            updateIrq();
            break;
        case 0x02:
            if (m_crb & 0x04) {
                m_orb = val;
                if (m_portBDevice) m_portBDevice->writePort(val);
                if (m_portBWriteCb) m_portBWriteCb(val);
                if (m_logger && m_logNamed) {
                    char buf[32]; snprintf(buf, sizeof(buf), "ioWrite ORB: %02X", val);
                    m_logNamed(m_logger, SIM_LOG_DEBUG, buf);
                }
                // Handshake: write ORB drives CB2 low if in mode 100
                if ((m_crb & 0x38) == 0x20) driveCB2(false);

                m_crb &= ~0x80;
                if (!(m_crb & 0x20)) m_crb &= ~0x40;
                updateIrq();
            } else {
                m_ddrb = val;
                if (m_portBDevice) m_portBDevice->setDdr(val);
            }
            break;
        case 0x03:
            if (m_logger && m_logNamed) {
                char buf[32]; snprintf(buf, sizeof(buf), "ioWrite CRB: %02X", val);
                m_logNamed(m_logger, SIM_LOG_DEBUG, buf);
            }
            m_crb = (m_crb & 0xC0) | (val & 0x3F);
            if ((m_crb & 0x38) == 0x30) driveCB2(false);
            else if ((m_crb & 0x38) == 0x38) driveCB2(true);
            updateIrq();
            break;
    }
    return true;
}

PIA6520::Snapshot PIA6520::getSnapshot() const {
    return { m_ora, m_ddra, m_cra, m_orb, m_ddrb, m_crb, m_ca1Prev, m_ca2Prev, m_cb1Prev, m_cb2Prev };
}

void PIA6520::restoreSnapshot(const Snapshot& s) {
    m_ora = s.ora; m_ddra = s.ddra; m_cra = s.cra;
    m_orb = s.orb; m_ddrb = s.ddrb; m_crb = s.crb;
    m_ca1Prev = s.ca1Prev; m_ca2Prev = s.ca2Prev;
    m_cb1Prev = s.cb1Prev; m_cb2Prev = s.cb2Prev;
    updateIrq();
}
