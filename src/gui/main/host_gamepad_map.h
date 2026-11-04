#pragma once

// Host controller state to emulated joystick/pad input. Kept free of SDL and
// wx so the mapping can be unit-tested.

#include "libcore/main/machine_desc.h"
#include <cstdint>

/** One host controller's state, in a game-controller layout. */
struct HostPadState {
    bool up = false, down = false, left = false, right = false;   // D-pad or hat
    int16_t stickX = 0, stickY = 0;                               // left stick, y down
    bool south = false, east = false, west = false, north = false; // face buttons by position
    bool shoulderL = false, shoulderR = false, back = false, start = false;
};

/** Stick travel (of 32767) that counts as a direction. */
constexpr int HOST_STICK_DEADZONE = 16000;

/**
 * PAD_* bits for a host controller. The face buttons map by position to an
 * SNES pad (south = B, east = A, west = Y, north = X), and the stick adds to
 * the D-pad.
 */
inline uint32_t hostPadButtons(const HostPadState& s) {
    using M = MachineDescriptor;
    uint32_t b = 0;
    if (s.up    || s.stickY < -HOST_STICK_DEADZONE) b |= M::PAD_UP;
    if (s.down  || s.stickY >  HOST_STICK_DEADZONE) b |= M::PAD_DOWN;
    if (s.left  || s.stickX < -HOST_STICK_DEADZONE) b |= M::PAD_LEFT;
    if (s.right || s.stickX >  HOST_STICK_DEADZONE) b |= M::PAD_RIGHT;
    if (s.south) b |= M::PAD_B;
    if (s.east)  b |= M::PAD_A;
    if (s.west)  b |= M::PAD_Y;
    if (s.north) b |= M::PAD_X;
    if (s.shoulderL) b |= M::PAD_L;
    if (s.shoulderR) b |= M::PAD_R;
    if (s.back)  b |= M::PAD_SELECT;
    if (s.start) b |= M::PAD_START;
    return b;
}

/**
 * The emulated port for the host's n-th controller (from 0), or -1 if it has
 * none. Pads (onGamepad) go in order; CBM joysticks start at the machine's
 * primary port (the C64's control port 2), then the other. `swap` swaps the
 * first two.
 */
inline int hostPadPort(const MachineDescriptor& m, int slot, bool swap) {
    if (m.onGamepad) {
        int port = slot;
        if (swap && port < 2) port ^= 1;
        return port;
    }
    if (!m.onJoystick || m.joystickPorts <= 0) return -1;
    int n = m.joystickPorts;
    int port;
    if (slot == 0) port = m.primaryJoystickPort;
    else if (slot == 1 && n >= 2) port = m.primaryJoystickPort == 0 ? 1 : 0;
    else if (slot < n) port = slot;
    else return -1;
    if (swap && n >= 2 && port < 2) port ^= 1;
    return port;
}

/** Send PAD_* bits to a machine port, as a pad or a CBM joystick. */
inline void sendHostPad(MachineDescriptor& m, int port, uint32_t buttons) {
    if (port < 0) return;
    if (m.onGamepad) m.onGamepad(port, buttons);
    else if (m.onJoystick) m.onJoystick(port, MachineDescriptor::cbmJoystickBits(buttons));
}
