#include "test_harness.h"
#include "mega65_math.h"
#include <cstring>

TEST_CASE(mega65_math_multiplication) {
    Mega65MathDevice math(0xD700);
    math.reset();

    // Multiply 100 * 200 = 20000 ($4E20)
    // MULTINA at $D770
    math.ioWrite(nullptr, 0xD770, 100);
    math.ioWrite(nullptr, 0xD771, 0);
    math.ioWrite(nullptr, 0xD772, 0);
    math.ioWrite(nullptr, 0xD773, 0);

    // MULTINB at $D774
    math.ioWrite(nullptr, 0xD774, 200);
    math.ioWrite(nullptr, 0xD775, 0);
    math.ioWrite(nullptr, 0xD776, 0);
    math.ioWrite(nullptr, 0xD777, 0);

    // Result in MULTOUT at $D778 (64-bit)
    uint8_t val;
    math.ioRead(nullptr, 0xD778, &val);
    ASSERT_EQ(val, 0x20);
    math.ioRead(nullptr, 0xD779, &val);
    ASSERT_EQ(val, 0x4E);
    math.ioRead(nullptr, 0xD77A, &val);
    ASSERT_EQ(val, 0x00);
    math.ioRead(nullptr, 0xD77B, &val);
    ASSERT_EQ(val, 0x00);
}

TEST_CASE(mega65_math_division) {
    Mega65MathDevice math(0xD700);
    math.reset();

    // Divide 1000 / 3 = 333 remainder 1
    // DIVINA at $D760 (1000 = $03E8)
    math.ioWrite(nullptr, 0xD760, 0xE8);
    math.ioWrite(nullptr, 0xD761, 0x03);
    math.ioWrite(nullptr, 0xD762, 0x00);
    math.ioWrite(nullptr, 0xD763, 0x00);

    // DIVINB at $D764 (3)
    math.ioWrite(nullptr, 0xD764, 0x03);
    math.ioWrite(nullptr, 0xD765, 0x00);
    math.ioWrite(nullptr, 0xD766, 0x00);
    math.ioWrite(nullptr, 0xD767, 0x00);

    // Quotient in DIVOUT at $D768 (333 = $014D)
    uint8_t val;
    math.ioRead(nullptr, 0xD768, &val);
    ASSERT_EQ(val, 0x4D);
    math.ioRead(nullptr, 0xD769, &val);
    ASSERT_EQ(val, 0x01);
    math.ioRead(nullptr, 0xD76A, &val);
    ASSERT_EQ(val, 0x00);
    math.ioRead(nullptr, 0xD76B, &val);
    ASSERT_EQ(val, 0x00);

    // Remainder in MULTINA at $D770 (1)
    math.ioRead(nullptr, 0xD770, &val);
    ASSERT_EQ(val, 0x01);
}

TEST_CASE(mega65_math_divzero) {
    Mega65MathDevice math(0xD700);
    math.reset();

    // Divide 1000 / 0
    math.ioWrite(nullptr, 0xD760, 0xE8);
    math.ioWrite(nullptr, 0xD761, 0x03);
    math.ioWrite(nullptr, 0xD762, 0x00);
    math.ioWrite(nullptr, 0xD763, 0x00);

    math.ioWrite(nullptr, 0xD764, 0x00);
    math.ioWrite(nullptr, 0xD765, 0x00);
    math.ioWrite(nullptr, 0xD766, 0x00);
    math.ioWrite(nullptr, 0xD767, 0x00);

    // Quotient should be all-ones
    uint8_t val;
    for (int i = 0; i < 8; ++i) {
        math.ioRead(nullptr, 0xD768 + i, &val);
        ASSERT_EQ(val, 0xFF);
    }
    
    // Remainder should be all-ones
    for (int i = 0; i < 4; ++i) {
        math.ioRead(nullptr, 0xD770 + i, &val);
        ASSERT_EQ(val, 0xFF);
    }
}

TEST_CASE(mega65_math_rng) {
    Mega65MathDevice math(0xD700);
    math.reset();

    uint8_t r1, r2;
    math.ioRead(nullptr, 0xD7EF, &r1);
    math.ioRead(nullptr, 0xD7EF, &r2);

    ASSERT_NE(r1, r2); // LFSR should advance
}
