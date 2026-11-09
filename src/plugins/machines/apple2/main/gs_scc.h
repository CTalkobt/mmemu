#pragma once

#include "libdevices/main/io_handler.h"
#include <cstdint>
#include <deque>
#include <functional>
#include <string>

namespace apple2 {

/**
 * The IIgs's serial ports: a Zilog 8530 SCC, after KEGS scc.c (GPL-3.0).
 * Channel A is port 1 (the printer port, slot 1: $C039 command, $C03B
 * data), channel B port 2 (the modem port, slot 2: $C038, $C03A).
 *
 * - Registers through the pointer (a write of $00-$0F to the command port
 *   selects WR/RR 0-15, the next access uses it); WR0's commands (reset
 *   ext/status, reset Tx interrupt, reset highest IUS), WR9's channel and
 *   hardware resets and master interrupt enable (shared), RR0 (Rx
 *   available, zero count, Tx empty, DCD, CTS), RR1, RR2 (channel B: the
 *   vector modified by the pending interrupt), RR3 (channel A: pending
 *   interrupts), RR8 data, RR12/13 the time constant, RR15.
 * - Timing: the baud rate generator (WR12/13 + 2, x2, from the 3.6864 MHz
 *   crystal or the 3.58 MHz PCLK, WR14), the clock mode (WR11) and multiplier
 *   (WR4), the character size (WR5, WR4: start, stop and parity bits): a
 *   character takes that many 1 MHz cycles to send or receive; local
 *   loopback (WR14 bit 4) runs a bit a character.
 * - Interrupts: Rx character available (WR1 modes 1, 2; a three-deep FIFO),
 *   Tx buffer empty (WR1 bit 1), baud rate zero count (WR15 bit 1).
 * - The host side of each port: none, loopback, or a TCP listener on
 *   127.0.0.1 (CLI `serial tcp:PORT`: port 1 first, then port 2).
 */
class GsScc : public IOHandler {
public:
    explicit GsScc(double slowHz);
    ~GsScc() override;

    const char* name() const override { return "SCC"; }
    uint32_t baseAddr() const override { return 0; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void reset() override;
    void tick(uint64_t cycles) override;
    void getDeviceInfo(DeviceInfo& out) const override;
    bool attachSerial(const std::string& spec) override;

    // $C038-$C03B: port 0 = channel A, 1 = channel B
    uint8_t readReg(int port);
    void writeReg(int port, uint8_t v);
    uint8_t readData(int port);
    void writeData(int port, uint8_t v);
    bool irq() const;

    /** Bytes sent on a port (tests; after any host link). */
    void onTransmit(int port, std::function<void(uint8_t)> fn) { m_ch[port & 1].txHook = std::move(fn); }
    /** A byte from the far end of a port. */
    void receive(int port, uint8_t b);
    /** Cycles one character takes to send on a port. */
    double charCycles(int port) const { return m_ch[port & 1].txCycles; }

private:
    enum class Link { None, Loopback, Tcp };
    struct Channel {
        uint32_t reg[16] = {};
        int ptr = 0, mode = 0;
        bool dcd = false, brZero = false, txEmpty = true;
        std::deque<uint8_t> inBuf;          // from the far end
        std::deque<uint8_t> rxQueue;        // the FIFO the CPU reads (3 deep)
        bool wantRx = false, wantTx = false, wantExt = false;
        double txCycles = 1000, rxCycles = 1000, brCycles = 0;
        double txLeft = -1, rxLeft = -1, brLeft = -1;   // < 0: no event pending
        std::function<void(uint8_t)> txHook;
        Link link = Link::None;
        int listenFd = -1, clientFd = -1;
        uint64_t sent = 0, received = 0;
    };

    void resetPort(int p);
    void hardResetPort(int p);
    void regenClocks(int p);
    void rxEvent(int p);
    void rxInt(int p);
    void txEvent(int p);
    void maybeBrEvent(int p);
    void transmit(int p, uint8_t v);
    void pollTcp(int p);
    void closeTcp(int p);
    uint32_t pending() const;               // RR3's bits: A rx/tx/ext 5-3, B 2-0
    bool mie() const { return m_ch[0].reg[9] & 0x08; }

    const double m_slowHz;
    Channel m_ch[2];
    uint64_t m_pollCountdown = 0;
};

}  // namespace apple2
