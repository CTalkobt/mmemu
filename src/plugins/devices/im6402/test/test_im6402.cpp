#include "test_harness.h"
#include "plugins/devices/im6402/main/im6402.h"
#include "plugins/devices/serial_endpoint/main/serial_endpoint.h"

namespace {
// 8 data bits, no parity, 1 stop (CLS1 CLS2 PI)
constexpr uint8_t CTRL_8N1 = Im6402::CTRL_CLS1 | Im6402::CTRL_CLS2 | Im6402::CTRL_PI;
}

TEST_CASE(im6402_control_word) {
    Im6402 u;
    u.writeControl(CTRL_8N1);
    ASSERT_EQ(u.wordLength(), 8);
    ASSERT(!u.parityEnabled());
    ASSERT_EQ(u.frameClocks(), 160);
    // 7 bits, even parity, 2 stop bits
    u.writeControl(Im6402::CTRL_CLS2 | Im6402::CTRL_EPE | Im6402::CTRL_SBS);
    ASSERT_EQ(u.wordLength(), 7);
    ASSERT(u.parityEnabled());
    ASSERT(u.evenParity());
    ASSERT_EQ(u.frameClocks(), (1 + 7 + 1) * 16 + 32);
    // 5 bits with SBS: 1.5 stop bits
    u.writeControl(Im6402::CTRL_SBS | Im6402::CTRL_PI);
    ASSERT_EQ(u.wordLength(), 5);
    ASSERT_EQ(u.frameClocks(), 6 * 16 + 24);
    // The pins alone don't change the word until CRL
    u.setControl(CTRL_8N1);
    ASSERT_EQ(u.wordLength(), 5);
    u.loadControl();
    ASSERT_EQ(u.wordLength(), 8);
}

// TBRE / TRE and the frame time; the byte reaches the far end at its end
TEST_CASE(im6402_transmit) {
    Im6402 u;
    u.writeControl(CTRL_8N1);
    auto ep = std::make_shared<LoopbackSerialEndpoint>();
    u.setEndpoint(ep);
    ASSERT_EQ(u.status(), Im6402::ST_TBRE | Im6402::ST_TRE);
    u.writeTbr(0x41);
    ASSERT(u.tbre());                      // moved straight to the shift register
    ASSERT(!u.tre());
    u.writeTbr(0x42);                      // buffered behind it
    ASSERT(!u.tbre());
    u.tickTx(159);
    ASSERT(u.sent.empty());
    u.tickTx(1);
    ASSERT_EQ(u.sent.size(), 1u);
    ASSERT_EQ(u.sent[0], 0x41);
    ASSERT(u.tbre());                      // the second moved in
    ASSERT(!u.tre());
    u.tickTx(160);
    ASSERT_EQ(u.sent.size(), 2u);
    ASSERT(u.tre());
    uint8_t b = 0;
    ASSERT(ep->receive(b));
    ASSERT_EQ(b, 0x41);
}

// TRO: start bit, data LSB first, parity, stop
TEST_CASE(im6402_tro_bits) {
    Im6402 u;
    u.writeControl(Im6402::CTRL_CLS1 | Im6402::CTRL_CLS2 | Im6402::CTRL_EPE);   // 8E1
    ASSERT(u.tro());                       // marking when idle
    u.writeTbr(0x03);                      // two ones: even parity bit 0
    ASSERT(!u.tro());                      // start
    u.tickTx(16);
    ASSERT(u.tro());                       // bit 0
    u.tickTx(16);
    ASSERT(u.tro());                       // bit 1
    u.tickTx(16);
    ASSERT(!u.tro());                      // bit 2
    u.tickTx(16 * 6);
    ASSERT(!u.tro());                      // parity
    u.tickTx(16);
    ASSERT(u.tro());                       // stop
    u.masterReset();
    ASSERT(u.tre());
    ASSERT(u.tro());
}

// Receive: DR at the centre of the first stop bit; OE if the last wasn't taken
TEST_CASE(im6402_receive) {
    Im6402 u;
    u.writeControl(CTRL_8N1);
    auto ep = std::make_shared<LoopbackSerialEndpoint>();
    u.setEndpoint(ep);
    int drEdges = 0;
    u.onDr = [&](bool v) { if (v) ++drEdges; };
    ep->inject({0x55, 0xAA});
    u.tickRx(151);
    ASSERT(!u.dr());
    u.tickRx(1);                           // 9.5 bit times
    ASSERT(u.dr());
    ASSERT_EQ(drEdges, 1);
    ASSERT_EQ(u.readRbr(), 0x55);
    u.tickRx(160);                         // the second lands with DR still set
    ASSERT(u.oe());
    ASSERT_EQ(u.readData(), 0xAA);
    ASSERT(!u.dr());
    ASSERT_EQ(u.status() & Im6402::ST_OE, Im6402::ST_OE);
    u.masterReset();
    ASSERT_EQ(u.status(), Im6402::ST_TBRE | Im6402::ST_TRE);
}

// Errors and the character length mask
TEST_CASE(im6402_errors_and_length) {
    Im6402 u;
    u.writeControl(Im6402::CTRL_CLS2 | Im6402::CTRL_EPE);   // 7E1
    u.receive(0xFF, true, false);
    u.tick16x(u.frameClocks());
    ASSERT(u.dr());
    ASSERT(u.pe());
    ASSERT(!u.fe());
    ASSERT_EQ(u.readData(), 0x7F);         // 7 bits
    u.receive(0x31, false, true);
    u.tick16x(u.frameClocks());
    ASSERT(u.fe());
    ASSERT(!u.pe());
    ASSERT(!u.oe());
    // No clock (the 81C55 timer stopped): nothing moves
    u.writeTbr(0x20);
    u.tick16x(0);
    ASSERT(!u.tre());
}

// The endpoint from a spec; a tick-driven echo through the loopback
TEST_CASE(im6402_attach_serial) {
    Im6402 u;
    u.writeControl(CTRL_8N1);
    ASSERT(!u.attachSerial("bogus:thing"));
    ASSERT(u.attachSerial("loopback"));
    u.writeTbr('K');
    u.tick16x(160);                        // sent: comes straight back
    u.tick16x(152);
    ASSERT(u.dr());
    ASSERT_EQ(u.readData(), 'K');
    ASSERT(u.attachSerial("none"));
    Im6402 v;
    v.loadState(u.saveState());
    ASSERT_EQ(v.wordLength(), 8);
}
