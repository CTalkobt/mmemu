#pragma once

#include "libdevices/main/io_handler.h"
#include "libdevices/main/iport_device.h"
#include <cstdint>
#include <functional>
#include <string>

/**
 * Minimal IEC Bus Device Simulator
 *
 * Simulates IEC bus response for MEGA65 boot sequence.
 * The IEC bus uses 6 signal lines (typically on CIA2 port A at $DD00):
 *   Bit 0: DATA OUT (computer controls)
 *   Bit 1: CLOCK OUT (computer controls)
 *   Bit 2: CLOCK IN (device pulls low)
 *   Bit 3: DATA IN (device pulls low)
 *   Bit 4: ATN (computer controls)
 *   Bit 5: ? (unused or secondary function)
 *
 * Devices pull lines LOW; pull-ups make them HIGH when released.
 *
 * This implementation:
 * - Tracks ATN (bit 4) to detect when computer is trying to communicate
 * - Simulates minimal device response to handshake
 * - Allows bus to stabilize for boot polling loops
 */
class IECBusDevice : public IPortDevice {
public:
    IECBusDevice();

    // IPortDevice interface
    uint8_t readPort() override;
    void writePort(uint8_t data) override;

    // Set reference to CIA2 port A for reading the computer's outputs
    void setComputerPortRef(uint8_t* portAPtr) { m_computerPortPtr = portAPtr; }

private:
    uint8_t* m_computerPortPtr = nullptr;
    uint8_t m_lastComputerValue = 0xFF;
    uint8_t m_busState = 0xFF;  // Default state: all lines pulled high
    int m_handshakeState = 0;   // Track handshake progress
    uint32_t m_stableCount = 0; // Count stable reads

    // IEC bus bit positions
    static constexpr uint8_t DATA_OUT  = 0x01;
    static constexpr uint8_t CLOCK_OUT = 0x02;
    static constexpr uint8_t CLOCK_IN  = 0x04;
    static constexpr uint8_t DATA_IN   = 0x08;
    static constexpr uint8_t ATN       = 0x10;
};
