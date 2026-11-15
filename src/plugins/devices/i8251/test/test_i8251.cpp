#include "test_harness.h"
#include "plugins/devices/i8251/main/i8251.h"
#include "plugins/devices/serial_endpoint/main/serial_endpoint.h"

namespace {
// Async x16, 8 bits, no parity, 1 stop bit
constexpr uint8_t MODE_8N1_X16 = 0x4E;
constexpr uint8_t CMD_ON = I8251::CMD_TXEN | I8251::CMD_RXE | I8251::CMD_DTR | I8251::CMD_RTS;
}  // namespace

// Reset expects a mode, then commands; IR goes back to expecting a mode
TEST_CASE(i8251_mode_and_command) {
    I8251 u;
    ASSERT(u.expectingMode());
    u.write(1, MODE_8N1_X16);
    ASSERT(!u.expectingMode());
    ASSERT_EQ(u.factor(), 16);
    ASSERT_EQ(u.wordLength(), 8);
    ASSERT(!u.parityEnabled());
    ASSERT_EQ(u.frameClocks(), 160);
    u.write(1, CMD_ON);
    ASSERT_EQ(u.command(), CMD_ON);
    ASSERT(u.dtr() && u.rts());
    // 7 bits, even parity, 2 stop bits, x64
    u.write(1, I8251::CMD_IR);
    ASSERT(u.expectingMode());
    u.write(1, 0xC0 | 0x20 | 0x10 | 0x08 | 0x03);
    ASSERT_EQ(u.factor(), 64);
    ASSERT_EQ(u.wordLength(), 7);
    ASSERT(u.parityEnabled() && u.evenParity());
    ASSERT_EQ(u.frameClocks(), (1 + 7 + 1) * 64 + 2 * 64);
    // 5 bits, 1.5 stop bits, x1
    u.write(1, I8251::CMD_IR);
    u.write(1, 0x80 | 0x01);
    ASSERT_EQ(u.wordLength(), 5);
    ASSERT_EQ(u.frameClocks(), 6 + 1);       // 1.5 stop bits at x1: a whole clock (rounded down)
}

// TxRDY / TxEMPTY and the frame time; the byte reaches the far end at its end
TEST_CASE(i8251_transmit) {
    I8251 u;
    auto ep = std::make_shared<LoopbackSerialEndpoint>();
    u.setEndpoint(ep);
    u.write(1, MODE_8N1_X16);
    u.write(1, CMD_ON);
    ASSERT_EQ(u.read(1) & 0x07, I8251::ST_TXRDY | I8251::ST_TXEMPTY);
    bool txPin = true;
    u.onTxRdy = [&](bool v) { txPin = v; };
    u.write(0, 0x41);
    // Moved to the shift register at once: the buffer is free again, but not empty
    ASSERT_EQ(u.read(1) & 0x07, I8251::ST_TXRDY);
    u.write(0, 0x42);
    ASSERT_EQ(u.read(1) & 0x07, 0);
    ASSERT(!txPin);
    u.tickTx(159);
    ASSERT(u.sent.empty());
    u.tickTx(1);
    ASSERT_EQ(u.sent.size(), 1u);
    ASSERT_EQ(u.sent[0], 0x41);
    ASSERT(txPin);
    u.tickTx(160);
    ASSERT_EQ(u.sent.size(), 2u);
    ASSERT_EQ(u.read(1) & 0x07, I8251::ST_TXRDY | I8251::ST_TXEMPTY);
}

// Without TxEN the character waits in the buffer
TEST_CASE(i8251_txen_gates) {
    I8251 u;
    u.write(1, MODE_8N1_X16);
    u.write(1, I8251::CMD_RXE);
    u.write(0, 0x55);
    u.tickTx(1000);
    ASSERT(u.sent.empty());
    ASSERT(!(u.read(1) & I8251::ST_TXRDY));
    u.write(1, I8251::CMD_RXE | I8251::CMD_TXEN);
    u.tickTx(160);
    ASSERT_EQ(u.sent.size(), 1u);
    ASSERT_EQ(u.sent[0], 0x55);
}

