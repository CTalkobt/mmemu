#include "test_harness.h"
#include "plugins/devices/mega65_rtc/main/mega65_rtc.h"

TEST_CASE(rtc_read_returns_valid_bcd) {
    Mega65Rtc rtc(0xD700);
    uint8_t vals[6];
    for (int i = 0; i < 6; ++i)
        rtc.ioRead(nullptr, 0xD710 + i, &vals[i]);

    // All registers should have valid BCD nibbles (0-9 each)
    for (int i = 0; i < 6; ++i) {
        uint8_t lo = vals[i] & 0x0F;
        uint8_t hi = (i == 2) ? ((vals[i] >> 4) & 0x07) : (vals[i] >> 4);
        if (lo > 9 || hi > 9)
            fprintf(stderr, "  RTC reg %d: raw=$%02X lo=%d hi=%d\n", i, vals[i], lo, hi);
        ASSERT(lo <= 9);
        ASSERT(hi <= 9);
    }
}

TEST_CASE(rtc_write_sets_offset_flag) {
    Mega65Rtc rtc(0xD700);
    // Writing any RTC register should work without crashing
    rtc.ioWrite(nullptr, 0xD710, 0x00);  // seconds
    rtc.ioWrite(nullptr, 0xD711, 0x30);  // minutes
    rtc.ioWrite(nullptr, 0xD712, 0x12);  // hours
    rtc.ioWrite(nullptr, 0xD713, 0x15);  // day
    rtc.ioWrite(nullptr, 0xD714, 0x06);  // month
    rtc.ioWrite(nullptr, 0xD715, 0x26);  // year
    // Reads should return valid BCD (not crash or return garbage)
    uint8_t val;
    rtc.ioRead(nullptr, 0xD710, &val);
    ASSERT((val & 0x0F) <= 9);
}

TEST_CASE(rtc_nvram_read_write) {
    Mega65Rtc rtc(0xD700);
    rtc.ioWrite(nullptr, 0xD740, 0xAB);
    rtc.ioWrite(nullptr, 0xD77F, 0xCD);
    uint8_t v1, v2;
    rtc.ioRead(nullptr, 0xD740, &v1);
    rtc.ioRead(nullptr, 0xD77F, &v2);
    ASSERT_EQ((int)v1, 0xAB);
    ASSERT_EQ((int)v2, 0xCD);
}

TEST_CASE(rtc_reset_clears_nvram) {
    Mega65Rtc rtc(0xD700);
    rtc.ioWrite(nullptr, 0xD740, 0x42);
    rtc.reset();
    uint8_t val;
    rtc.ioRead(nullptr, 0xD740, &val);
    ASSERT_EQ((int)val, 0);
}

TEST_CASE(rtc_reserved_returns_zero) {
    Mega65Rtc rtc(0xD700);
    uint8_t val = 0xFF;
    rtc.ioRead(nullptr, 0xD716, &val);
    ASSERT_EQ((int)val, 0);
}

TEST_CASE(rtc_wrong_address_not_handled) {
    Mega65Rtc rtc(0xD700);
    uint8_t val;
    ASSERT(!rtc.ioRead(nullptr, 0xD600, &val));
}
