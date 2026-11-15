#pragma once

#include "plugins/devices/serial_endpoint/main/serial_endpoint.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

/**
 * Motorola MC14412 300-baud FSK modem (Bell 103 / CCITT V.21), as the
 * Model 100 and Tandy 200 use it, with the phone line it is wired to.
 * mmsim's own code; the pin names follow the MC14412 data sheet and the
 * wiring follows MAME (kyocera.cpp: the 100's `modem_w`, the 200's
 * `i8155_pb_w`) and VirtualT (io.c, port $A0).
 *
 * The chip modulates the UART's TxD and demodulates the line into RxD, so
 * at the byte level it is a pass-through: this class is the SerialEndpoint
 * the UART sends into (while the computer has the modem selected), and it
 * passes bytes on to the "phone line" - any other SerialEndpoint (a TCP
 * socket, a capture file, a loopback; CLI `serial modem:SPEC`).
 *
 * Inputs (set by the machine):
 *  - enable (the 100's $A0 bit 1, the 200's MEN): the chip transmitting
 *    its carrier; the Model 100's ROM turns it on only once it has heard
 *    the far end's tone (originate) or as it answers;
 *  - off hook: the line held (the 100: $A0 bit 0 with port B bit 7 low,
 *    the 200: CALL, port B bit 7); the ROM pulse-dials by toggling it;
 *  - originate (the 100's ORIG/ANS switch, the 200's port B bit 1).
 *
 * The far end: present when the line endpoint says so (dcd(): a TCP
 * client connected, or always for loopback / file). It is taken to be a
 * modem in the other mode: CARRIER_DELAY seconds (mmsim's choice,
 * unverified) after the line is off hook with a far end, its tone is on
 * the line - lineSignal() is a square wave at its mark frequency (2225 Hz
 * heard by an originating modem, 1270 Hz by an answering one) and
 * carrier() is true - and it goes at once when the line is dropped. The
 * Model 100's ROM detects that tone itself by timing lineSignal()'s edges
 * ($D0 bit 0); the 200 reads carrier() as its CD input.
 * Bytes pass when there is carrier and the chip is enabled; otherwise what
 * the UART sends is lost (the line is silent) and nothing arrives.
 *
 * Ringing: while on hook with the far end present (a TCP client has
 * connected: "an incoming call"), ring() follows the US cadence (2 s on,
 * 4 s off). Which pin, and its polarity, the 100 reads it on is unverified
 * (the $D0 status bit 5 "RP", pulled up).
 *
 * Pulse dialling: on-hook breaks shorter than 150 ms while dialling count
 * as pulses, a make longer than 250 ms ends a digit (10 pulses = 0); the
 * digits are collected in dialled() (shown by the debugger; nothing is
 * done with them).
 *
 * Time: tick() with CPU clocks and the clock's rate.
 */
class Mc14412 : public SerialEndpoint {
public:
    static constexpr double CARRIER_DELAY = 0.1;
    static constexpr double PULSE_MAX_BREAK = 0.15;
    static constexpr double DIGIT_GAP = 0.25;

    explicit Mc14412(double clockHz = 2457600.0) : m_hz(clockHz) {}

    /** The phone line's far end (null: no line). */
    void setLine(std::shared_ptr<SerialEndpoint> line);
    /** The line from a spec (`tcp:PORT`, `file:PATH`, `loopback`, `none`, ...); false if it fails. */
    bool attachLine(const std::string& spec);
    SerialEndpoint* line() const { return m_line.get(); }

    void setEnable(bool on);
    void setOffHook(bool off);
    void setOriginate(bool orig) { m_originate = orig; }
    bool enabled() const { return m_enable; }
    bool offHook() const { return m_offHook; }
    bool originate() const { return m_originate; }

    /** Carrier detected (bytes pass). */
    bool carrier() const { return m_carrier; }
    /** Bytes pass: carrier, and the chip enabled. */
    bool connected() const { return m_carrier && m_enable && m_line; }
    /**
     * The received line signal squared up: while there is carrier, a square
     * wave at the far modem's mark frequency (originate: the answerer's
     * 2225 Hz; answer: the originator's 1270 Hz); otherwise low. The Model
     * 100's ROM times its edges to detect the carrier itself.
     */
    bool lineSignal() const;
    /** The far end's tone frequency (Hz) lineSignal() plays. */
    double toneHz() const { return m_originate ? 2225.0 : 1270.0; }
    /** The ring signal (true while ringing). */
    bool ring() const;
    /** Pulse-dialled digits since the last time the line went off hook after being on hook a while. */
    const std::string& dialled() const { return m_dialled; }

    void tick(uint64_t clocks);

    // SerialEndpoint: the UART side
    void send(uint8_t b) override;
    bool receive(uint8_t& b) override;
    void poll() override;
    std::string describe() const override;
    bool dcd() const override { return m_carrier; }
    bool cts() const override { return connected(); }
    bool dsr() const override { return m_enable; }

    /** Bytes sent to the line (tests, the debugger). */
    std::vector<uint8_t> sentToLine;

    std::vector<uint8_t> saveState() const;
    void loadState(const std::vector<uint8_t>& s);

private:
    bool farEnd() const { return m_line && m_line->dcd(); }
    void hook(bool off);

    double m_hz;
    std::shared_ptr<SerialEndpoint> m_line;
    std::string m_spec = "none";
    bool m_enable = false, m_offHook = false, m_originate = true;
    bool m_carrier = false;
    uint64_t m_now = 0;
    uint64_t m_cdWait = 0;          // clocks of the conditions holding
    uint64_t m_hookChange = 0;      // when the relay last changed
    uint64_t m_ringStart = 0;       // when the far end appeared on hook
    bool m_farPrev = false;
    int m_pulses = 0;
    bool m_inBreak = false;         // on hook after having been off hook
    std::string m_dialled;
};
