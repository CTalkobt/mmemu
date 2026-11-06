#include "test_harness.h"
#include "plugins/devices/acia6551/main/acia6551.h"
#include "libdevices/main/isignal_line.h"
#include <vector>

namespace {
class Line : public ISignalLine {
public:
    bool get() const override { return level; }
    void set(bool v) override { level = v; }
    void pulse() override {}
    bool level = false;                                     // true = IRQ asserted
};
uint8_t rd(Acia6551& a, int r) { uint8_t v = 0; a.ioRead(nullptr, 0xFD00 + r, &v); return v; }
void wr(Acia6551& a, int r, uint8_t v) { a.ioWrite(nullptr, 0xFD00 + r, v); }
void run(Acia6551& a, uint64_t n) { for (uint64_t i = 0; i < n; ++i) a.tick(1); }
}

// Hardware reset: transmit empty, receive IRQ off, no carrier without a far end
TEST_CASE(acia_reset_state) {
    Acia6551 a;
    ASSERT(rd(a, 1) == 0x70);                               // TDRE; DCD, DSR not asserted
    ASSERT(rd(a, 2) == 0x02);
    ASSERT(rd(a, 3) == 0x00);
    ASSERT(a.attachSerial("loopback"));
    ASSERT(rd(a, 1) == 0x10);                               // the far end asserts DCD and DSR
}

// A character takes start + data + parity + stop bits at the programmed rate
TEST_CASE(acia_character_time) {
    Acia6551 a;
    a.setClockHz(1000000);
    wr(a, 3, 0x08);                                         // 1200 bps, 8N1
    ASSERT(a.bitRate() == 1200);
    ASSERT(a.charCycles() == (uint64_t)(1000000.0 / 1200 * 10));
    wr(a, 3, 0x8E | 0x20);                                  // 9600, 7 bits, 2 stop
    wr(a, 2, 0x20);                                         // parity on
    ASSERT(a.charCycles() == (uint64_t)(1000000.0 / 9600 * 11));
}

// TX -> RX loopback: the byte comes back a character time later, with an IRQ
TEST_CASE(acia_loopback_round_trip) {
    Acia6551 a;
    Line irq;
    a.setIrqLine(&irq);
    a.setClockHz(1000000);
    ASSERT(a.attachSerial("loopback"));
    std::vector<uint8_t> out;
    a.onTransmit([&](uint8_t b) { out.push_back(b); });
    wr(a, 3, 0x1F);                                         // 19200 8N1
    wr(a, 2, 0x09);                                         // DTR, receive IRQ on, transmitter on (no TX IRQ)
    wr(a, 0, 'A');
    ASSERT(!(rd(a, 1) & 0x10));                             // transmit register full
    // Enabling the transmitter started its character clock (aciacore.c):
    // the byte moves to the shift register on its next tick
    run(a, a.charCycles() + 1);
    ASSERT(rd(a, 1) & 0x10);                                // moved to the shift register
    ASSERT(out.size() == 1 && out[0] == 'A');
    ASSERT(!(rd(a, 1) & 0x08));
    run(a, a.charCycles() + 1);
    ASSERT(irq.level);
    uint8_t st = rd(a, 1);
    ASSERT(st & 0x80);                                      // IRQ flagged
    ASSERT(st & 0x08);                                      // receive full
    ASSERT(!irq.level);                                     // reading status clears it
    ASSERT(rd(a, 0) == 'A');
    ASSERT(!(rd(a, 1) & 0x08));
}

// A second byte before the first is read overruns; programmed reset clears it
TEST_CASE(acia_overrun_and_programmed_reset) {
    Acia6551 a;
    a.setClockHz(1000000);
    wr(a, 3, 0x1F);
    wr(a, 2, 0x0B);                                         // DTR, receive IRQ off, transmitter on
    a.receive('x');
    a.receive('y');
    run(a, 3 * a.charCycles());
    uint8_t st = rd(a, 1);
    ASSERT(st & 0x08);
    ASSERT(st & 0x04);                                      // overrun
    ASSERT(!(st & 0x80));                                   // no IRQ: receive IRQ off
    ASSERT(rd(a, 0) == 'x');                                // the first byte kept
    a.receive('z');
    run(a, 2 * a.charCycles());
    wr(a, 1, 0);                                            // programmed reset
    ASSERT(!(rd(a, 1) & 0x04));
    ASSERT((rd(a, 2) & 0x1F) == 0);                         // command cleared but parity
}

// Transmit IRQs: one per character time while enabled
TEST_CASE(acia_transmit_irq) {
    Acia6551 a;
    Line irq;
    a.setIrqLine(&irq);
    a.setClockHz(1000000);
    wr(a, 3, 0x1F);
    wr(a, 2, 0x07);                                         // DTR, receive IRQ off, TX with IRQ
    ASSERT(!irq.level);
    run(a, a.charCycles() + 1);
    ASSERT(irq.level);
    ASSERT(rd(a, 1) & 0x90);
    ASSERT(!irq.level);
}
