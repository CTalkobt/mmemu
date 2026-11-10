#include "cpu6510.h"
#include "mmemu_plugin_api.h"
#include <cstdio>

MOS6510::MOS6510() {
    // Standard C64 pull-up state: DDR=0x00, DATA=0x3F.
    // Port only has 6 bits (0-5).
    m_ddr  = 0x00;
    m_data = m_pinMask;
    m_dataOut = 0x3F;
    m_cap = 0;
    updateSignals();
}

MOS6510::~MOS6510() {
    delete m_portBus;
}

int MOS6510::step() {
    return MOS6502::step();
}

void MOS6510::reset() {
    // Restore power-on port state BEFORE base reset reads the reset vector.
    // The base reset reads $FFFC/$FFFD through the bus, which goes through the
    // C64 PLA.  The PLA decides whether to serve KERNAL ROM based on the
    // HIRAM/LORAM/CHAREN signal lines, which are driven by the 6510 I/O port.
    // If we update signals after the base reset, the PLA may see a stale (post-
    // program) port state and route $FFFC to RAM instead of the KERNAL, giving
    // the wrong reset vector.
    m_ddr  = 0x00;
    m_data = 0x3F;
    m_dataOut = 0x3F;
    m_cap = 0;
    updateSignals();
    MOS6502::reset();
}

// $01 as read, after VICE (c64pla.c, c64mem.c zero_read): an output reads
// its latch; an input with a pull-up (bits 0-2, 4) reads 1 unless held low
// (the tape sense); bit 5 as an input reads 0; bit 3 as an input reads what
// was last driven on it; bits 6 and 7, unconnected, read their fading charge
uint8_t MOS6510::portRead() {
    uint8_t level = (uint8_t)((m_data | ~m_ddr) & (m_dataOut | 0x17));
    if (!(m_ddr & 0x20)) level &= (uint8_t)~0x20;
    if (!(m_ddr & 0x10) && !m_cassetteSense.get()) level &= (uint8_t)~0x10;
    if (m_pinMask & 0x40) {                     // the 8502's bit 6: CAPS LOCK, pulled up
        if (!(m_ddr & 0x40)) {
            level |= 0x40;
            if (m_bit6 && !m_bit6->get()) level &= (uint8_t)~0x40;
        }
    }
    const uint64_t now = cycles();
    for (int b = 6; b < 8; ++b) {
        const uint8_t bit = (uint8_t)(1 << b);
        if (!(unconnected() & bit)) continue;
        if ((m_cap & bit) && now > m_capFall[b]) m_cap &= (uint8_t)~bit;
        if (!(m_ddr & bit)) level = (uint8_t)((level & ~bit) | (m_cap & bit));
    }
    return level;
}

// Charge the unconnected pins in `bits` to `values` (they then fall off)
void MOS6510::charge(uint8_t bits, uint8_t values) {
    bits &= unconnected();
    for (int b = 6; b < 8; ++b) {
        const uint8_t bit = (uint8_t)(1 << b);
        if (!(bits & bit)) continue;
        m_cap = (uint8_t)((m_cap & ~bit) | (values & bit));
        m_capFall[b] = cycles() + FALL_OFF_CYCLES;
    }
}

void MOS6510::ddrWrite(uint8_t val) {
    // An unconnected pin switched from output to input keeps its charge
    charge((uint8_t)(m_ddr & (m_ddr ^ val)), m_data);
    m_ddr = val;                                // all 8 bits read back
    m_dataOut = (uint8_t)((m_dataOut & ~m_ddr) | (m_data & m_ddr));
    updateSignals();
}

void MOS6510::dataWrite(uint8_t val) {
    charge(m_ddr, val);                         // outputs charge their pins
    m_data = val;
    m_dataOut = (uint8_t)((m_dataOut & ~m_ddr) | (m_data & m_ddr));
    updateSignals();
}

void MOS6510::installBus(IBus* bus) {
    if (m_realBus == bus && m_portBus) return;
    
    m_realBus = bus;
    delete m_portBus;
    m_portBus = nullptr;

    if (bus) {
        m_portBus = new PortBus(this, bus);
        // Pass the proxy to the 6502 core so all CPU reads/writes go through it.
        MOS6502::setDataBus(m_portBus);
        MOS6502::setCodeBus(m_portBus);
    } else {
        MOS6502::setDataBus(nullptr);
        MOS6502::setCodeBus(nullptr);
    }
}

void MOS6510::setDataBus(IBus* bus) {
    installBus(bus);
}

void MOS6510::setCodeBus(IBus* bus) {
    installBus(bus);
}

void MOS6510::updateSignals() {
    // Effective port level: output bits driven by DATA, input bits pulled high.
    // Port only has 6 bits (0-5).
    uint8_t level = (m_data & m_ddr) | (~m_ddr & 0x3F);
    m_loram .set((level >> 0) & 1);
    m_hiram .set((level >> 1) & 1);
    m_charen.set((level >> 2) & 1);
    m_cassetteWrite.set((level >> 3) & 1);
    m_cassetteMotor.set((level >> 5) & 1);
}
