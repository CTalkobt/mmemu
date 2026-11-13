#include "test_harness.h"
#include "plugins/devices/cia6526/main/cia6526.h"
#include "libmem/main/memory_bus.h"
#include <cstring>
#include <string>
#include <vector>

struct CiaFixture {
    FlatMemoryBus bus{"system", 16};
    CIA6526 cia{"CIA1", 0xDC00};

    CiaFixture() {
        cia.setClockHz(1022727);  // NTSC
    }
};

// ============================================================================
// Basic Port I/O Tests
// ============================================================================

TEST_CASE(cia_port_read_write) {
    CiaFixture f;
    uint8_t val;

    // Configure Port A as output (DDRA = 0xFF)
    ASSERT(f.cia.ioWrite(&f.bus, 0xDC02, 0xFF));
    // Write to Port A output latch
    ASSERT(f.cia.ioWrite(&f.bus, 0xDC00, 0x42));
    ASSERT(f.cia.ioRead(&f.bus, 0xDC00, &val));
    ASSERT_EQ((int)val, 0x42);

    // Configure Port B as output (DDRB = 0xFF)
    ASSERT(f.cia.ioWrite(&f.bus, 0xDC03, 0xFF));
    // Write to Port B output latch
    ASSERT(f.cia.ioWrite(&f.bus, 0xDC01, 0x99));
    ASSERT(f.cia.ioRead(&f.bus, 0xDC01, &val));
    ASSERT_EQ((int)val, 0x99);
}

TEST_CASE(cia_data_direction_registers) {
    CiaFixture f;
    uint8_t val;

    // Write DDRA (set bits as outputs)
    ASSERT(f.cia.ioWrite(&f.bus, 0xDC02, 0xF0));
    ASSERT(f.cia.ioRead(&f.bus, 0xDC02, &val));
    ASSERT_EQ((int)val, 0xF0);

    // Write DDRB
    ASSERT(f.cia.ioWrite(&f.bus, 0xDC03, 0x0F));
    ASSERT(f.cia.ioRead(&f.bus, 0xDC03, &val));
    ASSERT_EQ((int)val, 0x0F);
}

TEST_CASE(cia_port_default_state) {
    CiaFixture f;
    uint8_t val;

    // Port pins default to 1 (no device attached)
    ASSERT(f.cia.ioRead(&f.bus, 0xDC00, &val));
    ASSERT_EQ((int)val, 0xFF);

    ASSERT(f.cia.ioRead(&f.bus, 0xDC01, &val));
    ASSERT_EQ((int)val, 0xFF);
}

// ============================================================================
// Timer A Tests
// ============================================================================

TEST_CASE(cia_timer_a_latch) {
    CiaFixture f;
    uint8_t val_lo, val_hi;

    // Write Timer A latch low byte (goes to latch, not counter)
    ASSERT(f.cia.ioWrite(&f.bus, 0xDC04, 0x34));

    // Write Timer A latch high byte (goes to latch, and since timer is stopped,
    // the latch value is immediately loaded into the counter)
    ASSERT(f.cia.ioWrite(&f.bus, 0xDC05, 0x12));

    // Read back counter values (which now equals latch since it was loaded)
    ASSERT(f.cia.ioRead(&f.bus, 0xDC04, &val_lo));
    ASSERT(f.cia.ioRead(&f.bus, 0xDC05, &val_hi));

    // Counter should have been loaded from latch
    ASSERT_EQ((int)val_lo, 0x34);
    ASSERT_EQ((int)val_hi, 0x12);
}

TEST_CASE(cia_timer_a_countdown) {
    CiaFixture f;
    uint8_t val_lo, val_hi;

    // Set Timer A to 0x0010 (16)
    f.cia.ioWrite(&f.bus, 0xDC04, 0x10);  // TALO = 16
    f.cia.ioWrite(&f.bus, 0xDC05, 0x00);  // TAHI = 0

    // Set CRA to start timer
    f.cia.ioWrite(&f.bus, 0xDC0E, 0x01);  // CR_START

    // Tick 5 times
    for (int i = 0; i < 5; ++i) {
        f.cia.tick(1);
    }

    // Read counter; should have decremented
    f.cia.ioRead(&f.bus, 0xDC04, &val_lo);
    f.cia.ioRead(&f.bus, 0xDC05, &val_hi);
    uint16_t counter = (val_hi << 8) | val_lo;
    ASSERT(counter < 16);  // Should be counting down
}

