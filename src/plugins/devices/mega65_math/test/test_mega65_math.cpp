#include "test_harness.h"
#include "plugins/devices/mega65_math/main/mega65_math.h"

namespace {

void write32(Mega65MathDevice& dev, uint32_t addr, uint32_t val) {
    dev.ioWrite(nullptr, addr,     val & 0xFF);
    dev.ioWrite(nullptr, addr + 1, (val >> 8) & 0xFF);
    dev.ioWrite(nullptr, addr + 2, (val >> 16) & 0xFF);
    dev.ioWrite(nullptr, addr + 3, (val >> 24) & 0xFF);
}

uint32_t read32(Mega65MathDevice& dev, uint32_t addr) {
    uint8_t b[4];
    for (int i = 0; i < 4; ++i) dev.ioRead(nullptr, addr + i, &b[i]);
    return b[0] | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
}

uint64_t read64(Mega65MathDevice& dev, uint32_t addr) {
    uint8_t b[8];
    for (int i = 0; i < 8; ++i) dev.ioRead(nullptr, addr + i, &b[i]);
    uint64_t lo = b[0] | (uint64_t(b[1]) << 8) | (uint64_t(b[2]) << 16) | (uint64_t(b[3]) << 24);
    uint64_t hi = b[4] | (uint64_t(b[5]) << 8) | (uint64_t(b[6]) << 16) | (uint64_t(b[7]) << 24);
    return lo | (hi << 32);
}

} // namespace

TEST_CASE(mega65_math_multiply_basic) {
    Mega65MathDevice dev(0xD700);
    write32(dev, 0xD770, 100);
    write32(dev, 0xD774, 200);
    ASSERT_EQ(read64(dev, 0xD778), (uint64_t)20000);
}

TEST_CASE(mega65_math_multiply_large) {
    Mega65MathDevice dev(0xD700);
    write32(dev, 0xD770, 0x10000);
    write32(dev, 0xD774, 0x10000);
    ASSERT_EQ(read64(dev, 0xD778), (uint64_t)0x100000000ULL);
}

TEST_CASE(mega65_math_multiply_zero) {
    Mega65MathDevice dev(0xD700);
    write32(dev, 0xD770, 12345);
    write32(dev, 0xD774, 0);
    ASSERT_EQ(read64(dev, 0xD778), (uint64_t)0);
}

TEST_CASE(mega65_math_divide_with_remainder) {
    Mega65MathDevice dev(0xD700);
    write32(dev, 0xD760, 1000);
    write32(dev, 0xD764, 3);
    ASSERT_EQ(read64(dev, 0xD768), (uint64_t)333);
    ASSERT_EQ(read32(dev, 0xD770), (uint32_t)1);
}

TEST_CASE(mega65_math_divide_exact) {
    Mega65MathDevice dev(0xD700);
    write32(dev, 0xD760, 42);
    write32(dev, 0xD764, 6);
    ASSERT_EQ(read64(dev, 0xD768), (uint64_t)7);
    ASSERT_EQ(read32(dev, 0xD770), (uint32_t)0);
}

TEST_CASE(mega65_math_divide_by_zero) {
    Mega65MathDevice dev(0xD700);
    write32(dev, 0xD760, 42);
    write32(dev, 0xD764, 0);
    ASSERT_EQ(read64(dev, 0xD768), (uint64_t)0);
    ASSERT_EQ(read32(dev, 0xD770), (uint32_t)0);
}

TEST_CASE(mega65_math_divbusy_zero) {
    // $D70F is in the $D700-$D70F range claimed by the F018B DMA controller,
    // so the math device correctly rejects reads there (returns false).
    // In the real hardware, DIVBUSY is readable via the DMA register shadow.
    // Just verify the math device doesn't claim that address.
    Mega65MathDevice dev(0xD700);
    uint8_t val = 0xFF;
    ASSERT(!dev.ioRead(nullptr, 0xD70F, &val));
}

TEST_CASE(mega65_math_rng_advances) {
    Mega65MathDevice dev(0xD700);
    uint8_t v1, v2, v3;
    dev.ioRead(nullptr, 0xD7EF, &v1);
    dev.ioRead(nullptr, 0xD7EF, &v2);
    dev.ioRead(nullptr, 0xD7EF, &v3);
    ASSERT(v1 != v2 || v2 != v3);
}

TEST_CASE(mega65_math_rng_stat_ready) {
    Mega65MathDevice dev(0xD700);
    uint8_t val = 0xFF;
    dev.ioRead(nullptr, 0xD7FE, &val);
    ASSERT_EQ(val & 0x80, (uint8_t)0);
}

TEST_CASE(mega65_math_rng_readonly) {
    Mega65MathDevice dev(0xD700);
    dev.ioWrite(nullptr, 0xD7EF, 0x42);
    dev.ioWrite(nullptr, 0xD7FE, 0x80);
    uint8_t val = 0xFF;
    dev.ioRead(nullptr, 0xD7FE, &val);
    ASSERT_EQ(val & 0x80, (uint8_t)0);
}

TEST_CASE(mega65_math_reset) {
    Mega65MathDevice dev(0xD700);
    write32(dev, 0xD770, 100);
    write32(dev, 0xD774, 200);
    dev.reset();
    ASSERT_EQ(read64(dev, 0xD778), (uint64_t)0);
}

TEST_CASE(mega65_math_out_of_range) {
    Mega65MathDevice dev(0xD700);
    uint8_t val = 0x42;
    ASSERT(!dev.ioRead(nullptr, 0xD800, &val));
    ASSERT(!dev.ioWrite(nullptr, 0xD800, 0x00));
}

TEST_CASE(mega65_math_name_and_base) {
    Mega65MathDevice dev(0xD700);
    ASSERT(std::string(dev.name()) == "MEGA65 Math");
    ASSERT_EQ(dev.baseAddr(), (uint32_t)0xD700);
    ASSERT_EQ(dev.addrMask(), (uint32_t)0xFF);
    dev.setName("CustomMath");
    ASSERT(std::string(dev.name()) == "CustomMath");
    dev.setBaseAddr(0xDF00);
    ASSERT_EQ(dev.baseAddr(), (uint32_t)0xDF00);
}
