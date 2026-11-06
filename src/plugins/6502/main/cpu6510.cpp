#include "cpu6510.h"
#include "mmemu_plugin_api.h"
#include <cstdio>

MOS6510::MOS6510() {
    // Standard C64 pull-up state: DDR=0x00, DATA=0x3F.
    // Port only has 6 bits (0-5).
    m_ddr  = 0x00;
    m_data = m_pinMask;
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
    updateSignals();
    MOS6502::reset();
}

uint8_t MOS6510::portRead() const {
    // Standard 6510: bits 6-7 of the I/O port don't exist and usually read as 1.
    // Bits 0-5: Output bits (DDR=1) return the register value (DATA).
    //           Input bits (DDR=0) return the logic level on the pins (float high).
    uint8_t level = (m_data & m_ddr) | (~m_ddr & m_pinMask);
    if (!(m_ddr & 0x10)) { if (!m_cassetteSense.get()) level &= ~0x10; else level |= 0x10; }
    if ((m_pinMask & 0x40) && !(m_ddr & 0x40) && m_bit6 && !m_bit6->get()) level &= ~0x40;
    return level | (uint8_t)~m_pinMask;
}

void MOS6510::ddrWrite(uint8_t val) {
    m_ddr = val & m_pinMask; // 6 pins (7 on the 8502)
    updateSignals();
}

void MOS6510::dataWrite(uint8_t val) {
    m_data = val & m_pinMask;
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
