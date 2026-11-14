#include "test_harness.h"
#include "via6522.h"
#include <vector>

namespace {
class MockSignal : public ISignalLine {
public:
    bool get() const override { return m_level; }
    void set(bool level) override { m_level = level; m_changed = true; }
    void pulse() override { m_level = !m_level; m_level = !m_level; }
    bool m_level = false;
    bool m_changed = false;
};
} // namespace

TEST_CASE(via6522_timers) {
    VIA6522 via("via1", 0x9110);
    MockSignal irq;
    via.setIrqLine(&irq);

    // Test Timer 1 One-Shot
    via.ioWrite(nullptr, 0x9114, 0x05); // T1CL = 5
    via.ioWrite(nullptr, 0x9115, 0x00); // T1CH = 0 (starts timer)
    
    via.tick(3);
    uint8_t val;
    via.ioRead(nullptr, 0x9114, &val);
    ASSERT(val <= 5); // Should have ticked down

    via.tick(10); // Should underflow
    via.ioRead(nullptr, 0x911D, &val); // Read IFR
    ASSERT(val & 0x40); // T1 bit set

    // Enable T1 interrupt
    via.ioWrite(nullptr, 0x911E, 0xC0); // IER: Set bit 7 (Set) and bit 6 (T1)
    ASSERT(irq.m_level == true); // IRQ should be active now
}

TEST_CASE(via6522_ca1_irq) {
    VIA6522 via("via1", 0x9110);
    MockSignal irq, ca1;
    via.setIrqLine(&irq);
    via.setCA1Line(&ca1);

    // Enable CA1 interrupt (IER bit 1)
    via.ioWrite(nullptr, 0x911E, 0x82); // set bit 7 (enable) + bit 1 (CA1)

    // CA1 defaults high; negative edge (PCR bit 0 = 0 by default)
    ca1.set(false); // falling edge
    via.tick(1);
    uint8_t ifr = 0;
    via.ioRead(nullptr, 0x911D, &ifr);
    ASSERT(ifr & 0x02); // CA1 flag set
    ASSERT(irq.m_level); // IRQ active

    // Reading ORA clears CA1 flag
    uint8_t val = 0;
    via.ioRead(nullptr, 0x9111, &val); // read ORA
    via.ioRead(nullptr, 0x911D, &ifr);
    ASSERT(!(ifr & 0x02)); // CA1 flag cleared
}

TEST_CASE(via6522_ca2_handshake_output) {
    VIA6522 via("via1", 0x9110);
    MockSignal ca1, ca2;
    via.setCA1Line(&ca1);
    via.setCA2Line(&ca2);

    ca1.m_level = true;  // CA1 idle high
    ca2.m_level = true;  // CA2 idle high (reflects VIA reset state)
    via.tick(1);         // prime m_ca1Prev / m_ca2Prev

    // Set PCR: CA2 = handshake output (bits 3:1 = 100 → PCR = 0x08)
    via.ioWrite(nullptr, 0x911C, 0x08); // PCR
    ASSERT(ca2.m_level == true); // handshake mode: PCR write alone must not assert CA2

    // Reading ORA asserts CA2 low
    uint8_t val = 0;
    via.ioRead(nullptr, 0x9111, &val);
    ASSERT(ca2.m_level == false); // CA2 asserted

    // Active CA1 negative edge releases CA2 (PCR bit 0 = 0, so negative edge)
    ca1.set(false);
    via.tick(1);
    ASSERT(ca2.m_level == true); // CA2 released
}

TEST_CASE(via6522_ca2_manual_output) {
    VIA6522 via("via1", 0x9110);
    MockSignal ca2;
    via.setCA2Line(&ca2);

    // PCR CA2 = manual low (bits 3:1 = 110 → PCR = 0x0C)
    via.ioWrite(nullptr, 0x911C, 0x0C);
    ASSERT(ca2.m_level == false);

    // PCR CA2 = manual high (bits 3:1 = 111 → PCR = 0x0E)
    via.ioWrite(nullptr, 0x911C, 0x0E);
    ASSERT(ca2.m_level == true);
}

