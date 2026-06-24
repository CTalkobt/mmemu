#include "cia6526.h"
#include <cstring>

CIA6526::CIA6526(const std::string& name, uint32_t baseAddr)
    : m_name(name), m_baseAddr(baseAddr)
{
    CIA6526::reset();
}

void CIA6526::reset() {
    m_pra  = 0xFF;
    m_prb  = 0xFF;
    m_ddra = 0x00;
    m_ddrb = 0x00;

    m_taLatch   = 0xFFFF;
    m_taCounter = 0xFFFF;
    m_taRunning = false;

    m_tbLatch   = 0xFFFF;
    m_tbCounter = 0xFFFF;
    m_tbRunning = false;

    m_icrMask    = 0x00;
    m_icrPending = 0x00;

    m_cra = 0x00;
    m_crb = 0x00;

    m_todTen = 0x00;
    m_todSec = 0x00;
    m_todMin = 0x00;
    m_todHr  = 0x01;

    m_alarmTen = 0x00;
    m_alarmSec = 0x00;
    m_alarmMin = 0x00;
    m_alarmHr  = 0x01;

    m_todLatched = false;
    m_latchTen   = 0x00;
    m_latchSec   = 0x00;
    m_latchMin   = 0x00;
    m_latchHr    = 0x01;

    m_todRunning = true;
    m_todAccum   = 0;

    if (m_irqLine) m_irqLine->set(false);
    if (m_portADevice) m_portADevice->setDdr(0x00);
    if (m_portBDevice) m_portBDevice->setDdr(0x00);
    if (m_portAWriteCallback) m_portAWriteCallback(m_pra, m_ddra);
}

// ---------------------------------------------------------------------------
// Address decode
// ---------------------------------------------------------------------------

bool CIA6526::ioRead(IBus* /*bus*/, uint32_t addr, uint8_t* val) {
    if ((addr & ~addrMask()) != m_baseAddr) return false;

    uint8_t reg = addr & 0x0F;
    switch (reg) {
        case PRA: {
            // Output bits from latch; input bits from port device (pins).
            *val = m_pra & m_ddra; // output bits
            if (m_portADevice) {
                uint8_t pins = m_portADevice->readPort();
                *val |= (pins & ~m_ddra); // input bits
            } else {
                *val |= (~m_ddra & 0xFF); // unconnected pins float high
            }
            break;
        }
        case PRB: {
            *val = m_prb & m_ddrb;
            if (m_portBDevice) {
                uint8_t pins = m_portBDevice->readPort();
                *val |= (pins & ~m_ddrb);
            } else {
                *val |= (~m_ddrb & 0xFF);
            }
            break;
        }
        case DDRA: *val = m_ddra; break;
        case DDRB: *val = m_ddrb; break;

        case TALO: *val = m_taCounter & 0xFF; break;
        case TAHI: *val = (m_taCounter >> 8) & 0xFF; break;

        case TBLO: *val = m_tbCounter & 0xFF; break;
        case TBHI: *val = (m_tbCounter >> 8) & 0xFF; break;

        // TOD reads: reading TODHR latches all TOD registers; reading TODTEN
        // releases the latch.
        case TODTEN:
            *val = m_todLatched ? m_latchTen : m_todTen;
            m_todLatched = false; // latch released
            break;
        case TODSEC: *val = m_todLatched ? m_latchSec : m_todSec; break;
        case TODMIN: *val = m_todLatched ? m_latchMin : m_todMin; break;
        case TODHR:
            // Freeze latch at this moment.
            m_latchTen = m_todTen;
            m_latchSec = m_todSec;
            m_latchMin = m_todMin;
            m_latchHr  = m_todHr;
            m_todLatched = true;
            *val = m_latchHr;
            break;

        case SDR: *val = 0x00; break; // SDR not implemented

        case ICR: {
            // Read returns pending bits + bit 7 if any; clears all pending bits.
            uint8_t pending = m_icrPending;
            if (pending & m_icrMask) pending |= ICR_INT;
            m_icrPending = 0x00;
            updateIrq(); // de-assert IRQ if all cleared
            *val = pending;
            break;
        }
        case CRA: *val = m_cra & ~CR_LOAD; break; // LOAD bit always reads 0
        case CRB: *val = m_crb & ~CR_LOAD; break;

        default: *val = 0xFF; break;
    }
    return true;
}

