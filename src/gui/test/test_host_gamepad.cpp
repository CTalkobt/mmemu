#include "test_harness.h"
#include "gui/main/host_gamepad_map.h"

using M = MachineDescriptor;

TEST_CASE(host_pad_buttons_map_by_position) {
    HostPadState s;
    ASSERT_EQ((int)hostPadButtons(s), 0);
    s.stickX = 20000; s.stickY = -20000;            // up-right on the stick
    s.south = true; s.start = true;
    ASSERT_EQ((int)hostPadButtons(s), (int)(M::PAD_UP | M::PAD_RIGHT | M::PAD_B | M::PAD_START));
    HostPadState t;
    t.stickX = 8000;                                // inside the dead zone
    t.left = true; t.east = true; t.west = true; t.north = true; t.shoulderL = true;
    ASSERT_EQ((int)hostPadButtons(t), (int)(M::PAD_LEFT | M::PAD_A | M::PAD_Y | M::PAD_X | M::PAD_L));
    ASSERT_EQ((int)M::cbmJoystickBits(M::PAD_LEFT | M::PAD_A), 0x1F & ~0x04 & ~0x10);
    ASSERT_EQ((int)M::cbmJoystickBits(M::PAD_START), 0x1F);   // not a CBM button
}

TEST_CASE(host_pad_ports_follow_the_machine) {
    MachineDescriptor c64;
    int lastPort = -1, lastBits = -1;
    c64.onJoystick = [&](int p, uint8_t b) { lastPort = p; lastBits = b; };
    c64.joystickPorts = 2;
    c64.primaryJoystickPort = 1;
    ASSERT_EQ(hostPadPort(c64, 0, false), 1);       // first controller: control port 2
    ASSERT_EQ(hostPadPort(c64, 1, false), 0);
    ASSERT_EQ(hostPadPort(c64, 2, false), -1);
    ASSERT_EQ(hostPadPort(c64, 0, true), 0);        // swapped
    sendHostPad(c64, 1, M::PAD_UP | M::PAD_B);
    ASSERT_EQ(lastPort, 1);
    ASSERT_EQ(lastBits, 0x1F & ~0x01 & ~0x10);

    MachineDescriptor vic20;
    vic20.onJoystick = [](int, uint8_t) {};
    vic20.joystickPorts = 1;
    ASSERT_EQ(hostPadPort(vic20, 0, false), 0);
    ASSERT_EQ(hostPadPort(vic20, 0, true), 0);
    ASSERT_EQ(hostPadPort(vic20, 1, false), -1);

    MachineDescriptor x16;
    uint32_t padBits = 0;
    x16.onGamepad = [&](int, uint32_t b) { padBits = b; return true; };
    ASSERT_EQ(hostPadPort(x16, 3, false), 3);
    ASSERT_EQ(hostPadPort(x16, 1, true), 0);
    sendHostPad(x16, 0, M::PAD_START);
    ASSERT_EQ((int)padBits, (int)M::PAD_START);

    MachineDescriptor none;
    ASSERT_EQ(hostPadPort(none, 0, false), -1);
}
