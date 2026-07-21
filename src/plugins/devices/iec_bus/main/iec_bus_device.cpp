#include "iec_bus_device.h"
#include <cstdio>

IECBusDevice::IECBusDevice() : m_busState(0xFF) {
    // All lines pulled high initially (no device pulling them low)
}

uint8_t IECBusDevice::readPort() {
    if (!m_computerPortPtr) return 0xFF;

    uint8_t computerValue = *m_computerPortPtr;

    // Detect ATN transitions (computer setting/clearing ATN bit)
    if ((computerValue & ATN) != (m_lastComputerValue & ATN)) {
        fprintf(stderr, "[IEC] ATN %s\n",
            (computerValue & ATN) ? "released" : "asserted");

        if (!(computerValue & ATN)) {
            // ATN asserted - computer is trying to communicate
            m_handshakeState = 1;
            m_stableCount = 0;
        } else {
            // ATN released - handshake complete
            m_handshakeState = 0;
        }
    }
    m_lastComputerValue = computerValue;

    // Simulate IEC device response
    // Computer controls: DATA_OUT (bit 0), CLOCK_OUT (bit 1), ATN (bit 4)
    // Device simulates: CLOCK_IN (bit 2), DATA_IN (bit 3)

    // Build response: start with computer's outputs (bits 0,1,4 + other bits)
    // Device doesn't pull low on DATA_IN/CLOCK_IN during handshake
    uint8_t response = computerValue | CLOCK_IN | DATA_IN;

    // During handshake, device can acknowledge by clearing CLOCK_IN
    if (m_handshakeState == 1) {
        // Simple response: just let the bus stabilize
        // After a few reads, stop pulling lines to allow boot to proceed
        m_stableCount++;

        if (m_stableCount > 50) {
            // Device done acknowledging, release lines
            m_handshakeState = 2;
        } else if (m_stableCount > 20) {
            // Minimal acknowledgment - briefly hold CLOCK_IN low
            response &= ~CLOCK_IN;
        }
    }

    fprintf(stderr, "[IEC] readPort: computer=$%02X device response=$%02X (handshake=%d stable=%u)\n",
        computerValue, response, m_handshakeState, m_stableCount);

    return response;
}

void IECBusDevice::writePort(uint8_t data) {
    // Computer can write to DATA_OUT, CLOCK_OUT, ATN
    fprintf(stderr, "[IEC] writePort: $%02X\n", data);
}
