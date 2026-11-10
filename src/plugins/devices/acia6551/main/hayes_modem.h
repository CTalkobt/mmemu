#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

/**
 * A Hayes-compatible modem on a serial port's far end that "dials" the
 * internet: `ATDT host` or `ATDT host:port` opens a TCP connection (port
 * 23, telnet, by default). For the C64, C128, Plus/4 and MEGA65 terminal
 * programs (CCGMS, Novaterm, StrikeTerm...), so commands and result codes
 * are PETSCII by default.
 *
 * Command mode (a line ends with CR; backspace is PETSCII $14 or ASCII
 * $08/$7F; with echo on the line is echoed):
 *
 * | Command | |
 * |---------|--|
 * | `AT` | OK |
 * | `ATD`, `ATDT`, `ATDP` host[:port] | dial: CONNECT, or NO CARRIER |
 * | `ATH`, `ATH0` | hang up |
 * | `ATO` | back online after `+++` |
 * | `ATZ`, `AT&F` | the defaults (and hang up) |
 * | `ATE0/1` | echo off / on |
 * | `ATV0/1` | numeric / verbose result codes |
 * | `ATQ0/1` | result codes on / off |
 * | `ATI` | identify |
 * | `A/` | repeat the last command |
 * | `ATSn=v`, `ATSn?` | S-registers (stored; S2 the escape char) |
 * | `X`, `M`, `L`, `&C`, `&D`, `&K`, `&W` | accepted, no effect |
 * | `AT*P0/1` | commands and results in ASCII / PETSCII (default) |
 * | `AT*T0/1` | online data as is (default) / PETSCII<->ASCII translated |
 * | `AT*N0/1` | raw TCP / telnet (default: IAC answered and stripped) |
 *
 * Online, bytes pass through (C64 BBSes speak PETSCII over telnet). `+++`
 * with a second of silence before and after returns to command mode (OK).
 * The far end closing gives NO CARRIER. Carrier (DCD) is up while online.
 *
 * PETSCII <-> ASCII as terminal programs do it: PETSCII $41-$5A are ASCII
 * lower case, $C1-$DA (and $61-$7A) upper case.
 */
class HayesModem {
public:
    /** `toComputer` delivers a byte to the serial port's receiver;
     *  `room` says whether the receiver can take more (flow control). */
    HayesModem(std::function<void(uint8_t)> toComputer, std::function<bool()> room);
    ~HayesModem();

    /** A byte the computer sent. `seconds`: emulated time now. */
    void fromComputer(uint8_t b, double seconds);
    /** Time passes: the escape guard, dialing, the connection. */
    void poll(double seconds);
    /** Carrier detect: online with a connection. */
    bool carrier() const { return m_fd >= 0 && !m_dialing; }
    bool online() const { return carrier() && !m_commandMode; }
    bool dialing() const { return m_dialing; }
    void hangUp();
    /** ATZ: the defaults, hung up. */
    void reset();
    std::string state() const;

    static uint8_t petsciiToAscii(uint8_t c);
    static uint8_t asciiToPetscii(uint8_t c);

private:
    void command(const std::string& line);
    void result(int code);
    void sendText(const std::string& ascii);
    void out(uint8_t b) { m_toComputer(b); }
    void dial(const std::string& address);
    void finishDial(bool ok);
    void sendOnline(uint8_t b);
    void receive();
    void telnetByte(uint8_t b);
    void sendRaw(const uint8_t* p, size_t n);

    std::function<void(uint8_t)> m_toComputer;
    std::function<bool()> m_room;

    // Settings
    bool m_echo = true, m_verbose = true, m_quiet = false;
    bool m_petscii = true, m_translate = false, m_telnet = true;
    uint8_t m_s[16] = {};

    // Command mode
    bool m_commandMode = true;
    std::string m_line, m_lastLine;
    bool m_sawA = false;

    // The escape: +++ between two guard times
    double m_lastByte = -10.0;
    int m_plus = 0;
    double m_plusAt = 0;

    // The connection
    int m_fd = -1;
    bool m_dialing = false;
    std::chrono::steady_clock::time_point m_dialStart;
    int m_telnetState = 0;
    uint8_t m_telnetVerb = 0;
};