TEST_CASE(cia_timer_a_load_bit) {
    CiaFixture f;
    uint8_t val;

    // Set Timer A to 0x1000
    f.cia.ioWrite(&f.bus, 0xDC04, 0x00);
    f.cia.ioWrite(&f.bus, 0xDC05, 0x10);

    // Start timer
    f.cia.ioWrite(&f.bus, 0xDC0E, 0x01);

    // Counting starts on the third cycle after the start: $1000 is reached
    // on cycle 2, $0000 (the underflow, reloading $1000) on cycle $1002
    f.cia.tick(0x1002);

    // After underflow and reload, counter should restart from latch
    uint8_t hi;
    f.cia.ioRead(&f.bus, 0xDC04, &val);
    f.cia.ioRead(&f.bus, 0xDC05, &hi);
    ASSERT_EQ((int)val, 0x00);  // Counter reloaded from latch
    ASSERT_EQ((int)hi, 0x10);
    // ...and a period is latch + 1 cycles: the next reload $1001 cycles on
    f.cia.tick(0x1000);
    f.cia.ioRead(&f.bus, 0xDC04, &val);
    ASSERT_EQ((int)val, 0x01);
    f.cia.tick(1);
    f.cia.ioRead(&f.bus, 0xDC04, &val);
    f.cia.ioRead(&f.bus, 0xDC05, &hi);
    ASSERT_EQ((int)((hi << 8) | val), 0x1000);
}

TEST_CASE(cia_timer_a_one_shot_mode) {
    CiaFixture f;
    uint8_t val;

    // Set Timer A to 0x0008
    f.cia.ioWrite(&f.bus, 0xDC04, 0x08);
    f.cia.ioWrite(&f.bus, 0xDC05, 0x00);

    // Start in one-shot mode: CRA = CR_START | CR_ONESHOT
    f.cia.ioWrite(&f.bus, 0xDC0E, 0x09);  // 0x01 | 0x08

    // Tick until underflow
    for (int i = 0; i < 20; ++i) {
        f.cia.tick(1);
    }

    // Continue ticking; should not wrap again in one-shot mode
    f.cia.ioRead(&f.bus, 0xDC0E, &val);
    bool running = (val & 0x01) != 0;
    ASSERT(!running);  // Timer should have stopped
}

// ============================================================================
// Timer B Tests
// ============================================================================

TEST_CASE(cia_timer_b_countdown) {
    CiaFixture f;
    uint8_t val_lo, val_hi;

    // Set Timer B to 0x0020
    f.cia.ioWrite(&f.bus, 0xDC06, 0x20);
    f.cia.ioWrite(&f.bus, 0xDC07, 0x00);

    // Start Timer B: CRB = CR_START
    f.cia.ioWrite(&f.bus, 0xDC0F, 0x01);

    // Tick 10 times
    for (int i = 0; i < 10; ++i) {
        f.cia.tick(1);
    }

    // Counter should have decremented
    f.cia.ioRead(&f.bus, 0xDC06, &val_lo);
    f.cia.ioRead(&f.bus, 0xDC07, &val_hi);
    uint16_t counter = (val_hi << 8) | val_lo;
    ASSERT(counter < 32);
}

TEST_CASE(cia_timer_b_count_timer_a_underflow) {
    CiaFixture f;
    uint8_t val_b_lo, val_b_hi;

    // Set Timer A to 0x0002 (will underflow quickly)
    f.cia.ioWrite(&f.bus, 0xDC04, 0x02);
    f.cia.ioWrite(&f.bus, 0xDC05, 0x00);

    // Start Timer A
    f.cia.ioWrite(&f.bus, 0xDC0E, 0x01);

    // Set Timer B to 0x0010
    f.cia.ioWrite(&f.bus, 0xDC06, 0x10);
    f.cia.ioWrite(&f.bus, 0xDC07, 0x00);

    // Start Timer B counting Timer A underflows: CRB = CR_START | CRB_INMODE_TA
    f.cia.ioWrite(&f.bus, 0xDC0F, 0x41);  // 0x01 | 0x40

    // Tick enough times for Timer A to underflow several times
    for (int i = 0; i < 50; ++i) {
        f.cia.tick(1);
    }

    // Timer B should have decremented based on Timer A underflows
    f.cia.ioRead(&f.bus, 0xDC06, &val_b_lo);
    f.cia.ioRead(&f.bus, 0xDC07, &val_b_hi);
    uint16_t counterB = (val_b_hi << 8) | val_b_lo;
    ASSERT(counterB < 16);  // Should have decremented
}