// Loopback: what is sent comes back; RxRDY at the middle of the stop bit, cleared by the read
TEST_CASE(i8251_loopback_receive) {
    I8251 u;
    u.setEndpoint(std::make_shared<LoopbackSerialEndpoint>());
    u.write(1, MODE_8N1_X16);
    u.write(1, CMD_ON);
    int rises = 0;
    bool pin = false;
    u.onRxRdy = [&](bool v) { pin = v; if (v) ++rises; };
    u.write(0, 0x5A);
    u.tick(160);                     // sent
    ASSERT(!(u.status() & I8251::ST_RXRDY));
    u.tick(9 * 16 + 7);
    ASSERT(!(u.status() & I8251::ST_RXRDY));
    u.tick(1);
    ASSERT(u.status() & I8251::ST_RXRDY);
    ASSERT(pin);
    ASSERT_EQ(rises, 1);
    ASSERT_EQ(u.read(0), 0x5A);
    ASSERT(!(u.status() & I8251::ST_RXRDY));
    ASSERT(!pin);
}

// Overrun when a character arrives before the last was read; ER clears the errors
TEST_CASE(i8251_overrun_and_error_reset) {
    I8251 u;
    u.write(1, MODE_8N1_X16);
    u.write(1, CMD_ON);
    u.receive(0x31);
    u.receive(0x32, false, true);
    u.tick(400);
    ASSERT(u.status() & I8251::ST_OE);
    ASSERT(u.status() & I8251::ST_FE);
    ASSERT_EQ(u.read(0), 0x32);
    u.write(1, CMD_ON | I8251::CMD_ER);
    ASSERT_EQ(u.status() & (I8251::ST_OE | I8251::ST_FE | I8251::ST_PE), 0);
    ASSERT_EQ(u.command(), CMD_ON);
}

// A disabled receiver ignores the line; 7-bit words are masked
TEST_CASE(i8251_rx_disabled_and_word_length) {
    I8251 u;
    u.write(1, 0x4A);                // x16, 7 bits, no parity, 1 stop
    u.write(1, I8251::CMD_TXEN);
    u.receive(0xC1);
    u.tick(1000);
    ASSERT(!(u.status() & I8251::ST_RXRDY));
    u.write(1, I8251::CMD_TXEN | I8251::CMD_RXE);
    u.receive(0xC1);
    u.tick(1000);
    ASSERT(u.status() & I8251::ST_RXRDY);
    ASSERT_EQ(u.read(0), 0x41);
}

// DSR in the status follows the far end; snapshots round-trip
TEST_CASE(i8251_dsr_and_state) {
    I8251 u;
    ASSERT(!(u.status() & I8251::ST_DSR));
    u.setEndpoint(std::make_shared<LoopbackSerialEndpoint>());
    ASSERT(u.status() & I8251::ST_DSR);
    u.write(1, MODE_8N1_X16);
    u.write(1, CMD_ON);
    u.write(0, 0x77);
    u.tick(50);
    auto s = u.saveState();
    I8251 v;
    v.loadState(s);
    ASSERT_EQ(v.mode(), MODE_8N1_X16);
    ASSERT_EQ(v.command(), CMD_ON);
    ASSERT(!v.expectingMode());
    v.tickTx(110);
    ASSERT_EQ(v.sent.size(), 1u);
    ASSERT_EQ(v.sent[0], 0x77);
}

