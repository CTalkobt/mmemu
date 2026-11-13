// The TRS-80's RS-232-C board (TR1602 UART) and parallel printer port,
// after the Model I/III technical reference manuals and xtrs's trs_uart.c
// (register layout and the default switches); mmsim's own code.

#include "trs80_serial.h"
#include <arpa/inet.h>
#include <cstdlib>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace trs80 {

namespace {
// BR1941 rates for codes 0-15
const double BAUD[16] = {50, 75, 110, 134.5, 150, 300, 600, 1200, 1800, 2000, 2400, 3600, 4800, 7200, 9600, 19200};
constexpr uint8_t ST_RCVD = 0x80, ST_SENT = 0x40, ST_OVERRUN = 0x20;
}  // namespace

Uart::~Uart() { closeAll(); }

void Uart::closeAll() {
    if (m_file) { std::fclose(m_file); m_file = nullptr; }
    if (m_clientFd >= 0) { ::close(m_clientFd); m_clientFd = -1; }
    if (m_listenFd >= 0) { ::close(m_listenFd); m_listenFd = -1; }
}

void Uart::reset() {
    m_status = ST_SENT;
    m_txBusy = false;
    m_txLeft = m_rxLeft = 0;
    m_rx = 0;
    updateIrq();
}

bool Uart::attachSerial(const std::string& spec) {
    if (spec.rfind("printer:", 0) == 0) return false;     // the printer's
    Link link = Link::None;
    FILE* file = nullptr;
    int fd = -1;
    if (spec == "none" || spec.empty()) link = Link::None;
    else if (spec == "loopback") link = Link::Loopback;
    else if (spec.rfind("file:", 0) == 0) {
        file = std::fopen(spec.c_str() + 5, "wb");
        if (!file) return false;
        link = Link::File;
    } else if (spec.rfind("tcp:", 0) == 0) {
        const int port = std::atoi(spec.c_str() + 4);
        if (port <= 0 || port > 65535) return false;
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return false;
        int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons((uint16_t)port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(fd, (sockaddr*)&a, sizeof a) < 0 || ::listen(fd, 1) < 0) { ::close(fd); return false; }
        ::fcntl(fd, F_SETFL, O_NONBLOCK);
        link = Link::Tcp;
    } else {
        return false;
    }
    closeAll();
    m_link = link;
    m_file = file;
    m_listenFd = fd;
    m_spec = spec;
    return true;
}

uint64_t Uart::frameTicks() const {
    // Start bit, data, parity, stop bits at the send rate
    static const int WORD[4] = {5, 7, 6, 8};
    const int bits = 1 + WORD[(m_control >> 5) & 3] + ((m_control & 0x08) ? 0 : 1) + ((m_control & 0x10) ? 2 : 1);
    return (uint64_t)(m_hz * bits / BAUD[(m_baud >> 4) & 15]);
}

uint8_t Uart::peek(int reg) const {
    switch (reg & 3) {
    case 0: return (uint8_t)(0x80 | 0x40 | 0x20 | (m_link == Link::None ? 0 : 0x02));  // CTS DSR CD, the line marking
    case 1: return m_switches;
    case 2: return m_status;
    default: return m_rx;
    }
}

uint8_t Uart::in(int reg) {
    const uint8_t v = peek(reg);
    if ((reg & 3) == 3 && (m_status & ST_RCVD)) {
        m_status &= (uint8_t)~ST_RCVD;
        updateIrq();
    }
    return v;
}

void Uart::out(int reg, uint8_t v) {
    switch (reg & 3) {
    case 0: reset(); break;
    case 1: m_baud = v; break;
    case 2: m_control = v; break;
    case 3:
        // The holding register: sent once the shift register is free
        m_tx = v;
        m_status &= (uint8_t)~ST_SENT;
        if (!m_txBusy) { m_txBusy = true; m_txLeft = frameTicks(); }
        updateIrq();
        break;
    }
}

void Uart::transmitted(uint8_t b) {
    sent.push_back(b);
    switch (m_link) {
    case Link::Loopback: m_rxQueue.push_back(b); break;
    case Link::File: std::fputc(b, m_file); std::fflush(m_file); break;
    case Link::Tcp:
        if (m_clientFd >= 0 && ::send(m_clientFd, &b, 1, MSG_NOSIGNAL) < 0) { ::close(m_clientFd); m_clientFd = -1; }
        break;
    default: break;
    }
}

void Uart::pollTcp() {
    if (m_clientFd < 0 && m_listenFd >= 0) {
        const int c = ::accept(m_listenFd, nullptr, nullptr);
        if (c >= 0) { ::fcntl(c, F_SETFL, O_NONBLOCK); m_clientFd = c; }
    }
    if (m_clientFd < 0 || m_rxQueue.size() > 256) return;
    uint8_t buf[64];
    const ssize_t n = ::recv(m_clientFd, buf, sizeof buf, 0);
    if (n > 0) m_rxQueue.insert(m_rxQueue.end(), buf, buf + n);
    else if (n == 0) { ::close(m_clientFd); m_clientFd = -1; }
}

void Uart::clock(uint64_t ticks) {
    if (m_txBusy) {
        if (m_txLeft > ticks) m_txLeft -= ticks;
        else {
            transmitted(m_tx);
            m_txBusy = false;
            m_status |= ST_SENT;
            updateIrq();
        }
    }
    if (m_link == Link::Tcp) {
        if (m_pollLeft > ticks) m_pollLeft -= ticks;
        else { m_pollLeft = (uint64_t)(m_hz / 1000); pollTcp(); }
    }
    if (!m_rxQueue.empty()) {
        if (!m_rxLeft) m_rxLeft = frameTicks();
        if (m_rxLeft > ticks) m_rxLeft -= ticks;
        else {
            m_rxLeft = 0;
            if (m_status & ST_RCVD) m_status |= ST_OVERRUN;
            m_rx = m_rxQueue.front();
            m_rxQueue.pop_front();
            m_status |= ST_RCVD;
            updateIrq();
        }
    }
}

void Uart::updateIrq() {
    if (onInterrupt) onInterrupt(m_status & ST_RCVD, m_status & ST_SENT, m_status & 0x38);
}

void Uart::getDeviceInfo(DeviceInfo& out) const {
    out.name = "RS-232 (TR1602)";
    out.baseAddr = 0xE8;
    out.addrMask = 3;
    char buf[64];
    std::snprintf(buf, sizeof buf, "$%02X", m_status);
    out.state.push_back({"Status", buf});
    std::snprintf(buf, sizeof buf, "$%02X", m_control);
    out.state.push_back({"Control", buf});
    std::snprintf(buf, sizeof buf, "%g / %g bps", BAUD[(m_baud >> 4) & 15], BAUD[m_baud & 15]);
    out.state.push_back({"Send / receive", buf});
    out.state.push_back({"Link", m_spec + (m_link == Link::Tcp ? (m_clientFd >= 0 ? " (client connected)" : " (waiting)") : "")});
}

Printer::~Printer() {
    if (m_file) std::fclose(m_file);
}

bool Printer::attachSerial(const std::string& spec) {
    if (spec.rfind("printer:", 0) != 0) return false;
    const std::string path = spec.substr(8);
    FILE* f = nullptr;
    if (path != "none" && !path.empty()) {
        f = std::fopen(path.c_str(), "wb");
        if (!f) return false;
    }
    if (m_file) std::fclose(m_file);
    m_file = f;
    return true;
}

void Printer::write(uint8_t b) {
    printed.push_back(b);
    if (m_file) { std::fputc(b, m_file); std::fflush(m_file); }
}

}  // namespace trs80