// ============================================================================
// Interrupt Control Register (ICR) Tests
// ============================================================================

TEST_CASE(cia_icr_mask_set) {
    CiaFixture f;
    uint8_t val;

    // Write to ICR: set bit 7 to select "set mask" mode, bit 0 for Timer A
    f.cia.ioWrite(&f.bus, 0xDC0D, 0x81);  // 0x80 | 0x01

    // ICR read returns pending bits, not the mask. Initially no interrupts have fired.
    f.cia.ioRead(&f.bus, 0xDC0D, &val);
    // Should return 0 (no pending interrupts yet)
    ASSERT_EQ((int)val, 0x00);
}

TEST_CASE(cia_icr_mask_clear) {
    CiaFixture f;
    uint8_t val;

    // First, enable Timer A interrupt
    f.cia.ioWrite(&f.bus, 0xDC0D, 0x81);

    // Then disable it: write bit 7=0 (clear mode), bit 0=1 (Timer A)
    f.cia.ioWrite(&f.bus, 0xDC0D, 0x01);  // 0x80 not set = clear mode

    // No interrupts have fired, so ICR should still return 0
    f.cia.ioRead(&f.bus, 0xDC0D, &val);
    ASSERT_EQ((int)val, 0x00);
}

TEST_CASE(cia_icr_timer_a_interrupt) {
    CiaFixture f;

    // Enable Timer A interrupt
    f.cia.ioWrite(&f.bus, 0xDC0D, 0x81);  // Enable Timer A

    // Set Timer A to 0x0002
    f.cia.ioWrite(&f.bus, 0xDC04, 0x02);
    f.cia.ioWrite(&f.bus, 0xDC05, 0x00);

    // Start Timer A
    f.cia.ioWrite(&f.bus, 0xDC0E, 0x01);

    // Tick until underflow
    for (int i = 0; i < 20; ++i) {
        f.cia.tick(1);
    }

    // Read ICR to check if interrupt pending
    uint8_t icr;
    f.cia.ioRead(&f.bus, 0xDC0D, &icr);
    // Should have Timer A interrupt bit set (and ICR_INT bit)
    ASSERT((icr & 0x80) != 0);  // ICR_INT should be set
}

// ============================================================================
// TOD (Time of Day) Clock Tests
// ============================================================================

TEST_CASE(cia_tod_read_write) {
    CiaFixture f;
    uint8_t val;

    // Write TOD seconds (BCD: 0x45 = 45 seconds)
    f.cia.ioWrite(&f.bus, 0xDC09, 0x45);
    f.cia.ioRead(&f.bus, 0xDC09, &val);
    ASSERT_EQ((int)val, 0x45);

    // Write TOD minutes
    f.cia.ioWrite(&f.bus, 0xDC0A, 0x30);
    f.cia.ioRead(&f.bus, 0xDC0A, &val);
    ASSERT_EQ((int)val, 0x30);

    // Write TOD hours
    f.cia.ioWrite(&f.bus, 0xDC0B, 0x02);
    f.cia.ioRead(&f.bus, 0xDC0B, &val);
    ASSERT_EQ((int)val, 0x02);
}

TEST_CASE(cia_tod_increment) {
    CiaFixture f;

    // Set TOD to 0:0:0
    f.cia.ioWrite(&f.bus, 0xDC08, 0x00);  // Tenths
    f.cia.ioWrite(&f.bus, 0xDC09, 0x00);  // Seconds
    f.cia.ioWrite(&f.bus, 0xDC0A, 0x00);  // Minutes
    f.cia.ioWrite(&f.bus, 0xDC0B, 0x01);  // Hours

    // Set clock to 60 Hz (PAL): CRA bit 7 = 1
    f.cia.ioWrite(&f.bus, 0xDC0E, 0x80);

    // Read tenths; should remain at zero (no ticks yet)
    uint8_t val;
    f.cia.ioRead(&f.bus, 0xDC08, &val);
    ASSERT_EQ((int)val, 0x00);
}

// ============================================================================
// Reset Tests
// ============================================================================

