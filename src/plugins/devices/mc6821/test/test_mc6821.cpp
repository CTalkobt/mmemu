#include "test_harness.h"
#include "plugins/devices/mc6821/main/mc6821.h"

// Reset clears everything; CRx bit 2 picks the port or its DDR
TEST_CASE(mc6821_ddr_and_port) {
    Mc6821 pia;
    ASSERT_EQ(pia.read(1), 0);
    pia.write(0, 0x0F);                   // DDRA (CRA bit 2 clear)
    ASSERT_EQ(pia.read(0), 0x0F);
    pia.write(1, 0x04);                   // the port
    pia.write(0, 0x05);
    // Port A reads the pins: outputs as written, inputs pulled up
    ASSERT_EQ(pia.read(0), 0xF5);
    pia.a.inSink = 0x7E;                  // a device pulls bits 0 and 7 low (bit 0 an output)
    ASSERT_EQ(pia.read(0), 0x74);
    // Port B: outputs read the output register, undriven inputs read 0
    pia.write(2, 0xF0);
    pia.write(3, 0x04);
    pia.write(2, 0xA5);
    ASSERT_EQ(pia.read(2), 0xA0);
    pia.b.inSink = 0x00;                  // even pulled low, an output reads its register
    pia.b.inSource = 0x0F;
    ASSERT_EQ(pia.read(2), 0xA0);
    pia.b.inSink = 0xFF;
    ASSERT_EQ(pia.read(2), 0xAF);
    ASSERT_EQ(pia.portB(), 0xAF);
    pia.reset();
    ASSERT_EQ(pia.read(2), 0x00);         // DDRB again, and cleared
}

// Cx1: the flag on the active edge (falling unless CRx bit 1) whether or not
// enabled; /IRQ with the enable; a read of the port clears it
TEST_CASE(mc6821_cx1_interrupts) {
    Mc6821 pia;
    int irqCalls = 0;
    pia.onIrq = [&]() { ++irqCalls; };
    pia.setCa1(true);
    pia.write(1, 0x04);                   // falling edge, IRQ off
    pia.setCa1(false);
    ASSERT_EQ(pia.read(1) & 0x80, 0x80);
    ASSERT(!pia.irqA());
    pia.write(1, 0x05);                   // enable: the pending flag asserts /IRQA
    ASSERT(pia.irqA());
    (void)pia.read(1);                    // reading CRA doesn't clear it
    ASSERT(pia.irqA());
    (void)pia.read(0);                    // reading the port does
    ASSERT(!pia.irqA());
    ASSERT_EQ(pia.read(1) & 0x80, 0);
    pia.setCa1(true);                     // a rising edge: not active
    ASSERT(!pia.irqA());
    pia.write(1, 0x07);                   // rising edge
    pia.setCa1(false);
    pia.setCa1(true);
    ASSERT(pia.irqA());
    ASSERT(irqCalls > 0);
    // Side B likewise, independent
    pia.write(3, 0x05);
    pia.setCb1(true);
    pia.setCb1(false);
    ASSERT(pia.irqB());
    pia.write(1, 0x04);
    (void)pia.read(2);
    ASSERT(!pia.irqB());
    // Peeking doesn't clear
    pia.setCb1(true);
    pia.setCb1(false);
    (void)pia.peek(2);
    ASSERT(pia.irqB());
}

// Cx2 as an input: IRQx2 on its edge (bit 4), enabled by bit 3
TEST_CASE(mc6821_cx2_input) {
    Mc6821 pia;
    pia.write(1, 0x0C);                   // CA2 input, falling edge, IRQ2 on, port
    pia.a.c2InSink = true;
    pia.updateCa2();
    pia.a.c2InSink = false;
    pia.updateCa2();
    ASSERT_EQ(pia.read(1) & 0x40, 0x40);
    ASSERT(pia.irqA());
    (void)pia.read(0);
    ASSERT(!pia.irqA());
    // Making CA2 an output clears IRQA2
    pia.a.c2InSink = true;
    pia.updateCa2();
    pia.write(1, 0x1C);                   // rising edge
    pia.a.c2InSink = false;
    pia.updateCa2();
    pia.a.c2InSink = true;
    pia.updateCa2();
    ASSERT(pia.irqA());
    pia.write(1, 0x3C);
    ASSERT(!pia.irqA());
    ASSERT_EQ(pia.read(1) & 0x40, 0);
}

