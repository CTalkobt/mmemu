#pragma once

#include "libdevices/main/io_handler.h"
#include "libdevices/main/ipointer.h"
#include <string>

/**
 * Commodore 1351 mouse in proportional mode (VICE mouse_1351.c): its
 * position, modulo 64, goes to the SID's POTX/POTY as bits 1-6 (bit 0 is
 * noise; kept 0); the left button is the joystick's fire, the right
 * button its up. Moving the mouse up raises Y. The host's picture pixels
 * count one step each (VICE: 2 pixels per step on its 2x screen).
 */
class Mouse1351 : public IOHandler, public IPointerDevice, public IPotSource {
public:
    const char* name() const override { return m_name.c_str(); }
    void setName(const std::string& n) override { m_name = n; }
    uint32_t baseAddr() const override { return 0; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void reset() override {}
    void tick(uint64_t) override {}

    void pointer(int dx, int dy, uint8_t buttons) override {
        m_x += dx;
        m_y -= dy;                          // up is +
        m_buttons = buttons;
    }
    uint8_t joystickBits() const override {
        uint8_t b = 0x1F;
        if (m_buttons & 1) b &= (uint8_t)~0x10; // left: fire
        if (m_buttons & 2) b &= (uint8_t)~0x01; // right: up
        return b;
    }
    uint8_t pot(int n) const override { return (uint8_t)(((n ? m_y : m_x) & 0x3F) << 1); }

private:
    std::string m_name = "1351 Mouse";
    int m_x = 0, m_y = 0;
    uint8_t m_buttons = 0;
};