TEST_CASE(via6522_cb2_handshake_output) {
    VIA6522 via("via1", 0x9110);
    MockSignal cb1, cb2;
    via.setCB1Line(&cb1);
    via.setCB2Line(&cb2);

    cb1.set(true);
    via.tick(1);

    // PCR: CB2 = handshake output (bits 7:5 = 100 → PCR = 0x80)
    via.ioWrite(nullptr, 0x911C, 0x80);

    // Writing ORB asserts CB2 low
    via.ioWrite(nullptr, 0x9110, 0x55);
    ASSERT(cb2.m_level == false);

    // Active CB1 negative edge releases CB2
    cb1.set(false);
    via.tick(1);
    ASSERT(cb2.m_level == true);
}

TEST_CASE(via6522_ports) {
    VIA6522 via("via1", 0x9110);
    
    class MockPort : public IPortDevice {
    public:
        uint8_t readPort() override { return m_val; }
        void writePort(uint8_t val) override { m_written = val; }
        void setDdr(uint8_t ddr) override { m_ddr = ddr; }
        uint8_t m_val = 0xAA;
        uint8_t m_written = 0;
        uint8_t m_ddr = 0;
    };

    MockPort portA;
    via.setPortADevice(&portA);

    // Set DDRA to all inputs
    via.ioWrite(nullptr, 0x9113, 0x00);
    ASSERT(portA.m_ddr == 0x00);

    uint8_t val;
    via.ioRead(nullptr, 0x9111, &val); // Read ORA
    ASSERT(val == 0xAA);

    // Set DDRA to all outputs
    via.ioWrite(nullptr, 0x9113, 0xFF);
    via.ioWrite(nullptr, 0x9111, 0x55); // Write ORA
    ASSERT(portA.m_written == 0x55);
}

// paN: a peripheral holds a port A input pin (the VIC-20's tape sense on PA6)
TEST_CASE(via6522_port_a_input_pin_lines) {
    VIA6522 via("VIA1", 0x9110);
    uint8_t v = 0;
    via.ioWrite(nullptr, 0x9113, 0x80);             // DDRA: PA7 output, others input
    via.ioWrite(nullptr, 0x9111, 0x00);
    via.ioRead(nullptr, 0x9111, &v);
    ASSERT_EQ((int)v, 0x00);                        // no pin lines: the latch, as before
    ISignalLine* sense = via.getSignalLine("pa6");
    ASSERT(sense != nullptr);
    ASSERT(via.getSignalLine("pa8") == nullptr);
    via.ioRead(nullptr, 0x9111, &v);
    ASSERT_EQ((int)v, 0x7F);                        // inputs pulled up; PA7 drives 0
    sense->set(false);
    via.ioRead(nullptr, 0x911F, &v);
    ASSERT_EQ((int)v, 0x3F);
    ASSERT(!sense->get());
    via.ioWrite(nullptr, 0x9113, 0xC0);             // PA6 now an output: the latch wins
    via.ioRead(nullptr, 0x9111, &v);
    ASSERT_EQ((int)v, 0x3F);
    via.ioWrite(nullptr, 0x9111, 0x40);
    via.ioRead(nullptr, 0x9111, &v);
    ASSERT_EQ((int)v, 0x7F);
}

// The T1 latches read back at $x6/$x7 (#341): the 1541 DOS loads T1 with
// LDA $1C07 / STA $1C05
TEST_CASE(via_t1_latch_reads_back) {
    VIA6522 via("VIA", 0x1C00);
    uint8_t v = 0;
    via.ioWrite(nullptr, 0x1C06, 0x34);
    via.ioWrite(nullptr, 0x1C07, 0x3A);
    via.ioRead(nullptr, 0x1C06, &v);
    ASSERT_EQ((int)v, 0x34);
    via.ioRead(nullptr, 0x1C07, &v);
    ASSERT_EQ((int)v, 0x3A);
}

