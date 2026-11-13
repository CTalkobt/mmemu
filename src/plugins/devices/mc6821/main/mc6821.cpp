// MC6821 PIA, after the Motorola data sheet.

#include "mc6821.h"
#include <cstring>

Mc6821::Mc6821() { reset(); }

void Mc6821::reset() {
    for (Side* s : {&a, &b}) {
        s->cr = s->ddr = s->out = 0;
        s->irq1 = s->irq2 = false;
        s->irq1At = s->irq2At = 0;
        s->c2Out = true;
    }
    if (a.postWrite) a.postWrite();
    if (b.postWrite) b.postWrite();
    if (a.onControl) a.onControl();
    if (b.onControl) b.onControl();
    irqChanged();
}

void Mc6821::advance(uint64_t now) {
    if (now <= m_now) return;
    const bool was = irq();
    m_now = now;
    if (irq() != was) irqChanged();
}

void Mc6821::setC1(Side& s, bool level, bool isA) {
    (void)isA;
    if (level == s.c1) return;
    s.c1 = level;
    const bool activeHigh = s.cr & 0x02;
    if (level != activeHigh) return;
    if (!s.irq1) s.irq1At = m_now + irqDelay;    // /IRQ follows tRS3 later
    s.irq1 = true;
    if ((s.cr & 0x38) == 0x20 && !s.c2Out) {     // handshake: Cx1 restores Cx2
        s.c2Out = true;
        if (s.onControl) s.onControl();
    }
    irqChanged();
}

void Mc6821::updateC2(Side& s) {
    if (s.c2IsOutput()) return;
    const bool level = (&s == &a) ? s.c2InSink : (s.c2InSource && s.c2InSink);
    if (level == s.c2) return;
    s.c2 = level;
    const bool activeHigh = s.cr & 0x10;
    if (level != activeHigh) return;
    if (!s.irq2) s.irq2At = m_now + irqDelay;
    s.irq2 = true;
    irqChanged();
}

// A read of port A (CA2) or a write of port B (CB2) in a strobe mode
void Mc6821::strobe(Side& s) {
    if ((s.cr & 0x30) != 0x20) return;
    s.c2Out = false;
    if (s.onControl) s.onControl();
    if (s.cr & 0x08) {                   // pulse: back high after the cycle
        s.c2Out = true;
        if (s.onControl) s.onControl();
    }
}

void Mc6821::writeCr(Side& s, uint8_t v) {
    s.cr = v & 0x3F;
    if (v & 0x20) {
        s.irq2 = false;
        // Set/reset: Cx2 follows bit 3; the strobe modes start high
        s.c2Out = (v & 0x10) ? (v & 0x08) != 0 : true;
    } else {
        s.c2Out = true;
        updateC2(s);
    }
    if (s.onControl) s.onControl();
    irqChanged();
}

uint8_t Mc6821::read(uint16_t addr) {
    switch (addr & 3) {
        case 0: {
            if (!(a.cr & 4)) return a.ddr;
            if (a.preRead) a.preRead();
            const uint8_t v = portA();
            const bool had = irqA();
            a.irq1 = a.irq2 = false;
            strobe(a);
            if (had) irqChanged();
            return v;
        }
        case 1:
            return (uint8_t)(a.cr | (a.irq1 ? 0x80 : 0) | (a.irq2 ? 0x40 : 0));
        case 2: {
            if (!(b.cr & 4)) return b.ddr;
            if (b.preRead) b.preRead();
            const uint8_t v = (uint8_t)((b.out & b.ddr) | (portB() & ~b.ddr));
            const bool had = irqB();
            b.irq1 = b.irq2 = false;
            if (had) irqChanged();
            return v;
        }
        default:
            return (uint8_t)(b.cr | (b.irq1 ? 0x80 : 0) | (b.irq2 ? 0x40 : 0));
    }
}

uint8_t Mc6821::peek(uint16_t addr) const {
    switch (addr & 3) {
        case 0: return (a.cr & 4) ? portA() : a.ddr;
        case 1: return (uint8_t)(a.cr | (a.irq1 ? 0x80 : 0) | (a.irq2 ? 0x40 : 0));
        case 2: return (b.cr & 4) ? (uint8_t)((b.out & b.ddr) | (portB() & ~b.ddr)) : b.ddr;
        default: return (uint8_t)(b.cr | (b.irq1 ? 0x80 : 0) | (b.irq2 ? 0x40 : 0));
    }
}

void Mc6821::write(uint16_t addr, uint8_t v) {
    switch (addr & 3) {
        case 0:
            if (a.cr & 4) a.out = v; else a.ddr = v;
            if (a.postWrite) a.postWrite();
            break;
        case 1:
            writeCr(a, v);
            break;
        case 2:
            if (b.cr & 4) {
                b.out = v;
                if (b.postWrite) b.postWrite();
                strobe(b);
            } else {
                b.ddr = v;
                if (b.postWrite) b.postWrite();
            }
            break;
        default:
            writeCr(b, v);
            break;
    }
}

namespace {
template <class T> void put(std::vector<uint8_t>& v, const T& x) {
    const auto* p = reinterpret_cast<const uint8_t*>(&x);
    v.insert(v.end(), p, p + sizeof(T));
}
template <class T> void get(const uint8_t*& p, const uint8_t* end, T& x) {
    if (p + sizeof(T) > end) return;
    std::memcpy(&x, p, sizeof(T));
    p += sizeof(T);
}
}  // namespace

std::vector<uint8_t> Mc6821::saveState() const {
    std::vector<uint8_t> v;
    for (const Side* s : {&a, &b}) {
        put(v, s->cr); put(v, s->ddr); put(v, s->out); put(v, s->c1); put(v, s->c2);
        put(v, s->irq1); put(v, s->irq2); put(v, s->c2Out);
    }
    put(v, m_now);
    for (const Side* s : {&a, &b}) { put(v, s->irq1At); put(v, s->irq2At); }
    return v;
}

void Mc6821::loadState(const std::vector<uint8_t>& st) {
    const uint8_t* p = st.data();
    const uint8_t* e = p + st.size();
    for (Side* s : {&a, &b}) {
        get(p, e, s->cr); get(p, e, s->ddr); get(p, e, s->out); get(p, e, s->c1); get(p, e, s->c2);
        get(p, e, s->irq1); get(p, e, s->irq2); get(p, e, s->c2Out);
        s->irq1At = s->irq2At = 0;
    }
    m_now = 0;
    get(p, e, m_now);
    for (Side* s : {&a, &b}) { get(p, e, s->irq1At); get(p, e, s->irq2At); }
    if (a.postWrite) a.postWrite();
    if (b.postWrite) b.postWrite();
    if (a.onControl) a.onControl();
    if (b.onControl) b.onControl();
    irqChanged();
}
