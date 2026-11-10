#pragma once

#include <cstdint>

/**
 * A pointing device fitted at run time (a C64's 1351 mouse): the host's
 * mouse moves it; its buttons show as joystick switches (active low:
 * bit 0 up, 1 down, 2 left, 3 right, 4 fire).
 */
class IPointerDevice {
public:
    virtual ~IPointerDevice() = default;
    /** Host motion (picture pixels; dy > 0 is down) and buttons (bit 0 left, 1 right). */
    virtual void pointer(int dx, int dy, uint8_t buttons) = 0;
    virtual uint8_t joystickBits() const = 0;
};

/** What a SID's POTX/POTY read (paddles, a 1351 in proportional mode). */
class IPotSource {
public:
    virtual ~IPotSource() = default;
    virtual uint8_t pot(int n) const = 0;   // 0 = POTX, 1 = POTY
};
