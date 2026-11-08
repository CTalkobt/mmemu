// 6522 VIA for the BBC Micro, after b-em via.c (GPL-2.0-or-later)

#include "bbc_via.h"

namespace bbc {

namespace {
constexpr uint8_t INT_CA1 = 0x02, INT_CA2 = 0x01, INT_CB1 = 0x10, INT_CB2 = 0x08;
constexpr uint8_t INT_TIMER1 = 0x40, INT_TIMER2 = 0x20;
enum { ORB, ORA, DDRB, DDRA, T1CL, T1CH, T1LL, T1LH, T2CL, T2CH, SR, ACR, PCR, IFR, IER, ORANH };
constexpr int TLIMIT = -3;
}  // namespace

void BbcVia::reset() {
    m_ora = m_orb = 0;
    m_ddra = m_ddrb = 0;
    m_ifr = m_ier = 0;
    m_t1pb7 = 0x80;
    m_t1c = m_t1l = 0x1FFFE;
    m_t2c = m_t2l = 0x1FFFE;
    m_t1hit = m_t2hit = 1;
    m_acr = m_pcr = 0;
    m_srCount = 0;
}

void BbcVia::updateIfr() {
    if ((m_ifr & 0x7F) & (m_ier & 0x7F)) m_ifr |= 0x80;
    else m_ifr &= 0x7F;
}

void BbcVia::poll(int cycles) {
    m_t1c -= cycles;
    if (m_t1c < TLIMIT) {
        while (m_t1c < TLIMIT) m_t1c += m_t1l + 4;
        if (!m_t1hit) {
            m_ifr |= INT_TIMER1;
            updateIfr();
            if (m_acr & 0x80) m_t1pb7 ^= 0x80;      // output to PB7
        }
        if (!(m_acr & 0x40)) m_t1hit = 1;           // one-shot
    }
    if (!(m_acr & 0x20)) {                          // timer 2 counts clocks (not PB6 pulses)
        m_t2c -= cycles;
        if (m_t2c < TLIMIT && !m_t2hit) {
            m_ifr |= INT_TIMER2;
            updateIfr();
            m_t2hit = 1;
        }
    }
    if (m_acr & 0x1C) shift(cycles);
}

void BbcVia::shift(int cycles) {
    if ((m_acr & 0x1C) != 0x18) return;             // shift out under the system clock only
    while (cycles--) {
        if (m_srCount > 0) {
            const int cb1 = !(m_srCount-- & 1);
            if (cb1) {
                cb2Out(m_sr >> 7);
                m_sr = (uint8_t)(m_sr << 1);
            }
        }
        if (m_srCount <= 0) {
            m_ifr |= 0x04;
            updateIfr();
        }
    }
}

void BbcVia::write(uint16_t addr, uint8_t val) {
    switch (addr & 0xF) {
        case ORA:
            m_ifr &= ~INT_CA1;
            if ((m_pcr & 0xA) != 0x2) m_ifr &= ~INT_CA2;    // CA2 not independent
            updateIfr();
            if ((m_pcr & 0x0E) == 0x08) { ca2Out(0); m_ca2 = 0; }                    // handshake
            if ((m_pcr & 0x0E) == 0x0A) { ca2Out(0); ca2Out(1); m_ca2 = 1; }         // pulse
            // fall through
        case ORANH:
            outA((uint8_t)((val & m_ddra) | ~m_ddra));
            m_ora = val;
            break;
        case ORB: {
            m_ifr &= ~INT_CB1;
            if ((m_pcr & 0xA0) != 0x20) m_ifr &= ~INT_CB2;
            updateIfr();
            m_orb = val;
            uint8_t out = (uint8_t)((val & m_ddrb) | ~m_ddrb);
            if (m_acr & 0x80) out = (uint8_t)((out & 0x7F) | m_t1pb7);
            outB(out);
            if ((m_pcr & 0xE0) == 0x80) { cb2Out(0); m_cb2 = 0; }
            if ((m_pcr & 0xE0) == 0xA0) { cb2Out(0); cb2Out(1); m_cb2 = 1; }
            break;
        }
        case DDRA:
            m_ddra = val;
            outA((uint8_t)((m_ora & m_ddra) | ~m_ddra));
            break;
        case DDRB: {
            m_ddrb = val;
            uint8_t out = (uint8_t)((m_orb & val) | ~val);
            if (m_acr & 0x80) out = (uint8_t)((out & 0x7F) | m_t1pb7);
            outB(out);
            break;
        }
        case ACR:
            m_acr = val;
            if (!(val & 0x80)) m_t1pb7 = 0x80;
            break;
        case PCR:
            m_pcr = val;
            if ((val & 0xE) == 0xC) { ca2Out(0); m_ca2 = 0; }
            else if (val & 0x8) { ca2Out(1); m_ca2 = 1; }
            if ((val & 0xE0) == 0xC0) { cb2Out(0); m_cb2 = 0; }
            else if (val & 0x80) { cb2Out(1); m_cb2 = 1; }
            break;
        case SR:
            m_sr = val;
            m_srCount = 16;
            m_ifr &= ~0x04;
            break;
        case T1LL:
        case T1CL:
            m_t1l = (m_t1l & 0x1FE00) | (val << 1);
            break;
        case T1LH:
            m_t1l = (m_t1l & 0x1FE) | (val << 9);
            m_ifr &= ~INT_TIMER1;
            updateIfr();
            break;
        case T1CH:
            if ((m_acr & 0xC0) == 0x80) m_t1pb7 = 0;    // one-shot lowers PB7
            m_t1l = (m_t1l & 0x1FE) | (val << 9);
            m_t1c = m_t1l + 1;
            m_t1hit = 0;
            m_ifr &= ~INT_TIMER1;
            updateIfr();
            break;
        case T2CL:
            m_t2l = (m_t2l & 0x1FE00) | (val << 1);
            break;
        case T2CH:
            m_t2l = (m_t2l & 0x1FE) | (val << 9);
            m_t2c = (m_acr & 0x20) ? m_t2l : m_t2l + 1;
            m_ifr &= ~INT_TIMER2;
            updateIfr();
            m_t2hit = 0;
            break;
        case IER:
            if (val & 0x80) m_ier |= (val & 0x7F);
            else m_ier &= ~(val & 0x7F);
            updateIfr();
            break;
        case IFR:
            m_ifr &= ~(val & 0x7F);
            updateIfr();
            break;
    }
}

uint8_t BbcVia::read(uint16_t addr) {
    uint8_t temp;
    switch (addr & 0xF) {
        case ORA:
            m_ifr &= ~INT_CA1;
            if ((m_pcr & 0xA) != 0x2) m_ifr &= ~INT_CA2;
            updateIfr();
            // fall through
        case ORANH:
            temp = m_ora & m_ddra;
            temp |= ((m_acr & 1) ? m_ira : portA()) & ~m_ddra;     // latched or live
            return temp;
        case ORB:
            m_ifr &= ~INT_CB1;
            if ((m_pcr & 0xA0) != 0x20) m_ifr &= ~INT_CB2;
            updateIfr();
            temp = m_orb & m_ddrb;
            if (m_acr & 2) {
                temp |= m_irb & ~m_ddrb;
            } else {
                temp |= portB() & ~m_ddrb;
                if (m_acr & 0x80) temp = (uint8_t)((temp & 0x7F) | m_t1pb7);
            }
            return temp;
        case DDRA: return m_ddra;
        case DDRB: return m_ddrb;
        case T1LL: return (uint8_t)((m_t1l & 0x1FE) >> 1);
        case T1LH: return (uint8_t)(m_t1l >> 9);
        case T1CL:
            m_ifr &= ~INT_TIMER1;
            updateIfr();
            if (m_t1c < -1) return 0xFF;                // during reload
            return (uint8_t)(((m_t1c + 1) >> 1) & 0xFF);
        case T1CH:
            if (m_t1c < -1) return 0xFF;
            return (uint8_t)((m_t1c + 1) >> 9);
        case T2CL:
            m_ifr &= ~INT_TIMER2;
            updateIfr();
            return (uint8_t)(((m_t2c + 1) >> 1) & 0xFF);
        case T2CH: return (uint8_t)((m_t2c + 1) >> 9);
        case SR: return m_sr;
        case ACR: return m_acr;
        case PCR: return m_pcr;
        case IER: return (uint8_t)(m_ier | 0x80);
        case IFR: return m_ifr;
    }
    return 0xFE;
}

void BbcVia::setCa1(int level) {
    if (level == m_ca1) return;
    if (((m_pcr & 0x01) && level) || (!(m_pcr & 0x01) && !level)) {
        if (m_acr & 0x01) m_ira = portA();
        m_ifr |= INT_CA1;
        updateIfr();
        if ((m_pcr & 0x0C) == 0x08) { m_ca2 = 1; ca2Out(1); }
    }
    m_ca1 = level;
}

void BbcVia::setCa2(int level) {
    if (level == m_ca2) return;
    if (m_pcr & 0x08) return;                           // output mode
    if (((m_pcr & 0x04) && level) || (!(m_pcr & 0x04) && !level)) {
        m_ifr |= INT_CA2;
        updateIfr();
    }
    m_ca2 = level;
}

void BbcVia::setCb1(int level) {
    if (level == m_cb1) return;
    if (((m_pcr & 0x10) && level) || (!(m_pcr & 0x10) && !level)) {
        if (m_acr & 0x02) {
            uint8_t data = portB() & ~m_ddrb;
            if (m_acr & 0x80) data = (uint8_t)((data & 0x7F) | m_t1pb7);
            m_irb = data;
        }
        m_ifr |= INT_CB1;
        updateIfr();
        if ((m_pcr & 0xC0) == 0x80) { m_cb2 = 1; cb2Out(1); }
    }
    m_cb1 = level;
}

void BbcVia::setCb2(int level) {
    if (level == m_cb2) return;
    if (m_pcr & 0x80) return;
    if (((m_pcr & 0x40) && level) || (!(m_pcr & 0x40) && !level)) {
        m_ifr |= INT_CB2;
        updateIfr();
    }
    m_cb2 = level;
}

}  // namespace bbc