TEST_CASE(cia_reset_clears_timers) {
    CiaFixture f;
    uint8_t val;

    // Write to Timer A
    f.cia.ioWrite(&f.bus, 0xDC04, 0x42);
    f.cia.ioWrite(&f.bus, 0xDC05, 0x24);

    // Reset CIA
    f.cia.reset();

    // Timer A should be reset to default
    f.cia.ioRead(&f.bus, 0xDC04, &val);
    ASSERT_EQ((int)val, 0xFF);  // Default value
}

TEST_CASE(cia_reset_clears_ports) {
    CiaFixture f;
    uint8_t val;

    // Write to Port A
    f.cia.ioWrite(&f.bus, 0xDC00, 0x42);
    f.cia.ioWrite(&f.bus, 0xDC02, 0xFF);  // DDRA

    // Reset CIA
    f.cia.reset();

    // Port A should be reset
    f.cia.ioRead(&f.bus, 0xDC00, &val);
    ASSERT_EQ((int)val, 0xFF);

    f.cia.ioRead(&f.bus, 0xDC02, &val);
    ASSERT_EQ((int)val, 0x00);  // DDRA reset to 0 (all inputs)
}

// ============================================================================
// Device Info and Aliases Tests
// ============================================================================

TEST_CASE(cia_device_info) {
    CiaFixture f;
    DeviceInfo info;
    f.cia.getDeviceInfo(info);

    ASSERT_NE(info.name.length(), 0);
    ASSERT_EQ(info.baseAddr, 0xDC00u);
    ASSERT_EQ(info.addrMask, 0x0Fu);
}

TEST_CASE(cia_base_addr_mask) {
    CiaFixture f;
    uint8_t val;

    // Valid addresses: $DC00-$DC0F
    ASSERT(f.cia.ioRead(&f.bus, 0xDC00, &val));
    ASSERT(f.cia.ioRead(&f.bus, 0xDC0F, &val));

    // Invalid: outside 16-byte range
    ASSERT(!f.cia.ioRead(&f.bus, 0xDB00, &val));
    ASSERT(!f.cia.ioRead(&f.bus, 0xDD00, &val));
}

// ============================================================================
// Multiple CIA Instances Tests
// ============================================================================

TEST_CASE(cia_multiple_instances) {
    FlatMemoryBus bus{"system", 16};
    CIA6526 cia1{"CIA1", 0xDC00};
    CIA6526 cia2{"CIA2", 0xDD00};

    cia1.setClockHz(1022727);
    cia2.setClockHz(1022727);

    uint8_t val1, val2;

    // Configure Port A as output for both CIAs
    cia1.ioWrite(&bus, 0xDC02, 0xFF);  // DDRA = 0xFF (output)
    cia2.ioWrite(&bus, 0xDD02, 0xFF);  // DDRA = 0xFF (output)

    // Write different values to each CIA
    cia1.ioWrite(&bus, 0xDC00, 0x11);
    cia2.ioWrite(&bus, 0xDD00, 0x22);

    // Verify independent state
    cia1.ioRead(&bus, 0xDC00, &val1);
    cia2.ioRead(&bus, 0xDD00, &val2);

    ASSERT_EQ((int)val1, 0x11);
    ASSERT_EQ((int)val2, 0x22);
}

TEST_CASE(cia_control_register_bits) {
    CiaFixture f;
    uint8_t val;

    // Set various CRA bits: START + LOAD + ONESHOT
    f.cia.ioWrite(&f.bus, 0xDC0E, 0x19);  // 0x01 | 0x08 | 0x10

    // Read back CRA
    f.cia.ioRead(&f.bus, 0xDC0E, &val);
    ASSERT((val & 0x01) != 0);  // CR_START persists
    ASSERT((val & 0x08) != 0);  // CR_ONESHOT persists
    // CR_LOAD (0x10) is write-only and always reads as 0 on real CIA
    ASSERT((val & 0x10) == 0);  // LOAD bit always reads as 0
}

