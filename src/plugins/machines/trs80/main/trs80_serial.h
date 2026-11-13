#pragma once

#include "libdevices/main/io_handler.h"
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace trs80 {

/**
 * The RS-232-C interface (the Model I's 26-1145 board, the Model III/4's
 * RS-232 board): a TR1602 UART with a BR1941 baud rate generator, at ports
 * $E8-$EB.
 *
 *  - $E8 in: modem status (CTS 7, DSR 6, CD 5, RI 4, the receive line 1);
 *    out: UART master reset.
 *  - $E9 in: the board's sense switches (Model I: 8N1 and 9600 baud
 *    preset); out: baud rates, send (high nibble) and receive (low nibble):
 *    0-15 = 50, 75, 110, 134.5, 150, 300, 600, 1200, 1800, 2000, 2400,
 *    3600, 4800, 7200, 9600, 19200.
 *  - $EA in: status (data received 7, transmit holding register empty 6,
 *    overrun 5, framing 4, parity 3); out: control (even parity 7, word
 *    length 6-5, two stop bits 4, no parity 3, not break 2, DTR 1, RTS 0).
 *  - $EB: the data (reading takes the received byte).
 *
 * A byte written goes out after its frame time at the baud rate; a byte
 * arriving is received after the same. The far end (CLI `serial`):
 * `none`, `loopback`, `file:PATH` (transmit to a file), `tcp:PORT` (a
 * listening socket on localhost). On the Model III/4 the UART interrupts
 * (latch bits 4 send, 5 receive, 6 error) through onInterrupt.
 */
class Uart : public IOHandler {
public:
    ~Uart() override;
    const char* name() const override { return "RS-232"; }
    uint32_t baseAddr() const override { return 0xE8; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void reset() override;
    void tick(uint64_t) override {}
    bool attachSerial(const std::string& spec) override;
    void getDeviceInfo(DeviceInfo& out) const override;

    /** The port at `port & 3` ($E8-$EB). */
    uint8_t in(int reg);
    uint8_t peek(int reg) const;
    void out(int reg, uint8_t v);
    /** Time passes (master ticks). */
    void clock(uint64_t ticks);
    void setMasterHz(double hz) { m_hz = hz; }
    /** The sense switches $E9 reads. */
    void setSwitches(uint8_t v) { m_switches = v; }
    /** A byte from the far end (tests). */
    void receive(uint8_t b) { m_rxQueue.push_back(b); }
    /** The status bits as interrupt sources: (receive, send, error). */
    std::function<void(bool rcv, bool snd, bool err)> onInterrupt;

    std::vector<uint8_t> sent;              ///< every byte transmitted

private:
    enum class Link { None, Loopback, File, Tcp };
    uint64_t frameTicks() const;
    void transmitted(uint8_t b);
    void updateIrq();
    void pollTcp();
    void closeAll();

    double m_hz = 2027520.0;
    uint8_t m_status = 0x40, m_control = 0x6E, m_baud = 0xEE, m_switches = 0xFF, m_rx = 0;
    bool m_txBusy = false;
    uint8_t m_tx = 0;
    uint64_t m_txLeft = 0, m_rxLeft = 0, m_pollLeft = 0;
    std::deque<uint8_t> m_rxQueue;
    Link m_link = Link::None;
    FILE* m_file = nullptr;
    int m_listenFd = -1, m_clientFd = -1;
    std::string m_spec = "none";
};

/**
 * The parallel printer port (Model I $37E8; Model III/4 $37E8 and port
 * $F8): writing a byte prints it; the status reads busy (7), paper out
 * (6), selected (5) and no fault (4): $30, always ready. The paper is a
 * host file (CLI `serial printer:PATH`, `printer:none`) and `printed`.
 */
class Printer : public IOHandler {
public:
    ~Printer() override;
    const char* name() const override { return "Printer"; }
    uint32_t baseAddr() const override { return 0x37E8; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void reset() override {}
    void tick(uint64_t) override {}
    bool attachSerial(const std::string& spec) override;

    uint8_t status() const { return 0x30; }
    void write(uint8_t b);
    std::vector<uint8_t> printed;           ///< everything printed

private:
    FILE* m_file = nullptr;
};

}  // namespace trs80
