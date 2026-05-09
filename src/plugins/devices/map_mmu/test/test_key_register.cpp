#include "test_harness.h"
#include "plugins/devices/map_mmu/main/key_register.h"

TEST_CASE(key_register_c64_knock) {
    KeyRegister kr;
    uint8_t val;

    // Write $00, then $00 → C64
    kr.ioWrite(nullptr, 0xD02F, 0x00);
    kr.ioWrite(nullptr, 0xD02F, 0x00);
    ASSERT_EQ(kr.getCurrentPersonality(), IopersonalityMode::C64);
}

TEST_CASE(key_register_c65_knock) {
    KeyRegister kr;

    // Write $A5, then $96 → C65
    kr.ioWrite(nullptr, 0xD02F, 0xA5);
    kr.ioWrite(nullptr, 0xD02F, 0x96);
    ASSERT_EQ(kr.getCurrentPersonality(), IopersonalityMode::C65);
}

TEST_CASE(key_register_mega65_knock) {
    KeyRegister kr;

    // Write $47, then $53 → MEGA65
    kr.ioWrite(nullptr, 0xD02F, 0x47);
    kr.ioWrite(nullptr, 0xD02F, 0x53);
    ASSERT_EQ(kr.getCurrentPersonality(), IopersonalityMode::MEGA65);
}

TEST_CASE(key_register_ethernet_knock) {
    KeyRegister kr;

    // Write $45, then $54 → ETHERNET
    kr.ioWrite(nullptr, 0xD02F, 0x45);
    kr.ioWrite(nullptr, 0xD02F, 0x54);
    ASSERT_EQ(kr.getCurrentPersonality(), IopersonalityMode::ETHERNET);
}

TEST_CASE(key_register_invalid_sequence) {
    KeyRegister kr;
    kr.reset();

    // Invalid sequence should not change personality
    kr.ioWrite(nullptr, 0xD02F, 0xFF);
    kr.ioWrite(nullptr, 0xD02F, 0xFF);
    ASSERT_EQ(kr.getCurrentPersonality(), IopersonalityMode::C64);
}

TEST_CASE(key_register_partial_valid_sequence) {
    KeyRegister kr;
    kr.reset();

    // First byte valid but second byte wrong
    kr.ioWrite(nullptr, 0xD02F, 0xA5);
    kr.ioWrite(nullptr, 0xD02F, 0x00);
    // Should remain at C64
    ASSERT_EQ(kr.getCurrentPersonality(), IopersonalityMode::C64);
}

TEST_CASE(key_register_reset_state) {
    KeyRegister kr;

    // Start C65 sequence
    kr.ioWrite(nullptr, 0xD02F, 0xA5);
    // Reset before completing
    kr.reset();
    // Should be back at WAITING_FIRST
    kr.ioWrite(nullptr, 0xD02F, 0x00);
    kr.ioWrite(nullptr, 0xD02F, 0x00);
    ASSERT_EQ(kr.getCurrentPersonality(), IopersonalityMode::C64);
}

TEST_CASE(key_register_read_returns_last_written) {
    KeyRegister kr;
    uint8_t val;

    // Write some value
    kr.ioWrite(nullptr, 0xD02F, 0x42);
    // Read should return 0x42
    kr.ioRead(nullptr, 0xD02F, &val);
    ASSERT_EQ(val, 0x42);
}

TEST_CASE(key_register_read_before_write) {
    KeyRegister kr;
    uint8_t val;

    // Read before any write should return 0
    kr.ioRead(nullptr, 0xD02F, &val);
    ASSERT_EQ(val, 0x00);
}

TEST_CASE(key_register_multiple_sequences) {
    KeyRegister kr;

    // First sequence: C64
    kr.ioWrite(nullptr, 0xD02F, 0x00);
    kr.ioWrite(nullptr, 0xD02F, 0x00);
    ASSERT_EQ(kr.getCurrentPersonality(), IopersonalityMode::C64);

    // Second sequence: MEGA65
    kr.ioWrite(nullptr, 0xD02F, 0x47);
    kr.ioWrite(nullptr, 0xD02F, 0x53);
    ASSERT_EQ(kr.getCurrentPersonality(), IopersonalityMode::MEGA65);

    // Third sequence: C65
    kr.ioWrite(nullptr, 0xD02F, 0xA5);
    kr.ioWrite(nullptr, 0xD02F, 0x96);
    ASSERT_EQ(kr.getCurrentPersonality(), IopersonalityMode::C65);
}

TEST_CASE(key_register_callback) {
    KeyRegister kr;
    IopersonalityMode callbackMode = IopersonalityMode::C64;
    bool callbackFired = false;

    kr.setPersonalityChangeCallback([&](IopersonalityMode mode) {
        callbackMode = mode;
        callbackFired = true;
    });

    // Write valid MEGA65 sequence
    kr.ioWrite(nullptr, 0xD02F, 0x47);
    kr.ioWrite(nullptr, 0xD02F, 0x53);

    ASSERT(callbackFired);
    ASSERT_EQ(callbackMode, IopersonalityMode::MEGA65);
}

TEST_CASE(key_register_callback_invalid_no_fire) {
    KeyRegister kr;
    bool callbackFired = false;

    kr.setPersonalityChangeCallback([&](IopersonalityMode mode) {
        callbackFired = true;
    });

    // Write invalid sequence
    kr.ioWrite(nullptr, 0xD02F, 0xFF);
    kr.ioWrite(nullptr, 0xD02F, 0xFF);

    ASSERT(!callbackFired);
}

TEST_CASE(key_register_state_machine_reset) {
    KeyRegister kr;

    // Write first byte
    kr.ioWrite(nullptr, 0xD02F, 0xA5);
    // Write wrong second byte
    kr.ioWrite(nullptr, 0xD02F, 0x00);
    // Now should be back to WAITING_FIRST
    // Try new sequence
    kr.ioWrite(nullptr, 0xD02F, 0x00);
    kr.ioWrite(nullptr, 0xD02F, 0x00);
    ASSERT_EQ(kr.getCurrentPersonality(), IopersonalityMode::C64);
}

TEST_CASE(key_register_initial_state) {
    KeyRegister kr;
    // Initial personality should be C64
    ASSERT_EQ(kr.getCurrentPersonality(), IopersonalityMode::C64);
}
