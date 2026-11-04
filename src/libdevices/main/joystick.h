#pragma once

#include "iport_device.h"

/**
 * Standard 5-bit active-low joystick for VIC-20 and C64.
 * Mapping (active-low, 0 = pressed, 1 = idle):
 *   Bit 0: Up
 *   Bit 1: Down
 *   Bit 2: Left
 *   Bit 3: Right
 *   Bit 4: Fire
 *   Bits 5-7: Typically read as 1 on the VIC-20.
 */
class Joystick : public IPortDevice {
public:
    Joystick();
    virtual ~Joystick() = default;

    // IPortDevice interface
    uint8_t readPort() override;
    void writePort(uint8_t val) override;
    void setDdr(uint8_t ddr) override;

    /**
     * Sets the raw joystick state.
     * 1 = idle, 0 = pressed.
     */
    void setState(uint8_t bits);
    /** Bits 0-4: up, down, left, right, fire (0 = pressed). */
    uint8_t state() const { return m_state; }

private:
    uint8_t m_state; // Bits 0-4 are active-low
    uint8_t m_ddr;
};

/**
 * A joystick's switches on chosen lines of a port, for machines that spread
 * them over several ports (the VIC-20: up, down, left and fire on VIA1 port
 * A, right on VIA2 port B). Lines without a switch read 1.
 */
class JoystickPins : public IPortDevice {
public:
    /** bitFor[i]: the port bit for switch i (up, down, left, right, fire), or -1. */
    JoystickPins(const Joystick* joy, const int bitFor[5]) : m_joy(joy) {
        for (int i = 0; i < 5; i++) m_bitFor[i] = bitFor[i];
    }
    uint8_t readPort() override {
        uint8_t v = 0xFF, s = m_joy->state();
        for (int i = 0; i < 5; i++)
            if (m_bitFor[i] >= 0 && !(s & (1 << i))) v &= (uint8_t)~(1 << m_bitFor[i]);
        return v;
    }
    void writePort(uint8_t) override {}
    void setDdr(uint8_t) override {}

private:
    const Joystick* m_joy;
    int m_bitFor[5];
};
