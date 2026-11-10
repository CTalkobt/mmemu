// The SPC700's TEST register ($F0), as Mesen: RAM writes off, and the wait states.

#include "test_harness.h"
#include "plugins/machines/snes/main/snes_apu.h"
#include <vector>

namespace {
/** Load `prog` at $0200 and point the SPC700 there. */
void load(snes::Apu& apu, const std::vector<uint8_t>& prog) {
    for (size_t i = 0; i < prog.size(); ++i) apu.ram()[0x200 + i] = prog[i];
    apu.cpu()->setPc(0x200);
}
}  // namespace

TEST_CASE(snes_apu_test_register_writes) {
    snes::Apu apu;
    load(apu, {0x8F, 0x08, 0xF0,     // MOV $F0, #$08: timers on, RAM writes off
               0x8F, 0x55, 0x10,     // MOV $10, #$55: lost
               0x8F, 0x0A, 0xF0,     // writes on again
               0x8F, 0x66, 0x11,     // MOV $11, #$66
               0x2F, 0xFE});         // BRA *
    apu.runTo(apu.cycle() + 100);
    EXPECT_EQ(apu.ram()[0x10], 0x00);
    EXPECT_EQ(apu.ram()[0x11], 0x66);
    EXPECT_TRUE(apu.ramWritesOn());
}

TEST_CASE(snes_apu_test_register_wait_states) {
    // NOPs from RAM: an opcode fetch (external) and an idle cycle (internal). External speed 1 doubles the fetch.
    auto run = [](uint8_t test) {
        snes::Apu apu;
        std::vector<uint8_t> prog = {0x8F, test, 0xF0};
        for (int i = 0; i < 400; ++i) prog.push_back(0x00);    // NOP
        load(apu, prog);
        apu.runTo(apu.cycle() + 20);
        const uint32_t from = apu.cpu()->pc();
        apu.runTo(apu.cycle() + 300);
        return apu.cpu()->pc() - from;
    };
    const uint32_t normal = run(0x0A), slow = run(0x1A);
    EXPECT_TRUE(normal >= 148 && normal <= 152);                // 2 cycles a NOP
    EXPECT_TRUE(slow >= 98 && slow <= 102);                     // 3 cycles a NOP
}

// The CPU's port writes (as Mesen): one in the second half of an APU cycle is seen from the cycle after
TEST_CASE(snes_apu_port_latch_delay) {
    // MOV $10, $F4 (5 cycles: $F4 is read in the third), over and over
    auto seen = [](uint64_t delay) {
        snes::Apu apu;
        std::vector<uint8_t> prog;
        for (int i = 0; i < 20; ++i) { prog.push_back(0xFA); prog.push_back(0xF4); prog.push_back(0x10); }
        load(apu, prog);
        apu.runTo(apu.cycle() + 1);                          // one instruction
        const uint64_t s = apu.cycle();                      // the next starts here, reading $F4 at s + 2
        apu.cpuWrite(0, 0x42, s + delay);
        apu.runTo(s + 1);
        return apu.ram()[0x10];
    };
    EXPECT_EQ(seen(0), 0x42);                                // at once
    EXPECT_EQ(seen(2), 0x42);                                // from the read's own cycle
    EXPECT_EQ(seen(3), 0x00);                                // a cycle too late for it
    // and the instruction after sees it
    snes::Apu apu;
    load(apu, {0xFA, 0xF4, 0x10, 0xFA, 0xF4, 0x11, 0x2F, 0xFE});
    apu.cpuWrite(0, 0x42, apu.cycle() + 3);
    apu.runTo(apu.cycle() + 8);
    EXPECT_EQ(apu.ram()[0x10], 0x00);
    EXPECT_EQ(apu.ram()[0x11], 0x42);
}