// Sync mode: one sync character; the transmitter idles until the first
// character, then fills with SYNC; the receiver hunts, SYNDET until a status read
TEST_CASE(i8251_sync_single) {
    I8251 u;
    u.write(1, 0x8C);                // sync, 8 bits, no parity, one sync character (SCS)
    ASSERT(u.synchronous());
    ASSERT_EQ(u.syncCount(), 1);
    u.write(1, 0x16);                // the sync character
    u.write(1, I8251::CMD_TXEN | I8251::CMD_RXE);
    ASSERT_EQ(u.command(), I8251::CMD_TXEN | I8251::CMD_RXE);
    ASSERT_EQ(u.syncChar(0), 0x16);
    ASSERT_EQ(u.frameClocks(), 8);   // no start or stop bits, a bit a clock
    u.tickTx(100);
    ASSERT(u.sent.empty());          // nothing until the first character
    u.write(0, 0x41);
    u.tickTx(8);
    ASSERT_EQ(u.sent.size(), 1u);
    ASSERT_EQ(u.sent[0], 0x41);
    ASSERT(u.status() & I8251::ST_TXEMPTY);    // sending fill
    u.tickTx(16);
    ASSERT_EQ(u.sent.size(), 3u);
    ASSERT_EQ(u.sent[1], 0x16);
    ASSERT_EQ(u.sent[2], 0x16);
    u.write(0, 0x42);                // waits for the fill character to end
    u.tickTx(8);
    ASSERT_EQ(u.sent[3], 0x16);
    u.tickTx(8);
    ASSERT_EQ(u.sent[4], 0x42);
    // Receive: hunting skips to the sync character; the next is data
    ASSERT(u.hunting());
    u.receive('X');
    u.receive(0x16);
    u.receive('H');
    u.tickRx(16);
    ASSERT(!(u.status() & I8251::ST_RXRDY));
    ASSERT(!u.hunting());
    ASSERT(u.status() & I8251::ST_SYNDET);
    u.tickRx(8);
    ASSERT(u.status() & I8251::ST_RXRDY);
    ASSERT(u.read(1) & I8251::ST_SYNDET);
    ASSERT(!(u.read(1) & I8251::ST_SYNDET)); // cleared by the read
    ASSERT_EQ(u.read(0), 'H');
    // EH: hunt again
    u.write(1, I8251::CMD_TXEN | I8251::CMD_RXE | I8251::CMD_EH);
    ASSERT(u.hunting());
    ASSERT(!(u.command() & I8251::CMD_EH));
}

// Two sync characters, in order; external sync detect
TEST_CASE(i8251_sync_double_and_external) {
    I8251 u;
    u.write(1, 0x0C);                // sync, 8 bits, two sync characters
    u.write(1, 0x16);
    ASSERT(u.expectingMode() == false);
    u.write(1, 0x17);
    u.write(1, I8251::CMD_RXE);
    ASSERT_EQ(u.command(), I8251::CMD_RXE);
    for (uint8_t b : {0x16, 0x5A, 0x17, 0x16, 0x17, 0x51}) u.receive(b);
    u.tickRx(5 * 8);
    ASSERT(!(u.status() & I8251::ST_RXRDY));
    ASSERT(u.status() & I8251::ST_SYNDET);
    u.tickRx(8);
    ASSERT_EQ(u.read(0), 0x51);
    // ESD: the SYNDET pin ends the hunt; the status bit is the pin
    I8251 e;
    e.write(1, 0xCC);                // sync, 8 bits, ESD, one sync character
    e.write(1, 0x16);
    e.write(1, I8251::CMD_RXE);
    ASSERT(e.externalSync());
    e.receive(0x16);
    e.tickRx(8);
    ASSERT(e.hunting());
    e.setSyndetPin(true);
    ASSERT(!e.hunting());
    ASSERT(e.read(1) & I8251::ST_SYNDET);
    ASSERT(e.read(1) & I8251::ST_SYNDET);       // the pin, not a latch
    e.receive(0x33);
    e.tickRx(8);
    ASSERT_EQ(e.read(0), 0x33);
    // A snapshot keeps the sync state
    I8251 v;
    v.loadState(u.saveState());
    ASSERT(v.synchronous());
    ASSERT_EQ(v.syncChar(1), 0x17);
}

// /CTS: the transmitter waits for it; the TxRDY pin is masked, the status bit is not
TEST_CASE(i8251_cts) {
    I8251 u;
    bool pin = false;
    u.onTxRdy = [&](bool v) { pin = v; };
    u.setCts(false);
    u.write(1, MODE_8N1_X16);
    u.write(1, CMD_ON);
    ASSERT(!pin);
    ASSERT(u.status() & I8251::ST_TXRDY);
    u.write(0, 0x55);
    u.tickTx(1000);
    ASSERT(u.sent.empty());
    u.setCts(true);
    u.tickTx(160);
    ASSERT_EQ(u.sent.size(), 1u);
    ASSERT(pin);
}
