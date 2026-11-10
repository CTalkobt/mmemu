#include "hayes_modem.h"
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
enum { RES_OK = 0, RES_CONNECT = 1, RES_NO_CARRIER = 3, RES_ERROR = 4 };
const char* const RESULT[] = {"OK", "CONNECT", "RING", "NO CARRIER", "ERROR"};
constexpr double GUARD = 1.0;                       // S12: 50ths of a second, 1 s
constexpr int DIAL_TIMEOUT_MS = 15000;
// Telnet
constexpr uint8_t IAC = 255, DONT = 254, DO = 253, WONT = 252, WILL = 251, SB = 250, SE = 240;
constexpr uint8_t OPT_ECHO = 1, OPT_SGA = 3;
}  // namespace

HayesModem::HayesModem(std::function<void(uint8_t)> toComputer, std::function<bool()> room)
    : m_toComputer(std::move(toComputer)), m_room(std::move(room)) {
    reset();
}

HayesModem::~HayesModem() { hangUp(); }

void HayesModem::reset() {
    hangUp();
    m_echo = m_verbose = true;
    m_quiet = false;
    m_petscii = true;
    m_translate = false;
    m_telnet = true;
    m_dcdFollowsCarrier = false;
    std::memset(m_s, 0, sizeof m_s);
    m_s[2] = '+';
    m_s[3] = 13;
    m_s[4] = 10;
    m_s[5] = 8;
    m_commandMode = true;
    m_line.clear();
    m_sawA = false;
    m_plus = 0;
}

uint8_t HayesModem::petsciiToAscii(uint8_t c) {
    if (c >= 0x41 && c <= 0x5A) return (uint8_t)(c + 0x20);    // unshifted: lower case
    if (c >= 0xC1 && c <= 0xDA) return (uint8_t)(c - 0x80);    // shifted: upper case
    if (c >= 0x61 && c <= 0x7A) return (uint8_t)(c - 0x20);
    if (c == 0x14) return 0x08;                                 // DEL
    return c;
}

uint8_t HayesModem::asciiToPetscii(uint8_t c) {
    if (c >= 'a' && c <= 'z') return (uint8_t)(c - 0x20);
    if (c >= 'A' && c <= 'Z') return (uint8_t)(c + 0x80);
    if (c == 0x08 || c == 0x7F) return 0x14;
    return c;
}

std::string HayesModem::state() const {
    if (m_dialing) return "dialing";
    if (m_fd >= 0) return m_commandMode ? "connected, command mode" : "online";
    return "command mode";
}

void HayesModem::sendText(const std::string& ascii) {
    for (char c : ascii) out(m_petscii ? asciiToPetscii((uint8_t)c) : (uint8_t)c);
}

void HayesModem::result(int code) {
    if (m_quiet) return;
    if (m_verbose) {
        // PETSCII has no line feed: a CR around the text
        sendText(m_petscii ? std::string("\r") + RESULT[code] + "\r" : std::string("\r\n") + RESULT[code] + "\r\n");
    } else {
        sendText(std::to_string(code) + "\r");
    }
}

// ---------------------------------------------------------------------------
// From the computer
// ---------------------------------------------------------------------------
void HayesModem::fromComputer(uint8_t b, double seconds) {
    const double quiet = seconds - m_lastByte;
    m_lastByte = seconds;
    if (online()) {
        // +++ after a second of silence (each + within the guard of the
        // last); the second of silence after it is checked in poll()
        if (b == m_s[2] && (m_plus > 0 ? quiet < GUARD : quiet >= GUARD)) {
            ++m_plus;
            m_plusAt = seconds;
        } else {
            m_plus = 0;
        }
        sendOnline(b);
        return;
    }
    // Command mode. Bit 7 is dropped but from PETSCII's shifted letters
    // ($C1-$DA, which are an Apple's capitals too): an Apple sends its
    // characters with bit 7 set ($8D is CR), a C64's shifted RETURN is $8D
    uint8_t c = b;
    if (c >= 0x80 && !(c >= 0xC1 && c <= 0xDA)) c &= 0x7F;
    const uint8_t a = m_petscii ? petsciiToAscii(c) : (uint8_t)(c & 0x7F);
    if (m_echo) out(b);
    if (a == '\r') {
        if (m_sawA) command(m_line);
        m_line.clear();
        m_sawA = false;
        return;
    }
    if (a == 0x08 || a == 0x7F) {
        if (!m_line.empty()) m_line.pop_back();
        else m_sawA = false;
        return;
    }
    if (!m_sawA) {
        if (a == 'A' || a == 'a') m_sawA = true;
        return;
    }
    if (m_line.empty() && a == '/') {               // A/: repeat
        m_sawA = false;
        command(m_lastLine);
        return;
    }
    if (a >= 0x20 && a < 0x7F) m_line += (char)a;
}

