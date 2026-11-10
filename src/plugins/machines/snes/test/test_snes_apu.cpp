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
