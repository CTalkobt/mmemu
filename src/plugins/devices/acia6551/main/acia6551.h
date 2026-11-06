#pragma once

#include "libdevices/main/io_handler.h"
#include <cstdint>
#include <deque>
#include <functional>
#include <string>

class ISignalLine;

/**
 * MOS 6551 ACIA (#9; the Plus/4's RS-232 at $FD00), after VICE's aciacore.c.
 *
 * Registers (address & 3): 0 data (write: transmit, read: receive), 1 status
 * (read; a write is a programmed reset), 2 command, 3 control.
 *
 *  - Status: 0 parity, 1 framing, 2 overrun, 3 receive full, 4 transmit
 *    empty, 5 DCD, 6 DSR (both 1 = not asserted: no far end), 7 IRQ.
 *    Reading it clears the IRQ.
 *  - Command: 0 DTR (1 = receiver and IRQs on), 1 receive IRQ off,
 *    2-3 transmitter (00 off, 01 on with IRQ, 10 on, 11 break), 4 echo,
 *    5-7 parity.
 *  - Control: 0-3 bit rate (50-19200; 0 = external clock), 5-6 word length
 *    (8, 7, 6, 5), 7 two stop bits.
 *
 * Characters take as long as on the wire: (start + data + parity + stop)
 * bits at the programmed rate, in cycles of `setClockHz`. The transmitter
 * ticks once per character time from when it is enabled: a written byte moves
 * to the shift register (transmit empty again, and with transmit IRQs on, an
 * IRQ every tick) when the previous one has gone. A byte arriving on an idle
 * line is received a character time later, then one per character time; an
 * unread one is overrun by the next.
 *
 * The far end (`attachSerial`): "loopback" (what is sent comes back),
 * "tcp:PORT" (a TCP server on localhost: one client is the modem), or
 * "none" (sent bytes are dropped; DCD and DSR not asserted). `onTransmit`/`receive` give
 * code direct access.
 */
class Acia6551 : public IOHandler {
public:
    explicit Acia6551(uint32_t base = 0xFD00);
    ~Acia6551() override;

    const char* name() const override { return m_name.c_str(); }
    void setName(const std::string& n) override { m_name = n; }
    uint32_t baseAddr() const override { return m_base; }
    uint32_t addrMask() const override { return 0x0F; }
    void setBaseAddr(uint32_t b) override { m_base = b; }
    void setClockHz(uint32_t hz) override { m_clockHz = hz; updateCharTime(); }
    void setIrqLine(ISignalLine* l) override { m_irqLine = l; }

    bool ioRead(IBus* bus, uint32_t addr, uint8_t* val) override;
    bool ioPeek(IBus* bus, uint32_t addr, uint8_t* val) override;
    bool ioWrite(IBus* bus, uint32_t addr, uint8_t val) override;
    void reset() override;
    void tick(uint64_t cycles) override;
    void getDeviceInfo(DeviceInfo& out) const override;
    bool attachSerial(const std::string& spec) override;

    /** Bytes going out (after any host link). */
    void onTransmit(std::function<void(uint8_t)> fn) { m_txHook = std::move(fn); }
    /** A byte from the far end, delivered at the line rate. */
    void receive(uint8_t b) { pushRx(b); }
    /** The bit rate the control register selects (0: external clock). */
    double bitRate() const;
    /** Cycles one character takes. */
    uint64_t charCycles() const { return m_charCycles; }

private:
    enum class Link { None, Loopback, Tcp };
    enum TxState { TX_NONE = 0, TX_STARTED = 1, TX_DR_WRITTEN = 2 };

    void updateCharTime();
    void setIrq(bool on);
    void txEvent();
    void rxEvent();
    void send(uint8_t b);
    /** A byte on the line; on an idle line the receiver starts on its start bit. */
    void pushRx(uint8_t b);
    void pollTcp();
    void closeTcp();
    bool connected() const;

    std::string m_name = "ACIA";
    uint32_t m_base;
    uint32_t m_clockHz = 1000000;
    ISignalLine* m_irqLine = nullptr;

    uint8_t m_cmd = 0x02, m_ctrl = 0, m_status = 0x10;
    uint8_t m_txData = 0, m_rxData = 0;
    uint8_t m_dataMask = 0xFF;
    bool m_irq = false;
    TxState m_inTx = TX_NONE;
    uint64_t m_charCycles = 1;

    bool m_txActive = false, m_rxActive = false;
    uint64_t m_txCountdown = 0, m_rxCountdown = 0;
    std::deque<uint8_t> m_rxQueue;
    std::function<void(uint8_t)> m_txHook;
    uint64_t m_sent = 0, m_received = 0, m_overruns = 0;

    Link m_link = Link::None;
    int m_listenFd = -1, m_clientFd = -1;
    int m_tcpPort = 0;
    uint64_t m_pollCountdown = 0;
};
