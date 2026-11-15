#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class SerialEndpoint;

/**
 * Intel 8251A USART (#334: the Tandy Model 200's RS-232 port at $C0/$C1),
 * asynchronous mode. After the Intel 8251A data sheet (programming the
 * 8251A, the status register); MAME's i8251.cpp consulted for the reset
 * sequence. mmsim's own code.
 *
 * Offset 1 (C/D high) is control on write and status on read; offset 0 is
 * data. After a reset the first control write is the mode instruction,
 * then command instructions until one with IR (bit 6) set returns the chip
 * to expecting a mode.
 *
 * Mode (async): bits 1-0 the baud factor (01 x1, 10 x16, 11 x64; 00 means
 * synchronous, which is accepted but not run: leftover), 3-2 the character
 * length (5-8 bits), 4 parity enable, 5 even parity, 7-6 the stop bits (01
 * one, 10 one and a half, 11 two).
 *
 * Command: 0 TxEN, 1 DTR, 2 RxE, 3 SBRK, 4 ER (clears PE, OE, FE), 5 RTS,
 * 6 IR (internal reset), 7 EH (sync only).
 *
 * Status: 0 TxRDY (the buffer is free; as the data sheet says, the status
 * bit is not gated by /CTS or TxEN), 1 RxRDY, 2 TxEMPTY, 3 PE, 4 OE, 5 FE,
 * 6 SYNDET/BRKDET (0), 7 DSR (the far end's DSR asserted).
 *
 * Timing: the transmitter and receiver clocks (TxC, RxC) are counted by
 * tick() / tickTx() / tickRx() (on the Model 200, the 81C55's TIMER OUT); a
 * frame takes (1 + bits + parity + stop) x factor clocks. A character from
 * the far end lands in the receive buffer at the middle of its first stop
 * bit; RxRDY (onRxRdy: the Model 200's RST 6.5) rises then if the receiver
 * is enabled. /CTS is not modelled (the transmitter runs whenever TxEN).
 */
class I8251 {
public:
    static constexpr uint8_t ST_TXRDY = 0x01;
    static constexpr uint8_t ST_RXRDY = 0x02;
    static constexpr uint8_t ST_TXEMPTY = 0x04;
    static constexpr uint8_t ST_PE = 0x08;
    static constexpr uint8_t ST_OE = 0x10;
    static constexpr uint8_t ST_FE = 0x20;
    static constexpr uint8_t ST_SYNDET = 0x40;
    static constexpr uint8_t ST_DSR = 0x80;

    static constexpr uint8_t CMD_TXEN = 0x01;
    static constexpr uint8_t CMD_DTR = 0x02;
    static constexpr uint8_t CMD_RXE = 0x04;
    static constexpr uint8_t CMD_SBRK = 0x08;
    static constexpr uint8_t CMD_ER = 0x10;
    static constexpr uint8_t CMD_RTS = 0x20;
    static constexpr uint8_t CMD_IR = 0x40;
    static constexpr uint8_t CMD_EH = 0x80;

    I8251();
    ~I8251();

    /** The RESET pin: expects a mode instruction, transmitter idle, buffers empty. */
    void reset();

    /** offset & 1: 1 status, 0 the received character (clears RxRDY). */
    uint8_t read(int offset);
    /** Without side effects. */
    uint8_t peek(int offset) const { return (offset & 1) ? status() : m_rbr; }
    /** offset & 1: 1 mode / command, 0 a character to send. */
    void write(int offset, uint8_t v);
    void writeControl(uint8_t v);
    void writeData(uint8_t v);
    uint8_t readData();
    uint8_t status() const;

    uint8_t mode() const { return m_mode; }
    uint8_t command() const { return m_command; }
    bool expectingMode() const { return m_expectMode; }
    bool synchronous() const { return (m_mode & 3) == 0; }
    /** Clocks per bit (1, 16 or 64). */
    int factor() const;
    int wordLength() const { return 5 + ((m_mode >> 2) & 3); }
    bool parityEnabled() const { return m_mode & 0x10; }
    bool evenParity() const { return m_mode & 0x20; }
    /** Stop bits x 2 (2, 3 or 4). */
    int stopHalfBits() const;
    /** TxC / RxC clocks per frame. */
    int frameClocks() const;

    bool rxRdy() const { return m_rxRdy && (m_command & CMD_RXE); }
    bool txRdy() const { return m_tbrEmpty; }
    bool txEmpty() const { return m_tbrEmpty && !m_txBusy; }
    bool dtr() const { return m_command & CMD_DTR; }
    bool rts() const { return m_command & CMD_RTS; }

    /** TxC and RxC tied together (the Model 200). */
    void tick(uint64_t clocks) {
        tickRx(clocks);         // first: a character sent now is not received in the same clocks
        tickTx(clocks);
    }
    void tickTx(uint64_t clocks);
    void tickRx(uint64_t clocks);

    /** The far end; null disconnects. */
    void setEndpoint(std::shared_ptr<SerialEndpoint> ep);
    SerialEndpoint* endpoint() const { return m_endpoint.get(); }
    /** Replaces the endpoint from a spec ("loopback", "tcp:PORT", "tpdd:DIR", ...). */
    bool attachSerial(const std::string& spec);
    /** A character arrives directly (tests). */
    void receive(uint8_t b, bool parityError = false, bool framingError = false);

    /** The RxRDY pin (the Model 200's RST 6.5). */
    std::function<void(bool)> onRxRdy;
    /** The TxRDY pin (buffer free, TxEN and the command written). */
    std::function<void(bool)> onTxRdy;

    /** Every character that reached the far end. */
    std::vector<uint8_t> sent;

    std::vector<uint8_t> saveState() const;
    void loadState(const std::vector<uint8_t>& s);

private:
    struct Pending {
        uint8_t b;
        bool pe, fe;
    };
    void updateRxRdyPin();
    void updateTxRdyPin();
    void startTx();

    uint8_t m_mode = 0x4E;            // async x16, 8 bits, no parity, 1 stop
    uint8_t m_command = 0;
    bool m_expectMode = true;
    uint8_t m_rbr = 0, m_tbr = 0, m_tsr = 0;
    bool m_rxRdy = false, m_pe = false, m_oe = false, m_fe = false;
    bool m_tbrEmpty = true, m_txBusy = false;
    uint64_t m_txLeft = 0;
    bool m_rxBusy = false;
    Pending m_rxChar{0, false, false};
    uint64_t m_rxLeft = 0, m_rxTail = 0;
    uint64_t m_pollAcc = 256;
    bool m_rxPin = false, m_txPin = false;
    std::vector<Pending> m_injected;
    std::shared_ptr<SerialEndpoint> m_endpoint;
};
