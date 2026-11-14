#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class SerialEndpoint;

/**
 * Intersil / Harris IM6402 CMOS UART (the Model 100/102's RS-232 and modem
 * UART), after the IM6402/IM6403 data sheet; MAME's im6402.cpp consulted.
 * mmsim's own code.
 *
 * The chip has no registers in the usual sense: its control word comes
 * from five pins loaded on CRL, its status flags are pins, and the receive
 * buffer drives the data bus when /RRD is low.
 *
 * Control (setControl, then the CRL strobe loadControl, or the one-step
 * writeControl), bits in the order the Model 100 wires them to port $D0:
 *  - bit 0 SBS   stop bits: 1 = 2 (1.5 with 5-bit characters), 0 = 1
 *  - bit 1 EPE   even parity (1) / odd (0)
 *  - bit 2 PI    parity inhibit: 1 = no parity bit
 *  - bit 3 CLS1, bit 4 CLS2: character length 5 + (CLS2 CLS1): 5, 6, 7, 8
 *
 * Status (status(), or the individual flags):
 *  - DR   data received (cleared by /DRR: dataReceivedReset())
 *  - TBRE transmitter buffer register empty
 *  - TRE  transmitter register empty (nothing being shifted out)
 *  - OE   overrun: a character arrived while DR was still set
 *  - FE   framing error, PE parity error (of the last character received)
 * status() packs them as bit 0 DR, 1 OE, 2 FE, 3 PE, 4 TBRE, 5 TRE.
 *
 * Clocks: the transmitter and receiver run from TRC and RRC, at 16 times
 * the bit rate. tickTx / tickRx advance them by a number of clock cycles
 * (rising edges); tick16x advances both (the Model 100 ties both to the
 * 81C55's TIMER OUT). A frame is 1 start + 5-8 data + 0/1 parity + 1/1.5/2
 * stop bits, 16 clocks per bit.
 *
 * Transmit: writeTbr loads the buffer (TBRE low); it moves to the transmit
 * register at once if that is empty (TBRE high again, TRE low). The byte
 * reaches the endpoint when its last stop bit ends (TRE high, or the next
 * buffered byte starts). tro() is the TRO pin's level as the bits go out.
 *
 * Receive: a byte from the endpoint is taken when the receiver is idle and
 * arrives in the RBR at the centre of its first stop bit (the data
 * sheet's DR timing), RRC clocks later; DR goes high (OE if it already was).
 * The far end is assumed to frame as configured; receive(b, pe, fe) injects
 * a character with errors. The received character is masked to its length.
 *
 * Master reset (MR): clears DR, OE, FE, PE, the RBR and both shift
 * registers; TBRE and TRE high, TRO marking. The control word is kept.
 *
 * Callbacks: onDr (the DR pin: RST 6.5 on the Model 100), onTbre, onTre.
 */
class Im6402 {
public:
    static constexpr uint8_t CTRL_SBS = 0x01;
    static constexpr uint8_t CTRL_EPE = 0x02;
    static constexpr uint8_t CTRL_PI = 0x04;
    static constexpr uint8_t CTRL_CLS1 = 0x08;
    static constexpr uint8_t CTRL_CLS2 = 0x10;

    static constexpr uint8_t ST_DR = 0x01;
    static constexpr uint8_t ST_OE = 0x02;
    static constexpr uint8_t ST_FE = 0x04;
    static constexpr uint8_t ST_PE = 0x08;
    static constexpr uint8_t ST_TBRE = 0x10;
    static constexpr uint8_t ST_TRE = 0x20;

    Im6402();
    ~Im6402();

    /** MR. */
    void masterReset();

    /** The five control pins (CTRL_* bits). */
    void setControl(uint8_t pins) { m_pins = pins & 0x1F; }
    /** CRL: latch the control pins. */
    void loadControl() { m_control = m_pins; }
    /** Pins then CRL in one go (the Model 100's OUT $D0). */
    void writeControl(uint8_t pins) { setControl(pins); loadControl(); }
    uint8_t control() const { return m_control; }

    int wordLength() const { return 5 + ((m_control >> 3) & 3); }
    bool parityEnabled() const { return !(m_control & CTRL_PI); }
    bool evenParity() const { return m_control & CTRL_EPE; }
    /** Stop bits x 2 (2, 3 or 4). */
    int stopHalfBits() const;
    /** RRC/TRC clocks per frame. */
    int frameClocks() const;

    /** TBRL: load the transmitter buffer. */
    void writeTbr(uint8_t v);
    /** The receiver buffer (the bus with /RRD low). */
    uint8_t readRbr() const { return m_rbr; }
    /** /DRR: clear DR. */
    void dataReceivedReset();
    /** RBR and then /DRR, as the Model 100's IN $C0 does. */
    uint8_t readData() {
        const uint8_t v = m_rbr;
        dataReceivedReset();
        return v;
    }

    uint8_t status() const;
    bool dr() const { return m_dr; }
    bool tbre() const { return m_tbre; }
    bool tre() const { return m_tre; }
    bool oe() const { return m_oe; }
    bool fe() const { return m_fe; }
    bool pe() const { return m_pe; }
    /** The TRO pin (true = mark). */
    bool tro() const;

    void tickTx(uint64_t clocks);
    void tickRx(uint64_t clocks);
    void tick16x(uint64_t clocks) {
        tickTx(clocks);
        tickRx(clocks);
    }

    /** The far end; null disconnects (the UART then sends into nothing). */
    void setEndpoint(std::shared_ptr<SerialEndpoint> ep) { m_endpoint = std::move(ep); }
    SerialEndpoint* endpoint() const { return m_endpoint.get(); }
    /** Replaces the endpoint from a spec ("loopback", "tcp:PORT", "tpdd:DIR", ...). */
    bool attachSerial(const std::string& spec);

    /** A character arrives from the far end directly (tests, errors). */
    void receive(uint8_t b, bool parityError = false, bool framingError = false);

    std::function<void(bool)> onDr, onTbre, onTre;

    /** Every byte that reached the far end (a log for tests and the debugger). */
    std::vector<uint8_t> sent;

    std::vector<uint8_t> saveState() const;
    void loadState(const std::vector<uint8_t>& s);

private:
    struct Pending {
        uint8_t b;
        bool pe, fe;
    };
    void setDr(bool v);
    void setTbre(bool v);
    void setTre(bool v);
    void startTx();

    uint8_t m_pins = 0x1E, m_control = 0x1E;   // 8 data bits, no parity, 1 stop
    uint8_t m_rbr = 0, m_tbr = 0, m_tsr = 0;
    bool m_dr = false, m_oe = false, m_fe = false, m_pe = false;
    bool m_tbre = true, m_tre = true;
    int m_txFrame = 0;                // clocks of the frame being sent
    uint64_t m_txPos = 0;             // clocks into it
    bool m_rxBusy = false;
    Pending m_rxChar{0, false, false};
    uint64_t m_rxLeft = 0;            // clocks until the character lands
    uint64_t m_pollAcc = 256;
    std::vector<Pending> m_injected;
    std::shared_ptr<SerialEndpoint> m_endpoint;
};
