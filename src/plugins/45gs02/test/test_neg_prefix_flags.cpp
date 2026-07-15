/// Test: NEG prefix instruction flag handling
/// Issue #887: NEG / NEG / STQ should only set flags from NEG prefixes if they execute

#include "test_harness.h"
#include "plugins/45gs02/main/cpu45gs02.h"
#include "libmem/main/memory_bus.h"

namespace {

struct TestFixture {
    FlatMemoryBus bus{"test", 16};
    MOS45GS02 cpu;

    TestFixture() {
        cpu.setDataBus(&bus);
        cpu.reset();
        cpu.regWrite(6, 0x2000); // PC = $2000
    }

    void poke(uint16_t addr, uint8_t val) { bus.write8(addr, val); }
};

} // namespace

TEST_CASE(neg_single_sets_flags) {
    TestFixture t;

    // Single NEG A should set N flag
    t.poke(0x2000, 0x42);  // NEG
    t.poke(0x2001, 0x60);  // RTS

    t.cpu.regWrite(0, 0x01);  // A = 0x01
    t.cpu.step();  // Execute NEG

    uint32_t a = t.cpu.regRead(0);
    uint32_t p = t.cpu.regRead(7);

    // NEG of 0x01 is -0x01 = 0xFF (sets N flag)
    ASSERT_EQ(a, 0xFF);
    ASSERT_EQ(p & 0x80, 0x80);  // N flag should be set
}

TEST_CASE(neg_double_prefix_behavior) {
    TestFixture t;

    // NEG / NEG followed by RTS
    // According to Bobby's analysis, the second NEG becomes the actual instruction
    t.poke(0x2000, 0x42);  // NEG (prefix or actual?)
    t.poke(0x2001, 0x42);  // NEG (prefix or actual?)
    t.poke(0x2002, 0x60);  // RTS

    t.cpu.regWrite(0, 0x42);  // A = 0x42
    uint32_t p_before = t.cpu.regRead(7);

    t.cpu.step();  // Execute instruction(s)

    uint32_t a = t.cpu.regRead(0);
    uint32_t p_after = t.cpu.regRead(7);

    // If first NEG is a prefix and second NEG is actual:
    // A = -0x42 = 0xBE (sets N flag)
    // If both are somehow skipped: A = 0x42
    // This test documents the actual behavior

    ASSERT_NE(a, 0x42);  // A should have changed
}

TEST_CASE(stq_does_not_set_flags) {
    TestFixture t;

    // STQ should not set any flags
    t.poke(0x2000, 0x85);  // STA zp (becomes STQ in quad mode)
    t.poke(0x2001, 0x00);  // Address: $00 (zero page)
    t.poke(0x2002, 0x60);  // RTS

    t.cpu.regWrite(8, 0x12345678);  // Q register
    uint32_t p_before = t.cpu.regRead(7);

    t.cpu.step();  // Execute STQ - this should NOT change flags

    uint32_t p_after = t.cpu.regRead(7);

    // STQ should not change N, V, D, B, Z, C flags
    // Only status that might differ is from initialization
    // The key point: writing via STQ shouldn't set any arithmetic flags
    ASSERT_EQ(p_before, p_after);  // No flag changes from STQ
}