// Starting a timer must not enable its interrupt, and the timer must count from
// the start (the old "grace period" and forced ICR mask were boot workarounds,
// not 6526 behaviour).
TEST_CASE(cia_timer_start_leaves_icr_mask_alone) {
    CiaFixture f;
    uint8_t icr;

    f.cia.ioWrite(&f.bus, 0xDC0D, 0x7F);   // disable all interrupts
    f.cia.ioWrite(&f.bus, 0xDC04, 0x10);   // Timer A latch = $0010
    f.cia.ioWrite(&f.bus, 0xDC05, 0x00);
    f.cia.ioWrite(&f.bus, 0xDC0E, 0x11);   // force load + start, continuous

    f.cia.tick(0x20);                      // well past one underflow
    ASSERT(f.cia.ioRead(&f.bus, 0xDC0D, &icr));
    ASSERT_EQ((int)(icr & 0x01), 0x01);    // underflow flagged without delay...
    ASSERT_EQ((int)(icr & 0x80), 0x00);    // ...but no IRQ: the mask is still clear
}

namespace {
struct PullDown : IPortDevice {
    uint8_t pins = 0xFF;
    uint8_t readPort() override { return pins; }
    void writePort(uint8_t) override {}
    void setDdr(uint8_t) override {}
};
}

TEST_CASE(cia_outputs_read_pins_when_enabled) {
    CiaFixture f;
    PullDown dev;
    dev.pins = 0xEF;                                // something holds bit 4 low
    f.cia.setPortADevice(&dev);
    uint8_t val;
    f.cia.ioWrite(&f.bus, 0xDC02, 0xFF);            // all outputs
    f.cia.ioWrite(&f.bus, 0xDC00, 0xFF);
    f.cia.ioRead(&f.bus, 0xDC00, &val);
    ASSERT_EQ((int)val, 0xFF);                      // default: the output latch
    f.cia.setOutputsReadPins(true);
    f.cia.ioRead(&f.bus, 0xDC00, &val);
    ASSERT_EQ((int)val, 0xEF);                      // the pin level
    f.cia.ioWrite(&f.bus, 0xDC00, 0x7F);
    f.cia.ioRead(&f.bus, 0xDC00, &val);
    ASSERT_EQ((int)val, 0x6F);
}

// ============================================================================
// Cycle timing of the old 6526 (VICE's ciatimer / ifr_delay model, #304).
// An access after tick(n) sees the chip as of the last cycle ticked.
// ============================================================================

namespace {
struct LineProbe : ISignalLine {
    bool level = false;
    bool get() const override { return level; }
    void set(bool l) override { level = l; }
    void pulse() override {}
};
uint16_t readTimerA(CiaFixture& f) {
    uint8_t lo, hi;
    f.cia.ioRead(&f.bus, 0xDC04, &lo);
    f.cia.ioRead(&f.bus, 0xDC05, &hi);
    return (uint16_t)((hi << 8) | lo);
}
}

// Started on cycle 0: the counter holds through cycle 2, decrements on 3
TEST_CASE(cia_timer_start_delay) {
    CiaFixture f;
    f.cia.ioWrite(&f.bus, 0xDC04, 0x10);
    f.cia.ioWrite(&f.bus, 0xDC05, 0x00);       // stopped: loads the counter
    f.cia.ioWrite(&f.bus, 0xDC0E, 0x01);
    f.cia.tick(2);
    ASSERT_EQ((int)readTimerA(f), 0x10);
    f.cia.tick(1);
    ASSERT_EQ((int)readTimerA(f), 0x0F);
    f.cia.tick(5);
    ASSERT_EQ((int)readTimerA(f), 0x0A);
}

// A force load lands two cycles after the write
TEST_CASE(cia_timer_force_load_delay) {
    CiaFixture f;
    f.cia.ioWrite(&f.bus, 0xDC04, 0x64);
    f.cia.ioWrite(&f.bus, 0xDC05, 0x00);       // counter = latch = 100
    f.cia.ioWrite(&f.bus, 0xDC04, 0x14);       // latch = 20, counter stays 100
    ASSERT_EQ((int)readTimerA(f), 100);
    f.cia.ioWrite(&f.bus, 0xDC0E, 0x10);       // force load, stopped
    f.cia.tick(1);
    ASSERT_EQ((int)readTimerA(f), 100);
    f.cia.tick(1);
    ASSERT_EQ((int)readTimerA(f), 20);
}