bool CIA6526::ioWrite(IBus* /*bus*/, uint32_t addr, uint8_t val) {
    if ((addr & ~addrMask()) != m_baseAddr) return false;

    uint8_t reg = addr & 0x0F;
    switch (reg) {
        case PRA:
            m_pra = val;
            if (m_portADevice) {
                m_portADevice->writePort(val);
            }
            if (m_portAWriteCallback) m_portAWriteCallback(m_pra, m_ddra);
            break;
        case PRB:
            m_prb = val;
            if (m_portBDevice) m_portBDevice->writePort(val);
            break;
        case DDRA:
            m_ddra = val;
            if (m_portADevice) m_portADevice->setDdr(val);
            if (m_portAWriteCallback) m_portAWriteCallback(m_pra, m_ddra);
            break;
        case DDRB:
            m_ddrb = val;
            if (m_portBDevice) m_portBDevice->setDdr(val);
            break;

        case TALO: m_taLatch = (m_taLatch & 0xFF00) | val; break;
        case TAHI:
            m_taLatch = (m_taLatch & 0x00FF) | ((uint16_t)val << 8);
            // If Timer A is stopped, force-load latch into counter.
            if (!m_taRunning) m_taCounter = m_taLatch;
            m_icrPending &= ~ICR_TA; // clear pending TA interrupt on latch load
            updateIrq();
            break;

        case TBLO: m_tbLatch = (m_tbLatch & 0xFF00) | val; break;
        case TBHI:
            m_tbLatch = (m_tbLatch & 0x00FF) | ((uint16_t)val << 8);
            if (!m_tbRunning) m_tbCounter = m_tbLatch;
            m_icrPending &= ~ICR_TB;
            updateIrq();
            break;

        // TOD / Alarm writes: CRB bit 7 (ALARM) determines target.
        case TODTEN:
            if (m_crb & CRB_ALARM) m_alarmTen = val & 0x0F;
            else                   { m_todTen  = val & 0x0F; m_todRunning = true; }
            break;
        case TODSEC:
            if (m_crb & CRB_ALARM) m_alarmSec = val & 0x7F;
            else                   m_todSec    = val & 0x7F;
            break;
        case TODMIN:
            if (m_crb & CRB_ALARM) m_alarmMin = val & 0x7F;
            else                   m_todMin    = val & 0x7F;
            break;
        case TODHR:
            if (m_crb & CRB_ALARM) m_alarmHr  = val & 0x9F;
            else {
                // Writing hours halts TOD until tenths are written.
                m_todHr      = val & 0x9F;
                m_todRunning = false;
            }
            break;

        case SDR:
            // Serial data register.  When CRA bit 6 is set (output mode),
            // writing triggers a serial shift.  With no physical IEC device,
            // the shift "completes" immediately — set ICR bit 3 (SP flag).
            // This matches xemu's CIA_OUT_SDR_SETS_ICR_BIT3 behavior and
            // prevents the ROM's IEC serial polling loop from hanging
            // (see mega65-rom-public#169).
            // Only set the flag (don't call updateIrq) — the ROM polls
            // $DC0D to check for completion rather than using interrupts.
            if (m_cra & 0x40) {
                m_icrPending |= ICR_SP;
            }
            break;

        case ICR:
            // Bit 7 = 1: set bits in mask; bit 7 = 0: clear bits from mask.
            if (val & 0x80) m_icrMask |=  (val & 0x7F);
            else            m_icrMask &= ~(val & 0x7F);
            updateIrq();
            break;

        case CRA: {
            uint8_t prev = m_cra;
            m_cra = val;
            m_taRunning = (val & CR_START) != 0;
            // Force-load latch if LOAD bit set; then clear the bit.
            if (val & CR_LOAD) {
                m_taCounter = m_taLatch;
                m_cra &= ~CR_LOAD;
            }
            if (!(prev & CR_START) && m_taRunning) {
                // Reload counter when starting from stopped state.
                // (Matching real CIA: counter loaded at start)
            }
            break;
        }
        case CRB: {
            m_crb = val;
            m_tbRunning = (val & CR_START) != 0;
            if (val & CR_LOAD) {
                m_tbCounter = m_tbLatch;
                m_crb &= ~CR_LOAD;
            }
            break;
        }

        default: break;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Tick
// ---------------------------------------------------------------------------

void CIA6526::tick(uint64_t cycles) {
    uint32_t taUnderflows = 0;
    tickTimerA(cycles);
    taUnderflows = 0; // count is tracked inside tickTimerA via m_icrPending; we
                      // infer underflows for Timer B by checking the delta.
    // Use a simpler approach: tick Timer B similarly, and for TB_INMODE_TA mode
    // drive it from the TA underflow count accumulated during this tick.
    tickTimerB(cycles, taUnderflows);
    tickTOD(cycles);
}

void CIA6526::tickTimerA(uint64_t cycles) {
    if (!m_taRunning) return;

    uint32_t elapsed = (uint32_t)cycles;
    while (elapsed > 0) {
        uint32_t step = (elapsed < (uint32_t)m_taCounter) ? elapsed
                                                           : (uint32_t)m_taCounter;
        m_taCounter -= (uint16_t)step;
        elapsed -= step;

        if (m_taCounter == 0) {
            // Underflow.
            m_icrPending |= ICR_TA;
            updateIrq();

            if (m_cra & CR_ONESHOT) {
                m_taRunning = false;
                m_cra &= ~CR_START;
                break;
            }
            m_taCounter = m_taLatch ? m_taLatch : 0xFFFF;
        }
    }
}

void CIA6526::tickTimerB(uint64_t cycles, uint32_t /*taUnderflows*/) {
    if (!m_tbRunning) return;

    uint8_t inmode = m_crb & CRB_INMODE_MASK;
    if (inmode == CRB_INMODE_TA) {
        // TB counts TA underflows — we can't easily count them here without
        // reworking tickTimerA, so approximate: if ICR_TA fired this tick,
        // step TB by the number of times TA wrapped (at least 1).
        // For now just step once if TA fired.
        if (!(m_icrPending & ICR_TA)) return;
        cycles = 1;
    }

    uint32_t elapsed = (uint32_t)cycles;
    while (elapsed > 0) {
        uint32_t step = (elapsed < (uint32_t)m_tbCounter) ? elapsed
                                                           : (uint32_t)m_tbCounter;
        m_tbCounter -= (uint16_t)step;
        elapsed -= step;

        if (m_tbCounter == 0) {
            m_icrPending |= ICR_TB;
            updateIrq();

            if (m_crb & CR_ONESHOT) {
                m_tbRunning = false;
                m_crb &= ~CR_START;
                break;
            }
            m_tbCounter = m_tbLatch ? m_tbLatch : 0xFFFF;
        }
    }
}

void CIA6526::tickTOD(uint64_t cycles) {
    if (!m_todRunning) return;

    m_todAccum += cycles;

    // Determine tenths-of-second period in CPU cycles.
    // TOD increments tenths at 10 Hz; external reference is 50 or 60 Hz.
    // We approximate: 1 tenth = clockHz / 10 cycles.
    uint64_t tenthPeriod = m_clockHz / 10;
    if (tenthPeriod == 0) tenthPeriod = 100000;

    while (m_todAccum >= tenthPeriod) {
        m_todAccum -= tenthPeriod;

        // Increment BCD tenths (0–9).
        m_todTen++;
        if (m_todTen > 9) {
            m_todTen = 0;
            m_todSec = bcdInc(m_todSec, 59);
            if (m_todSec == 0) {
                m_todMin = bcdInc(m_todMin, 59);
                if (m_todMin == 0) {
                    // Increment hours (BCD 1–12 + bit7 AM/PM).
                    uint8_t hr  = m_todHr & 0x1F;
                    uint8_t pm  = m_todHr & 0x80;
                    hr = bcdInc(hr, 12);
                    if (hr == 0) hr = 0x01; // wrap: 12 → 01 (BCD)
                    // Flip AM/PM when rolling from 11 → 12.
                    uint8_t prev = m_todHr & 0x1F;
                    if (prev == 0x11) pm ^= 0x80; // BCD 11 flips the flag
                    m_todHr = pm | hr;
                }
            }
        }

        // Check alarm.
        if (m_todTen == m_alarmTen &&
            m_todSec == m_alarmSec &&
            m_todMin == m_alarmMin &&
            m_todHr  == m_alarmHr) {
            m_icrPending |= ICR_TOD;
            updateIrq();
        }
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void CIA6526::flagTrigger() {
    m_icrPending |= 0x10;
    updateIrq();
}

void CIA6526::updateIrq() {
    bool assert = (m_icrPending & m_icrMask) != 0;
    if (m_irqLine) m_irqLine->set(assert);
}

uint8_t CIA6526::bcdInc(uint8_t bcd, uint8_t maxBcd) {
    uint8_t lo = (bcd & 0x0F) + 1;
    uint8_t hi = (bcd >> 4);
    if (lo > 9) { lo = 0; hi++; }
    // Convert to decimal to check overflow.
    uint8_t decimal = hi * 10 + lo;
    if (decimal > maxBcd) return 0x00; // wrap to zero
    return (hi << 4) | lo;
}

void CIA6526::getDeviceInfo(DeviceInfo& out) const {
    out.name = m_name;
    out.baseAddr = m_baseAddr;
    out.addrMask = addrMask();

    auto addReg = [&](const std::string& name, Reg reg, uint8_t val, const std::string& desc = "") {
        out.registers.push_back({name, (uint32_t)reg, val, desc});
    };

    addReg("PRA", PRA, m_pra);
    addReg("PRB", PRB, m_prb);
    addReg("DDRA", DDRA, m_ddra);
    addReg("DDRB", DDRB, m_ddrb);
    addReg("TALO", TALO, m_taLatch & 0xFF);
    addReg("TAHI", TAHI, m_taLatch >> 8);
    addReg("TBLO", TBLO, m_tbLatch & 0xFF);
    addReg("TBHI", TBHI, m_tbLatch >> 8);
    addReg("TODTEN", TODTEN, m_todTen);
    addReg("TODSEC", TODSEC, m_todSec);
    addReg("TODMIN", TODMIN, m_todMin);
    addReg("TODHR", TODHR, m_todHr);
    addReg("SDR", SDR, 0); // SDR not fully implemented
    addReg("ICR", ICR, m_icrPending);
    addReg("CRA", CRA, m_cra);
    addReg("CRB", CRB, m_crb);

    char buf[32];
    std::sprintf(buf, "%d", (int)m_taCounter);
    out.state.push_back({"Timer A Counter", buf});
    std::sprintf(buf, "%d", (int)m_tbCounter);
    out.state.push_back({"Timer B Counter", buf});
    out.state.push_back({"Timer A Running", m_taRunning ? "true" : "false"});
    out.state.push_back({"Timer B Running", m_tbRunning ? "true" : "false"});
    
    out.dependencies.push_back({"Port A Device", m_portADevice ? "connected" : "none"});
    out.dependencies.push_back({"Port B Device", m_portBDevice ? "connected" : "none"});
    out.dependencies.push_back({"IRQ Line", m_irqLine ? "connected" : "none"});
}
