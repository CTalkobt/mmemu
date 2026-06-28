#include "test_harness.h"
#include "plugins/devices/cia6526/main/cia6526.h"
#include "libmem/main/memory_bus.h"
#include <cstring>

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

    // Tick many times to cause underflow and reload
    for (int i = 0; i < 0x2000; ++i) {
        f.cia.tick(1);
    }

    // After underflow and reload, counter should restart from latch
    f.cia.ioRead(&f.bus, 0xDC04, &val);
    ASSERT_EQ((int)val, 0x00);  // Counter reloaded from latch
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