void HayesModem::command(const std::string& raw) {
    std::string line = raw;
    if (line.empty() || (toupper((unsigned char)line[0]) != 'T')) {
        if (!line.empty()) { result(RES_ERROR); return; }
    }
    m_lastLine = line;
    size_t i = line.empty() ? 0 : 1;                // past the T
    auto num = [&]() {
        int v = 0;
        bool any = false;
        while (i < line.size() && isdigit((unsigned char)line[i])) { v = v * 10 + (line[i++] - '0'); any = true; }
        return any ? v : 0;
    };
    while (i < line.size()) {
        const char c = (char)toupper((unsigned char)line[i++]);
        switch (c) {
            case ' ': break;
            case 'D': {                             // the rest of the line is the address
                if (i < line.size() && strchr("TPtp", line[i])) ++i;
                std::string addr;
                for (; i < line.size(); ++i)
                    if (line[i] != ' ' && line[i] != '"') addr += (char)tolower((unsigned char)line[i]);
                dial(addr);
                return;                             // the result comes when the call does
            }
            case 'H': num(); hangUp(); break;
            case 'O':
                num();
                if (m_fd < 0 || m_dialing) { result(RES_NO_CARRIER); return; }
                m_commandMode = false;
                result(RES_CONNECT);
                return;
            case 'Z': num(); reset(); break;
            case 'E': m_echo = num() != 0; break;
            case 'V': m_verbose = num() != 0; break;
            case 'Q': m_quiet = num() != 0; break;
            case 'I': num(); sendText(m_petscii ? "\rMMEMU HAYES MODEM (TCP)\r" : "\r\nMMEMU HAYES MODEM (TCP)\r\n"); break;
            case 'X': case 'M': case 'L': case 'B': case 'C': case 'N': case 'W': num(); break;
            case 'S': {
                const int r = num();
                if (i < line.size() && line[i] == '=') {
                    ++i;
                    const int v = num();
                    if (r < 16) m_s[r] = (uint8_t)v;
                } else if (i < line.size() && line[i] == '?') {
                    ++i;
                    sendText((m_petscii ? "\r" : "\r\n") + std::to_string(r < 16 ? m_s[r] : 0) + (m_petscii ? "\r" : "\r\n"));
                }
                break;
            }
            case '&': {
                if (i >= line.size()) { result(RES_ERROR); return; }
                const char f = (char)toupper((unsigned char)line[i++]);
                const int v = num();
                if (f == 'F') reset();
                else if (f == 'C') m_dcdFollowsCarrier = v != 0;
                else if (!strchr("DKWV", f)) { result(RES_ERROR); return; }
                break;
            }
            case '*': {                             // mmemu's extensions
                if (i >= line.size()) { result(RES_ERROR); return; }
                const char f = (char)toupper((unsigned char)line[i++]);
                const bool on = num() != 0;
                if (f == 'P') m_petscii = on;
                else if (f == 'T') m_translate = on;
                else if (f == 'N') m_telnet = on;
                else { result(RES_ERROR); return; }
                break;
            }
            default: result(RES_ERROR); return;
        }
    }
    result(RES_OK);
}

// ---------------------------------------------------------------------------
// The connection
// ---------------------------------------------------------------------------
void HayesModem::dial(const std::string& address) {
    hangUp();
    std::string host = address;
    std::string port = "23";
    const size_t colon = address.rfind(':');
    if (colon != std::string::npos) {
        host = address.substr(0, colon);
        port = address.substr(colon + 1);
    }
    if (host.empty() || port.empty() || port.find_first_not_of("0123456789") != std::string::npos) {
        result(RES_NO_CARRIER);
        return;
    }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res) {
        result(RES_NO_CARRIER);
        return;
    }
    int fd = -1;
    for (addrinfo* r = res; r; r = r->ai_next) {
        fd = ::socket(r->ai_family, r->ai_socktype, r->ai_protocol);
        if (fd < 0) continue;
        ::fcntl(fd, F_SETFL, O_NONBLOCK);
        if (::connect(fd, r->ai_addr, r->ai_addrlen) == 0 || errno == EINPROGRESS) break;
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) { result(RES_NO_CARRIER); return; }
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    m_fd = fd;
    m_dialing = true;
    m_dialStart = std::chrono::steady_clock::now();
    m_telnetState = 0;
}

