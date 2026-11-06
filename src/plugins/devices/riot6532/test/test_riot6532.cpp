#include "test_harness.h"
#include "plugins/devices/riot6532/main/riot6532.h"

// RAM, the timer at each interval and its wrap (Stella M6532.cxx)
TEST_CASE(riot6532_timer_and_ram) {
    Riot6532 r;
    r.write(0x80, 0x42);
    EXPECT_EQ(r.read(0x80), 0x42);
    EXPECT_EQ(r.read(0x180), 0x42);                // A8 ignored: RAM mirrors
    uint64_t t = 100;
    r.sync(t);
    r.write(0x296, 10);                            // TIM64T = 10
    r.sync(t + 1);
    EXPECT_EQ(r.read(0x284), 9);                   // it counts from the next cycle
    r.sync(t + 1 + 64 * 9);
    EXPECT_EQ(r.read(0x284), 0);
    r.sync(t + 1 + 64 * 10);                       // wrapped: $FF, flag set, per cycle
    EXPECT_EQ(r.peek(0x285) & 0x80, 0x80);
    EXPECT_EQ(r.peek(0x284), 0xFF);
    r.sync(t + 1 + 64 * 10 + 5);
    EXPECT_EQ(r.read(0x284), 0xFA);
    EXPECT_EQ(r.peek(0x285) & 0x80, 0x00);         // INTIM read cleared it
    r.write(0x295, 3);                             // TIM8T
    r.sync(t + 1 + 64 * 10 + 5 + 1 + 8);
    EXPECT_EQ(r.read(0x284), 1);
}

// The ports: pins through the data direction registers
TEST_CASE(riot6532_ports) {
    Riot6532 r;
    r.setPortA([] { return (uint8_t)0xEF; });       // left joystick right pressed
    r.setPortB([] { return (uint8_t)0x0B; });       // console switches
    EXPECT_EQ(r.read(0x280), 0xEF);
    EXPECT_EQ(r.read(0x282), 0x0B);
    r.write(0x281, 0x0F);                          // low nibble outputs
    r.write(0x280, 0x05);
    EXPECT_EQ(r.read(0x280), 0xE5);
    EXPECT_EQ(r.read(0x281), 0x0F);
}