// One-shot: the underflow reloads the latch, sets the flag at once and stops
// the timer (CRA's START reads 0)
TEST_CASE(cia_timer_one_shot_stops) {
    CiaFixture f;
    uint8_t v;
    f.cia.ioWrite(&f.bus, 0xDC04, 0x03);
    f.cia.ioWrite(&f.bus, 0xDC05, 0x00);
    f.cia.ioWrite(&f.bus, 0xDC0E, 0x09);       // start, one-shot
    f.cia.tick(4);                             // 3, 3, 2, 1
    ASSERT_EQ((int)readTimerA(f), 1);
    f.cia.ioPeek(&f.bus, 0xDC0D, &v);
    ASSERT_EQ((int)v, 0x00);
    f.cia.tick(1);                             // 0: underflow
    ASSERT_EQ((int)readTimerA(f), 3);
    f.cia.ioPeek(&f.bus, 0xDC0D, &v);
    ASSERT_EQ((int)(v & 0x01), 0x01);
    f.cia.ioRead(&f.bus, 0xDC0E, &v);
    ASSERT_EQ((int)(v & 0x01), 0x00);
    f.cia.tick(20);
    ASSERT_EQ((int)readTimerA(f), 3);          // stays stopped
}

// Reading ICR returns the flags and clears them
TEST_CASE(cia_icr_read_clears) {
    CiaFixture f;
    uint8_t v;
    f.cia.ioWrite(&f.bus, 0xDC0D, 0x81);
    f.cia.ioWrite(&f.bus, 0xDC04, 0x02);
    f.cia.ioWrite(&f.bus, 0xDC05, 0x00);
    f.cia.ioWrite(&f.bus, 0xDC0E, 0x09);
    f.cia.tick(10);
    f.cia.ioRead(&f.bus, 0xDC0D, &v);
    ASSERT_EQ((int)v, 0x81);
    f.cia.ioPeek(&f.bus, 0xDC0D, &v);
    ASSERT_EQ((int)v, 0x80);                   // the flags clear at once, IR two cycles on
    f.cia.tick(2);
    f.cia.ioRead(&f.bus, 0xDC0D, &v);
    ASSERT_EQ((int)v, 0x00);
}

// The IRQ output and ICR bit 7 come one cycle after the underflow flag; an
// ICR read releases IRQ at once
TEST_CASE(cia_irq_delay) {
    CiaFixture f;
    LineProbe irq;
    f.cia.setIrqLine(&irq);
    uint8_t v;
    f.cia.ioWrite(&f.bus, 0xDC0D, 0x81);
    f.cia.ioWrite(&f.bus, 0xDC04, 0x02);
    f.cia.ioWrite(&f.bus, 0xDC05, 0x00);
    f.cia.ioWrite(&f.bus, 0xDC0E, 0x09);
    f.cia.tick(3);                             // 2, 2, 1, 0: underflow on cycle 4
    ASSERT(!irq.level);
    f.cia.tick(1);
    f.cia.ioPeek(&f.bus, 0xDC0D, &v);
    ASSERT_EQ((int)v, 0x01);                   // flag now, IR not yet
    ASSERT(!irq.level);
    f.cia.tick(1);
    f.cia.ioPeek(&f.bus, 0xDC0D, &v);
    ASSERT_EQ((int)v, 0x81);
    ASSERT(irq.level);
    f.cia.ioRead(&f.bus, 0xDC0D, &v);
    ASSERT_EQ((int)v, 0x81);
    ASSERT(!irq.level);
}

// Enabling the mask of a flag already set raises IRQ two cycles later
TEST_CASE(cia_irq_on_mask_enable) {
    CiaFixture f;
    LineProbe irq;
    f.cia.setIrqLine(&irq);
    f.cia.ioWrite(&f.bus, 0xDC04, 0x02);
    f.cia.ioWrite(&f.bus, 0xDC05, 0x00);
    f.cia.ioWrite(&f.bus, 0xDC0E, 0x09);
    f.cia.tick(10);                            // TA flag set, masked off
    ASSERT(!irq.level);
    f.cia.ioWrite(&f.bus, 0xDC0D, 0x81);
    f.cia.tick(1);
    ASSERT(!irq.level);
    f.cia.tick(1);
    ASSERT(irq.level);
}

// Timer B counting timer A underflows: one count per underflow
TEST_CASE(cia_timer_b_counts_ta_exactly) {
    CiaFixture f;
    uint8_t lo, hi;
    f.cia.ioWrite(&f.bus, 0xDC04, 0x04);
    f.cia.ioWrite(&f.bus, 0xDC05, 0x00);       // TA period 5
    f.cia.ioWrite(&f.bus, 0xDC06, 0x10);
    f.cia.ioWrite(&f.bus, 0xDC07, 0x00);
    f.cia.ioWrite(&f.bus, 0xDC0F, 0x41);
    f.cia.ioWrite(&f.bus, 0xDC0E, 0x01);
    f.cia.tick(2 + 4 + 5 * 3 + 3);             // TA underflows on 6, 11, 16, 21
    f.cia.ioRead(&f.bus, 0xDC06, &lo);
    f.cia.ioRead(&f.bus, 0xDC07, &hi);
    ASSERT_EQ((int)((hi << 8) | lo), 0x10 - 4);
}