// Cx2 as an output: set/reset by CRx bit 3; CB2 handshake low on a write of
// port B until CB1; pulse mode back high at once
TEST_CASE(mc6821_cx2_output) {
    Mc6821 pia;
    int changes = 0;
    pia.b.onControl = [&]() { ++changes; };
    pia.write(1, 0x3C);
    ASSERT(pia.ca2());
    pia.write(1, 0x34);
    ASSERT(!pia.ca2());
    pia.write(3, 0x3C);
    ASSERT(pia.cb2());
    pia.write(3, 0x34);
    ASSERT(!pia.cb2());
    // Handshake: CB2 falls on a port B write, rises on the CB1 active edge
    pia.write(3, 0x24);
    ASSERT(pia.cb2());
    pia.write(2, 0x55);
    ASSERT(!pia.cb2());
    pia.setCb1(true);
    pia.setCb1(false);
    ASSERT(pia.cb2());
    // Pulse: low and back
    changes = 0;
    pia.write(3, 0x2C);
    pia.write(2, 0x55);
    ASSERT(pia.cb2());
    ASSERT(changes >= 3);
    // CA2 read strobe: low on a read of port A
    pia.write(1, 0x24);
    (void)pia.read(0);
    ASSERT(!pia.ca2());
    pia.setCa1(true);
    pia.setCa1(false);
    ASSERT(pia.ca2());
}

// A port write doesn't clear the side's flags (the 6520 device does; the 6821 doesn't)
TEST_CASE(mc6821_write_keeps_flags) {
    Mc6821 pia;
    pia.write(3, 0x05);
    pia.setCb1(true);
    pia.setCb1(false);
    pia.write(2, 0x12);
    ASSERT(pia.irqB());
    auto st = pia.saveState();
    Mc6821 other;
    other.loadState(st);
    ASSERT(other.irqB());
    ASSERT_EQ(other.peek(3) & 0x80, 0x80);
}

// tRS3: /IRQ follows the active edge a microsecond late (14 ticks of
// 14.31818 MHz, as XRoar); the flag reads set at once
TEST_CASE(mc6821_irq_delay) {
    Mc6821 pia;
    pia.irqDelay = 14;
    int calls = 0;
    pia.onIrq = [&]() { ++calls; };
    pia.advance(1000);
    pia.write(3, 0x05);                   // CB1 falling edge, IRQ on
    pia.setCb1(true);
    calls = 0;
    pia.setCb1(false);                    // the edge at 1000
    calls = 0;
    ASSERT_EQ(pia.read(3) & 0x80, 0x80);  // the flag at once
    ASSERT(!pia.irqB());
    pia.advance(1013);
    ASSERT(!pia.irqB());
    ASSERT_EQ(calls, 0);
    pia.advance(1014);                    // 1 us on: /IRQB
    ASSERT(pia.irqB());
    ASSERT_EQ(calls, 1);
    // A second edge while the flag is set doesn't push it back
    pia.setCb1(true);
    pia.setCb1(false);
    ASSERT(pia.irqB());
    (void)pia.read(2);                    // the port read clears flag and output at once
    ASSERT(!pia.irqB());
    // A flag set while disabled: enabling it later asserts at once
    pia.write(3, 0x04);
    pia.setCb1(true);
    pia.setCb1(false);                    // at 1014
    pia.advance(1100);
    ASSERT(!pia.irqB());
    pia.write(3, 0x05);
    ASSERT(pia.irqB());
    (void)pia.read(2);
    // Cx2 input edges likewise (CA2: falling edge, IRQ on)
    pia.write(1, 0x0C);
    pia.a.c2InSink = true;
    pia.updateCa2();
    pia.a.c2InSink = false;
    pia.updateCa2();                      // CA2's falling edge at 1100
    ASSERT_EQ(pia.read(1) & 0x40, 0x40);
    ASSERT(!pia.irqA());
    pia.advance(1114);
    ASSERT(pia.irqA());
    // Snapshots keep the pending output
    pia.write(3, 0x05);
    pia.setCb1(true);
    pia.setCb1(false);                    // at 1114
    Mc6821 other;
    other.irqDelay = 14;
    other.loadState(pia.saveState());
    ASSERT(!other.irqB());
    other.advance(1128);
    ASSERT(other.irqB());
    // With no delay (the default) the output follows at once
    Mc6821 plain;
    plain.write(1, 0x05);
    plain.setCa1(true);
    plain.setCa1(false);
    ASSERT(plain.irqA());
}
