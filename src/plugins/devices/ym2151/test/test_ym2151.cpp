#include "test_harness.h"
#include "plugins/devices/ym2151/main/ym2151.h"
#include "libdevices/main/isignal_line.h"
#include <cmath>

namespace {
struct Line : ISignalLine {
    bool level = false;
    int changes = 0;
    void set(bool v) override { if (v != level) changes++; level = v; }
    bool get() const override { return level; }
    void pulse() override { set(true); set(false); }
};

// Write a register the way software does: wait until not busy
void reg(Ym2151& ym, uint8_t r, uint8_t v) {
    uint8_t st = 0;
    for (int i = 0; i < 1000; i++) { ym.ioRead(nullptr, 0x9F41, &st); if (!(st & 0x80)) break; ym.tick(8); }
    ym.ioWrite(nullptr, 0x9F40, r);
    ym.ioWrite(nullptr, 0x9F41, v);
}
}

TEST_CASE(ym2151_busy_after_write_and_peek) {
    Ym2151 ym;
    ym.reset();
    uint8_t st = 0;
    ASSERT(ym.ioPeek(nullptr, 0x9F41, &st));
    ASSERT_EQ((int)(st & 0x80), 0);
    ym.ioWrite(nullptr, 0x9F40, 0x20);
    ym.ioWrite(nullptr, 0x9F41, 0xC7);
    ym.ioRead(nullptr, 0x9F41, &st);
    ASSERT_EQ((int)(st & 0x80), 0x80);          // busy right after a data write
    ym.tick(8000);                              // 1 ms later
    ym.ioRead(nullptr, 0x9F41, &st);
    ASSERT_EQ((int)(st & 0x80), 0);
    ASSERT(ym.ioPeek(nullptr, 0x9F40, &st));
    ASSERT_EQ((int)st, 0x9F);                   // even address: open bus
}

TEST_CASE(ym2151_key_on_produces_sound) {
    Ym2151 ym;
    ym.reset();
    // Channel 0: algorithm 7 (all carriers), both outputs; carrier on M1 only
    reg(ym, 0x20, 0xC7);                        // RL=11, FB=0, CON=7
    reg(ym, 0x28, 0x4A);                        // KC: octave 4, A
    reg(ym, 0x40, 0x01);                        // M1 DT1/MUL
    reg(ym, 0x60, 0x00);                        // M1 TL = 0 (loudest)
    reg(ym, 0x80, 0x1F);                        // M1 AR
    reg(ym, 0xE0, 0x0F);                        // M1 SL/RR
    for (int op : {0x68, 0x70, 0x78}) reg(ym, (uint8_t)op, 0x7F);   // other operators silent
    reg(ym, 0x08, 0x08);                        // key on M1, channel 0
    ym.tick(8000000 / 20);                      // 50 ms
    double sum = 0;
    for (int i = 0; i < 2000; i++) {
        int32_t l, r;
        ym.nextSample(48828, l, r);
        sum += std::abs(l) + std::abs(r);
    }
    ASSERT(sum > 1000.0);
}

TEST_CASE(ym2151_timer_a_irq) {
    Ym2151 ym;
    Line irq;
    ym.setIrqLine(&irq);
    ym.reset();
    reg(ym, 0x10, 0xFF);                        // timer A = 1023: shortest period
    reg(ym, 0x11, 0x03);
    reg(ym, 0x14, 0x15);                        // load and enable IRQ for timer A
    ym.tick(8000000 / 100);                     // 10 ms
    ASSERT(irq.level);
    uint8_t st = 0;
    ym.ioRead(nullptr, 0x9F41, &st);
    ASSERT_EQ((int)(st & 0x01), 0x01);          // timer A flag
}