// The fast path (whole stretches of steady counting) agrees with one cycle
// at a time
TEST_CASE(cia_tick_bulk_matches_single) {
    CiaFixture a, b;
    for (CiaFixture* f : {&a, &b}) {
        f->cia.ioWrite(&f->bus, 0xDC04, 0x37);
        f->cia.ioWrite(&f->bus, 0xDC05, 0x01);
        f->cia.ioWrite(&f->bus, 0xDC06, 0x05);
        f->cia.ioWrite(&f->bus, 0xDC07, 0x00);
        f->cia.ioWrite(&f->bus, 0xDC0D, 0x83);
        f->cia.ioWrite(&f->bus, 0xDC0F, 0x41);
        f->cia.ioWrite(&f->bus, 0xDC0E, 0x11);
    }
    a.cia.tick(5000);
    for (int i = 0; i < 5000; ++i) b.cia.tick(1);
    ASSERT_EQ((int)readTimerA(a), (int)readTimerA(b));
    uint8_t va, vb;
    a.cia.ioRead(&a.bus, 0xDC06, &va);
    b.cia.ioRead(&b.bus, 0xDC06, &vb);
    ASSERT_EQ((int)va, (int)vb);
    a.cia.ioRead(&a.bus, 0xDC0D, &va);
    b.cia.ioRead(&b.bus, 0xDC0D, &vb);
    ASSERT_EQ((int)va, (int)vb);
}

// The 6526A raises IR and IRQ in the underflow's own cycle (the 6526: one
// cycle later, cia_irq_delay)
TEST_CASE(cia_6526a_irq_delay) {
    CiaFixture f;
    std::string err;
    ASSERT(f.cia.setOption("model", "6526A", err));
    LineProbe irq;
    f.cia.setIrqLine(&irq);
    uint8_t v;
    f.cia.ioWrite(&f.bus, 0xDC0D, 0x81);
    f.cia.ioWrite(&f.bus, 0xDC04, 0x02);
    f.cia.ioWrite(&f.bus, 0xDC05, 0x00);
    f.cia.ioWrite(&f.bus, 0xDC0E, 0x09);
    f.cia.tick(3);
    ASSERT(!irq.level);
    f.cia.tick(1);                             // the underflow
    f.cia.ioPeek(&f.bus, 0xDC0D, &v);
    ASSERT_EQ((int)v, 0x81);
    ASSERT(irq.level);
    ASSERT(!f.cia.setOption("model", "6527", err));
}

// The serial port shifts a byte out on timer A's underflows: 16 of them,
// then SP; it doesn't finish at once
TEST_CASE(cia_sdr_shifts_out) {
    CiaFixture f;
    std::vector<uint8_t> out;
    f.cia.setSerialOut([&](uint8_t b) { out.push_back(b); });
    f.cia.ioWrite(&f.bus, 0xDC04, 0x03);
    f.cia.ioWrite(&f.bus, 0xDC05, 0x00);       // underflow every 4 cycles
    f.cia.ioWrite(&f.bus, 0xDC0E, 0x51);       // start, force load, SP output
    f.cia.ioWrite(&f.bus, 0xDC0C, 0xA5);
    uint8_t v;
    f.cia.tick(16 * 4 - 8);
    f.cia.ioPeek(&f.bus, 0xDC0D, &v);
    ASSERT_EQ(v & 0x08, 0);
    ASSERT(out.empty());
    bool cntLow = false;
    for (int i = 0; i < 40 && !(v & 0x08); ++i) {
        f.cia.tick(1);
        cntLow |= !f.cia.cntOut();
        f.cia.ioPeek(&f.bus, 0xDC0D, &v);
    }
    ASSERT(v & 0x08);
    ASSERT(cntLow);
    ASSERT_EQ(out.size(), 1u);
    ASSERT_EQ((int)out[0], 0xA5);
}

