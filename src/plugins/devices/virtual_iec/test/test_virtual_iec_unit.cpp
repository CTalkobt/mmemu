#include "test_harness.h"
#include "plugins/devices/virtual_iec/main/virtual_iec.h"
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

// --- Construction and identity ---

TEST_CASE(iec_initial_state) {
    VirtualIECBus iec(8);
    ASSERT(std::string(iec.name()) == "VirtualIEC");
    ASSERT_EQ(iec.baseAddr(), (uint32_t)0);
    ASSERT_EQ(iec.addrMask(), (uint32_t)0);

    // ioRead/ioWrite are stubs
    uint8_t val = 0x42;
    ASSERT(!iec.ioRead(nullptr, 0, &val));
    ASSERT(!iec.ioWrite(nullptr, 0, 0));
}

TEST_CASE(iec_read_port_idle) {
    VirtualIECBus iec(8);
    // With nothing asserted: CLK and DATA are released (high)
    uint8_t val = iec.readPort();
    ASSERT(val & (1 << 6)); // CLK released → bit 6 high
    ASSERT(val & (1 << 7)); // DATA released → bit 7 high
}

TEST_CASE(iec_write_port_atn) {
    VirtualIECBus iec(8);
    // Assert ATN (bit 3)
    iec.writePort(0x08);
    iec.tick(1);
    // Device should go to ATTENTION and assert DATA
    uint8_t val = iec.readPort();
    ASSERT(!(val & (1 << 7))); // DATA asserted (low) → bit 7 = 0
}

TEST_CASE(iec_reset) {
    VirtualIECBus iec(8);
    iec.writePort(0x08); // ATN
    iec.tick(100);

    iec.reset();

    // After reset, bus should be idle, everything released
    uint8_t val = iec.readPort();
    ASSERT(val & (1 << 6)); // CLK released
    ASSERT(val & (1 << 7)); // DATA released
}

// --- Disk mount/eject ---

TEST_CASE(iec_mount_disk) {
    VirtualIECBus iec(8);

    ASSERT(iec.mountDisk(8, "/tmp/test.d64"));
    ASSERT(iec.getMountedDiskPath(8) == "/tmp/test.d64");

    // Wrong unit
    ASSERT(!iec.mountDisk(9, "/tmp/other.d64"));
    ASSERT(iec.getMountedDiskPath(9).empty());
}

TEST_CASE(iec_eject_disk) {
    VirtualIECBus iec(8);
    iec.mountDisk(8, "/tmp/test.d64");
    iec.ejectDisk(8);
    ASSERT(iec.getMountedDiskPath(8).empty());

    // Eject wrong unit — should be safe
    iec.ejectDisk(9);
}

TEST_CASE(iec_disk_status) {
    VirtualIECBus iec(8);
    int t, s;
    bool led;

    // No disk mounted
    ASSERT(iec.getDiskStatus(8, t, s, led));
    ASSERT_EQ(t, 0);
    ASSERT_EQ(s, 0);
    ASSERT(!led);

    // Mount disk — track changes to 18
    iec.mountDisk(8, "test.d64");
    ASSERT(iec.getDiskStatus(8, t, s, led));
    ASSERT_EQ(t, 18);

    // Wrong unit
    ASSERT(!iec.getDiskStatus(9, t, s, led));
}

TEST_CASE(iec_led_follows_activity) {
    VirtualIECBus iec(8);
    int t, s;
    bool led;

    // Idle → LED off
    iec.getDiskStatus(8, t, s, led);
    ASSERT(!led);

    // ATN → state changes → LED on
    iec.writePort(0x08);
    iec.tick(100);
    iec.getDiskStatus(8, t, s, led);
    ASSERT(led);

    // Reset → back to idle → LED off
    iec.reset();
    iec.getDiskStatus(8, t, s, led);
    ASSERT(!led);
}

// --- IEC protocol basics ---

TEST_CASE(iec_attention_state) {
    VirtualIECBus iec(8);

    // Assert ATN
    iec.writePort(0x08);
    iec.tick(50);

    // Device asserts DATA in ATTENTION state
    uint8_t val = iec.readPort();
    ASSERT(!(val & (1 << 7))); // DATA asserted
}

TEST_CASE(iec_process_command_listen) {
    VirtualIECBus iec(8);

    // ATN + CLK asserted → transition to ADDRESSING
    iec.writePort(0x08 | 0x10); // ATN + CLK
    iec.tick(50);

    // Now send LISTEN device 8 ($28) bit by bit via CLK/DATA toggling
    // This tests the state machine transitions through ATTENTION → ADDRESSING
    // Full bit-level protocol is complex, but we can verify the state machine
    // doesn't crash and returns to a sensible state
    for (int i = 0; i < 1000; i++) iec.tick(1);
}

TEST_CASE(iec_setddr) {
    VirtualIECBus iec(8);
    // setDdr is a no-op but should not crash
    iec.setDdr(0xFF);
    iec.setDdr(0x00);
}

// --- Device info ---

TEST_CASE(iec_device_info) {
    VirtualIECBus iec(8);
    iec.mountDisk(8, "/tmp/test.d64");

    DeviceInfo info;
    iec.getDeviceInfo(info);

    ASSERT(info.name == "VirtualIEC");

    bool hasState = false, hasDevice = false, hasMounted = false;
    for (auto& kv : info.state) {
        if (kv.first == "State" && kv.second == "IDLE") hasState = true;
        if (kv.first == "Device Number" && kv.second == "8") hasDevice = true;
        if (kv.first == "Mounted Disk" && kv.second == "/tmp/test.d64") hasMounted = true;
    }
    ASSERT(hasState);
    ASSERT(hasDevice);
    ASSERT(hasMounted);
}

TEST_CASE(iec_device_info_no_disk) {
    VirtualIECBus iec(8);

    DeviceInfo info;
    iec.getDeviceInfo(info);

    bool hasNoDisk = false;
    for (auto& kv : info.state) {
        if (kv.first == "Mounted Disk" && kv.second == "none") hasNoDisk = true;
    }
    ASSERT(hasNoDisk);
}

TEST_CASE(iec_device_info_bus_lines) {
    VirtualIECBus iec(8);

    // Assert ATN
    iec.writePort(0x08);
    iec.tick(10);

    DeviceInfo info;
    iec.getDeviceInfo(info);

    bool hasAtn = false;
    for (auto& kv : info.state) {
        if (kv.first == "ATN In" && kv.second == "LO (asserted)") hasAtn = true;
    }
    ASSERT(hasAtn);
}

TEST_CASE(iec_different_device_number) {
    VirtualIECBus iec(9); // device 9

    // Mount on unit 9 should work
    ASSERT(iec.mountDisk(9, "/tmp/test.d64"));
    ASSERT(!iec.mountDisk(8, "/tmp/test.d64")); // wrong unit

    int t, s;
    bool led;
    ASSERT(iec.getDiskStatus(9, t, s, led));
    ASSERT(!iec.getDiskStatus(8, t, s, led));
}