// The timers as VICE's viacore (#341, testprogs drive/viavarious): T1 counts
// N ... 0, FFFF, then reloads (one-shot too), flags the cycle after it reads
// 0, once per T1CH write in one-shot mode; free-running needs a T1CH write
TEST_CASE(via_t1_counts_through_ffff_and_reloads) {
    VIA6522 via("VIA", 0x1800);
    uint8_t lo = 0, hi = 0, ifr = 0;
    auto t1 = [&]() { via.ioPeek(nullptr, 0x1804, &lo); via.ioPeek(nullptr, 0x1805, &hi); return (int)(lo | (hi << 8)); };
    via.ioWrite(nullptr, 0x1804, 0x02);
    via.ioWrite(nullptr, 0x1805, 0x00);                 // T1 = 2 from the next cycle
    via.tick(1);
    ASSERT_EQ(t1(), 2);
    via.tick(1); ASSERT_EQ(t1(), 1);
    via.tick(1); ASSERT_EQ(t1(), 0);
    via.ioPeek(nullptr, 0x180D, &ifr);
    ASSERT_EQ(ifr & 0x40, 0);                           // not in the cycle it reads 0
    via.tick(1); ASSERT_EQ(t1(), 0xFFFF);
    via.ioPeek(nullptr, 0x180D, &ifr);
    ASSERT_EQ(ifr & 0x40, 0x40);
    via.tick(1); ASSERT_EQ(t1(), 2);                    // reloaded, in one-shot mode too
    via.ioWrite(nullptr, 0x180D, 0x40);
    via.tick(4);
    via.ioPeek(nullptr, 0x180D, &ifr);
    ASSERT_EQ(ifr & 0x40, 0);                           // one-shot: once per write
    via.ioWrite(nullptr, 0x180B, 0x40);                 // free-running now: still none
    via.tick(8);
    via.ioPeek(nullptr, 0x180D, &ifr);
    ASSERT_EQ(ifr & 0x40, 0);
    via.ioWrite(nullptr, 0x1805, 0x00);                 // a T1CH write arms it
    via.tick(4);
    via.ioPeek(nullptr, 0x180D, &ifr);
    ASSERT_EQ(ifr & 0x40, 0x40);
}

// T2 with the shift register on it (ACR SR modes 1, 4, 5) is an 8-bit timer:
// the low byte reloads from its latch, the high byte counts its underflows,
// and the 16-bit underflow still flags
TEST_CASE(via_t2_eight_bit_mode) {
    VIA6522 via("VIA", 0x1800);
    uint8_t lo = 0, hi = 0, ifr = 0;
    via.ioWrite(nullptr, 0x180B, 0x10);                 // SR out free-running on T2
    via.ioWrite(nullptr, 0x1808, 0x03);
    via.ioWrite(nullptr, 0x1809, 0x01);                 // T2 = $0103
    via.tick(1);
    via.ioPeek(nullptr, 0x1808, &lo); via.ioPeek(nullptr, 0x1809, &hi);
    ASSERT_EQ((int)(lo | (hi << 8)), 0x0103);
    via.tick(3); via.ioPeek(nullptr, 0x1808, &lo); via.ioPeek(nullptr, 0x1809, &hi);
    ASSERT_EQ((int)(lo | (hi << 8)), 0x0100);
    via.tick(1); via.ioPeek(nullptr, 0x1808, &lo); via.ioPeek(nullptr, 0x1809, &hi);
    ASSERT_EQ((int)(lo | (hi << 8)), 0x00FF);
    via.tick(1); via.ioPeek(nullptr, 0x1808, &lo);
    ASSERT_EQ((int)lo, 0x03);                           // reloaded from the latch
    via.tick(3);                                        // 0002, 0001, 0000
    via.ioPeek(nullptr, 0x1808, &lo); via.ioPeek(nullptr, 0x1809, &hi);
    ASSERT_EQ((int)(lo | (hi << 8)), 0x0000);
    via.ioPeek(nullptr, 0x180D, &ifr);
    ASSERT_EQ(ifr & 0x20, 0);
    via.tick(1);
    via.ioPeek(nullptr, 0x180D, &ifr);
    ASSERT_EQ(ifr & 0x20, 0x20);                        // the 16-bit underflow
}