// The MEGA65's CIA finishes a byte at once
TEST_CASE(cia_sdr_mega65_instant) {
    CiaFixture f;
    std::string err;
    ASSERT(f.cia.setOption("model", "mega65", err));
    f.cia.ioWrite(&f.bus, 0xDC0E, 0x40);
    f.cia.ioWrite(&f.bus, 0xDC0C, 0x12);
    f.cia.tick(2);
    uint8_t v;
    f.cia.ioRead(&f.bus, 0xDC0D, &v);
    ASSERT(v & 0x08);
}

// TOD: stopped after reset; started by writing the tenths; 50 Hz mains on a
// PAL machine, divided by 5 (CRA bit 7 set) or by 6, which runs it slow
TEST_CASE(cia_tod_counts_mains_ticks) {
    for (int cra : {0x80, 0x00}) {
        CIA6526 cia("CIA1", 0xDC00);
        FlatMemoryBus bus("system", 16);
        cia.setClockHz(985248);
        uint8_t sec, ten;
        cia.tick(985248 * 2);
        cia.ioRead(&bus, 0xDC09, &sec);
        cia.ioRead(&bus, 0xDC08, &ten);
        ASSERT_EQ((int)sec, 0);                // stopped
        ASSERT_EQ((int)ten, 0);
        cia.ioWrite(&bus, 0xDC0E, (uint8_t)cra);
        cia.ioWrite(&bus, 0xDC0B, 0x01);
        cia.ioWrite(&bus, 0xDC0A, 0x00);
        cia.ioWrite(&bus, 0xDC09, 0x00);
        cia.ioWrite(&bus, 0xDC08, 0x00);       // starts it
        cia.tick(985248 * 3);                  // 3 seconds
        cia.ioRead(&bus, 0xDC09, &sec);
        cia.ioRead(&bus, 0xDC08, &ten);
        const int tenths = (sec >> 4) * 100 + (sec & 15) * 10 + ten;
        const int lo = cra ? 29 : 24, hi = cra ? 30 : 25;   // 50 Hz / 5, / 6
        ASSERT(tenths >= lo && tenths <= hi);
    }
}

// The hours: 11 -> 12 flips AM/PM, 12 -> 01, and writing 12 flips it too
TEST_CASE(cia_tod_hours_roll) {
    CiaFixture f;
    uint8_t hr;
    f.cia.ioWrite(&f.bus, 0xDC0E, 0x00);       // 60 Hz on this NTSC clock
    f.cia.ioWrite(&f.bus, 0xDC0B, 0x11);
    f.cia.ioWrite(&f.bus, 0xDC0A, 0x59);
    f.cia.ioWrite(&f.bus, 0xDC09, 0x59);
    f.cia.ioWrite(&f.bus, 0xDC08, 0x09);
    f.cia.tick(1022727 / 10 + 20000);
    f.cia.ioPeek(&f.bus, 0xDC0B, &hr);
    ASSERT_EQ((int)hr, 0x92);                  // 12 PM
    f.cia.ioWrite(&f.bus, 0xDC0B, 0x12);       // AM 12 written: becomes PM
    f.cia.ioPeek(&f.bus, 0xDC0B, &hr);
    ASSERT_EQ((int)hr, 0x92);
}

// "decode": the C64 decodes a CIA through its page, registers every 16 bytes
// (VICE CIA/mirrors); by default it answers only its 16 registers
TEST_CASE(cia_decode_window) {
    CiaFixture f;
    uint8_t val = 0;
    ASSERT(!f.cia.ioWrite(&f.bus, 0xDC12, 0xFF));          // $DC10+: not ours
    std::string err;
    ASSERT(f.cia.setOption("decode", "0xFF", err));
    ASSERT(f.cia.ioWrite(&f.bus, 0xDCF2, 0xFF));           // DDRA through a mirror
    ASSERT(f.cia.ioWrite(&f.bus, 0xDC30, 0x5A));           // PRA
    ASSERT(f.cia.ioRead(&f.bus, 0xDC00, &val));
    ASSERT_EQ((int)val, 0x5A);
    ASSERT(f.cia.ioRead(&f.bus, 0xDCA0, &val));
    ASSERT_EQ((int)val, 0x5A);
    ASSERT(!f.cia.ioRead(&f.bus, 0xDD00, &val));           // the next page isn't
    ASSERT(!f.cia.setOption("decode", "0x17", err));       // not a window mask
}
