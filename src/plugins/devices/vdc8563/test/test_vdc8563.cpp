#include "test_harness.h"
#include "plugins/devices/vdc8563/main/vdc8563.h"

namespace {
void reg(Vdc8563& v, int r, uint8_t val) { v.ioWrite(nullptr, 0xD600, (uint8_t)r); v.ioWrite(nullptr, 0xD601, val); }
uint8_t get(Vdc8563& v, int r) { uint8_t x = 0; v.ioWrite(nullptr, 0xD600, (uint8_t)r); v.ioRead(nullptr, 0xD601, &x); return x; }
}  // namespace

// Status, register read-back masks (VICE vdc-mem.c regmask), mirrors
TEST_CASE(vdc_status_and_registers) {
    Vdc8563 v;
    uint8_t s = 0;
    v.ioRead(nullptr, 0xD600, &s);
    EXPECT_EQ(s & 0x87, 0x82);                     // ready, revision 2
    reg(v, 0, 0x7E);
    EXPECT_EQ(get(v, 0), 0x7E);
    reg(v, 5, 0x00);
    EXPECT_EQ(get(v, 5), 0xE0);                    // unused bits read 1
    reg(v, 36, 0x00);
    EXPECT_EQ(get(v, 36), 0xF0);
    EXPECT_EQ(get(v, 40), 0xFF);                   // no such register
    v.ioWrite(nullptr, 0xD6FE, 12);                // $D6FE mirrors $D600
    v.ioWrite(nullptr, 0xD6FF, 0x55);
    EXPECT_EQ(v.reg(12), 0x55);
}

// R31 writes/reads RAM at R18/R19 and advances; R30 fills or copies
TEST_CASE(vdc_memory_fill_copy) {
    Vdc8563 v;
    reg(v, 18, 0x20); reg(v, 19, 0x00);
    reg(v, 31, 0xAA); reg(v, 31, 0xBB);
    EXPECT_EQ(v.ramByte(0x2000), 0xAA);
    EXPECT_EQ(v.ramByte(0x2001), 0xBB);
    EXPECT_EQ(v.reg(19), 0x02);
    reg(v, 18, 0x20); reg(v, 19, 0x00);
    EXPECT_EQ(get(v, 31), 0xAA);                   // read advances too
    EXPECT_EQ(get(v, 31), 0xBB);
    // Fill 16 bytes of $11 at $1000
    reg(v, 18, 0x10); reg(v, 19, 0x00);
    reg(v, 24, 0x00); reg(v, 31, 0x11); reg(v, 30, 15);   // R31 write stores one, fill 15 more
    EXPECT_EQ(v.ramByte(0x1000), 0x11);
    EXPECT_EQ(v.ramByte(0x100F), 0x11);
    EXPECT_EQ(v.ramByte(0x1010), 0x00);
    EXPECT_EQ(v.reg(19), 0x10);
    // Copy $2000-$2001 to $3000
    reg(v, 18, 0x30); reg(v, 19, 0x00);
    reg(v, 32, 0x20); reg(v, 33, 0x00);
    reg(v, 24, 0x80); reg(v, 30, 2);
    EXPECT_EQ(v.ramByte(0x3000), 0xAA);
    EXPECT_EQ(v.ramByte(0x3001), 0xBB);
    EXPECT_EQ(v.reg(33), 0x02);
    // 16 KB: addresses wrap at $4000
    reg(v, 18, 0x40); reg(v, 19, 0x00); reg(v, 31, 0x77);
    EXPECT_EQ(v.ramByte(0x0000), 0x77);
}