void HayesModem::finishDial(bool ok) {
    m_dialing = false;
    if (!ok) {
        ::close(m_fd);
        m_fd = -1;
        result(RES_NO_CARRIER);
        return;
    }
    m_commandMode = false;
    m_plus = 0;
    result(RES_CONNECT);
}

void HayesModem::hangUp() {
    if (m_fd >= 0) ::close(m_fd);
    m_fd = -1;
    m_dialing = false;
    m_commandMode = true;
    m_plus = 0;
}

void HayesModem::sendRaw(const uint8_t* p, size_t n) {
    if (m_fd < 0) return;
    if (::send(m_fd, p, n, MSG_NOSIGNAL) < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        hangUp();
        result(RES_NO_CARRIER);
    }
}

void HayesModem::sendOnline(uint8_t b) {
    if (m_translate) b = petsciiToAscii(b);
    if (m_telnet && b == IAC) {                     // a data $FF is doubled
        const uint8_t two[2] = {IAC, IAC};
        sendRaw(two, 2);
        return;
    }
    sendRaw(&b, 1);
}

void HayesModem::poll(double seconds) {
    if (m_dialing) {
        pollfd p{m_fd, POLLOUT, 0};
        if (::poll(&p, 1, 0) > 0) {
            int err = 0;
            socklen_t len = sizeof err;
            ::getsockopt(m_fd, SOL_SOCKET, SO_ERROR, &err, &len);
            finishDial(err == 0);
        } else if (std::chrono::steady_clock::now() - m_dialStart > std::chrono::milliseconds(DIAL_TIMEOUT_MS)) {
            finishDial(false);
        }
        return;
    }
    // +++ and then a second of silence: command mode
    if (online() && m_plus >= 3 && seconds - m_plusAt >= GUARD) {
        m_plus = 0;
        m_commandMode = true;
        result(RES_OK);
    }
    if (m_fd >= 0) receive();
}

void HayesModem::receive() {
    // In command mode the far end's bytes wait (as a modem's buffer would)
    while (m_fd >= 0 && !m_commandMode && m_room()) {
        uint8_t buf[64];
        const ssize_t n = ::recv(m_fd, buf, sizeof buf, 0);
        if (n > 0) {
            for (ssize_t i = 0; i < n; ++i) {
                if (m_telnet) telnetByte(buf[i]);
                else out(m_translate ? asciiToPetscii(buf[i]) : buf[i]);
            }
            if (n < (ssize_t)sizeof buf) return;
        } else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            hangUp();                               // the far end hung up
            result(RES_NO_CARRIER);
            return;
        } else {
            return;
        }
    }
}

// Telnet (RFC 854): IAC IAC is a data $FF; DO/DONT/WILL/WONT are answered
// (the server may echo and suppress go-ahead; nothing else is offered);
// subnegotiations are skipped
void HayesModem::telnetByte(uint8_t b) {
    switch (m_telnetState) {
        case 0:
            if (b == IAC) { m_telnetState = 1; return; }
            out(m_translate ? asciiToPetscii(b) : b);
            return;
        case 1:
            if (b == IAC) { m_telnetState = 0; out(m_translate ? asciiToPetscii(b) : b); return; }
            if (b >= WILL && b <= DONT) { m_telnetVerb = b; m_telnetState = 2; return; }
            if (b == SB) { m_telnetState = 3; return; }
            m_telnetState = 0;                      // NOP, GA and the like
            return;
        case 2: {
            // WONT and DONT need no answer: nothing was asked for, and
            // answering them can loop
            uint8_t reply = 0;
            if (m_telnetVerb == WILL) reply = (b == OPT_ECHO || b == OPT_SGA) ? DO : DONT;
            else if (m_telnetVerb == DO) reply = b == OPT_SGA ? WILL : WONT;
            if (reply) {
                const uint8_t r[3] = {IAC, reply, b};
                sendRaw(r, 3);
            }
            m_telnetState = 0;
            return;
        }
        case 3:                                     // in a subnegotiation, until IAC SE
            if (b == IAC) m_telnetState = 4;
            return;
        default:
            m_telnetState = b == SE ? 0 : 3;
            return;
    }
}
