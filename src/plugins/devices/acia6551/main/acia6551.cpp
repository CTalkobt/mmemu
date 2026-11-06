#include "acia6551.h"
#include "libdevices/main/device_info.h"
#include "libdevices/main/isignal_line.h"
#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

// Status bits
static constexpr uint8_t SR_OVERRUN = 0x04, SR_RDRF = 0x08, SR_TDRE = 0x10, SR_DCD = 0x20, SR_DSR = 0x40;
// DCD and DSR read 1 when the far end does not assert them (no link), as VICE's modem status
static constexpr uint8_t SR_ERRORS = 0x07;
// Command bits
static constexpr uint8_t CMD_DTR = 0x01, CMD_RX_IRQ_OFF = 0x02, CMD_TX_MASK = 0x0C;
static constexpr uint8_t CMD_TX_IRQ = 0x04, CMD_PARITY_ON = 0x20;

// The control register's rates (aciacore.c; 0 is 16x an external clock)
static const double BPS[16] = {
    0, 50, 75, 109.92, 134.58, 150, 300, 600, 1200, 1800,
    2400, 3600, 4800, 7200, 9600, 19200
};

Acia6551::Acia6551(uint32_t base) : m_base(base) { Acia6551::reset(); }

Acia6551::~Acia6551() { closeTcp(); }

void Acia6551::reset() {
    // Hardware reset: command 000000x0 (receive IRQ off), control 0, status
    // with transmit empty (and no carrier without a far end)
    m_cmd = CMD_RX_IRQ_OFF;
    m_ctrl = 0;
    m_status = SR_TDRE;
    m_inTx = TX_NONE;
    m_txActive = m_rxActive = false;
    m_rxQueue.clear();
    setIrq(false);
    updateCharTime();
}

double Acia6551::bitRate() const { return BPS[m_ctrl & 0x0F]; }

void Acia6551::updateCharTime() {
    int bits = 8 - ((m_ctrl >> 5) & 3);                  // 8, 7, 6, 5 data bits
    m_dataMask = (uint8_t)(0xFF >> (8 - bits));
    bits += 1                                           // start
          + ((m_cmd & CMD_PARITY_ON) ? 1 : 0)
          + ((m_ctrl & 0x80) ? 2 : 1);                  // stop (1.5 counted as 2)
    double bps = bitRate();
    if (bps <= 0) bps = 9600;                           // external clock: nothing to time it by
    m_charCycles = (uint64_t)((double)m_clockHz / bps * bits);
    if (m_charCycles == 0) m_charCycles = 1;
}

void Acia6551::setIrq(bool on) {
    if (on == m_irq) return;
    m_irq = on;
    if (m_irqLine) m_irqLine->set(on);                  // a shared IRQ source: true = asserted
}

bool Acia6551::connected() const {
    return m_link == Link::Loopback || (m_link == Link::Tcp && m_clientFd >= 0) || m_txHook;
}

// ---------------------------------------------------------------------------
// Registers
// ---------------------------------------------------------------------------

bool Acia6551::ioPeek(IBus*, uint32_t addr, uint8_t* val) {
    if ((addr & ~0x0Fu) != m_base) return false;
    switch (addr & 3) {
        case 0: *val = m_rxData; break;
        case 1: *val = (uint8_t)(m_status | (connected() ? 0 : SR_DCD | SR_DSR) | (m_irq ? 0x80 : 0)); break;
        case 2: *val = m_cmd; break;
        default: *val = m_ctrl; break;
    }
    return true;
}

bool Acia6551::ioRead(IBus* bus, uint32_t addr, uint8_t* val) {
    if ((addr & ~0x0Fu) != m_base) return false;
    ioPeek(bus, addr, val);
    switch (addr & 3) {
        case 0: m_status &= (uint8_t)~(SR_ERRORS | SR_RDRF); break;
        case 1: setIrq(false); break;
        default: break;
    }
    return true;
}

bool Acia6551::ioWrite(IBus*, uint32_t addr, uint8_t val) {
    if ((addr & ~0x0Fu) != m_base) return false;
    switch (addr & 3) {
    case 0:                                             // transmit
        m_txData = val;
        if (m_cmd & CMD_DTR) {
            m_inTx = TX_DR_WRITTEN;
            if (!m_txActive) { m_txActive = true; m_txCountdown = 1; }
            m_status &= (uint8_t)~SR_TDRE;
        }
        break;
    case 1:                                             // programmed reset
        m_status &= (uint8_t)~SR_OVERRUN;
        m_cmd &= 0xE0;                                  // keep parity
        m_inTx = TX_NONE;
        m_txActive = false;
        m_rxActive = false;
        setIrq(false);
        break;
    case 2: {                                           // command
        m_cmd = val;
        updateCharTime();
        bool txOn = (m_cmd & CMD_TX_MASK) != 0;
        if ((m_cmd & CMD_DTR) && txOn) {
            if (!m_rxActive) { m_rxActive = true; m_rxCountdown = m_charCycles; }
            if (!m_txActive) { m_txActive = true; m_txCountdown = m_charCycles; }
        } else {
            m_rxActive = false;                         // receiver and transmitter off
            if (!(m_cmd & CMD_DTR) || !txOn) m_txActive = false;
        }
        break;
    }
    default:                                            // control
        m_ctrl = val;
        updateCharTime();
        break;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------

void Acia6551::tick(uint64_t cycles) {
    if (m_link == Link::Tcp) {
        if (m_pollCountdown <= cycles) {
            pollTcp();
            m_pollCountdown = m_clockHz / 1000 + 1;      // every millisecond
        } else {
            m_pollCountdown -= cycles;
        }
    }
    if (m_txActive) {
        if (m_txCountdown <= cycles) txEvent(); else m_txCountdown -= cycles;
    }
    if (m_rxActive) {
        if (m_rxCountdown <= cycles) rxEvent(); else m_rxCountdown -= cycles;
    }
}

void Acia6551::txEvent() {
    if (m_inTx == TX_DR_WRITTEN) {                      // into the shift register
        send((uint8_t)(m_txData & m_dataMask));
        m_status |= SR_TDRE;
    }
    // Transmit IRQs come every character time, data or not (aciacore.c)
    if ((m_cmd & CMD_TX_MASK) == CMD_TX_IRQ) setIrq(true);
    if (m_inTx != TX_NONE) m_inTx = (TxState)(m_inTx - 1);
    if (m_inTx != TX_NONE || (m_cmd & CMD_TX_MASK) == CMD_TX_IRQ) {
        m_txCountdown = m_charCycles;
    } else {
        m_txActive = false;
    }
}

void Acia6551::rxEvent() {
    m_rxCountdown = m_charCycles;
    if (m_rxQueue.empty()) return;
    uint8_t b = m_rxQueue.front();
    m_rxQueue.pop_front();
    ++m_received;
    if (!(m_status & SR_RDRF)) {
        m_rxData = (uint8_t)(b & m_dataMask);
    } else {
        m_status |= SR_OVERRUN;                         // the new byte is lost
        ++m_overruns;
    }
    m_status |= SR_RDRF;
    if (!(m_cmd & CMD_RX_IRQ_OFF)) setIrq(true);
}

void Acia6551::pushRx(uint8_t b) {
    if (m_rxQueue.empty()) m_rxCountdown = m_charCycles;   // a whole character from its start bit
    m_rxQueue.push_back(b);
}

void Acia6551::send(uint8_t b) {
    ++m_sent;
    if (m_txHook) m_txHook(b);
    switch (m_link) {
        case Link::Loopback: pushRx(b); break;
        case Link::Tcp:
            if (m_clientFd >= 0 && ::send(m_clientFd, &b, 1, MSG_NOSIGNAL) < 0 && errno != EAGAIN)
                { ::close(m_clientFd); m_clientFd = -1; }
            break;
        default: break;
    }
}

// ---------------------------------------------------------------------------
// The far end
// ---------------------------------------------------------------------------

bool Acia6551::attachSerial(const std::string& spec) {
    closeTcp();
    m_link = Link::None;
    if (spec == "none" || spec.empty()) return true;
    if (spec == "loopback") { m_link = Link::Loopback; return true; }
    if (spec.rfind("tcp:", 0) == 0) {
        int port = std::atoi(spec.c_str() + 4);
        if (port <= 0 || port > 65535) return false;
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return false;
        int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons((uint16_t)port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(fd, (sockaddr*)&a, sizeof a) < 0 || ::listen(fd, 1) < 0) { ::close(fd); return false; }
        ::fcntl(fd, F_SETFL, O_NONBLOCK);
        m_listenFd = fd;
        m_tcpPort = port;
        m_link = Link::Tcp;
        return true;
    }
    return false;
}

void Acia6551::pollTcp() {
    if (m_clientFd < 0 && m_listenFd >= 0) {
        int c = ::accept(m_listenFd, nullptr, nullptr);
        if (c >= 0) { ::fcntl(c, F_SETFL, O_NONBLOCK); m_clientFd = c; }
    }
    if (m_clientFd < 0 || m_rxQueue.size() > 256) return;   // the far end waits (flow control)
    uint8_t buf[64];
    ssize_t n = ::recv(m_clientFd, buf, sizeof buf, 0);
    if (n > 0) {
        for (ssize_t i = 0; i < n; ++i) pushRx(buf[i]);
    } else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
        ::close(m_clientFd);                            // hung up
        m_clientFd = -1;
    }
}

void Acia6551::closeTcp() {
    if (m_clientFd >= 0) ::close(m_clientFd);
    if (m_listenFd >= 0) ::close(m_listenFd);
    m_clientFd = m_listenFd = -1;
}

void Acia6551::getDeviceInfo(DeviceInfo& out) const {
    out.name = m_name;
    out.baseAddr = m_base;
    out.addrMask = addrMask();
    char buf[64];
    std::snprintf(buf, sizeof buf, "$%02X", (unsigned)(m_status | (connected() ? 0 : SR_DCD | SR_DSR) | (m_irq ? 0x80 : 0)));
    out.state.push_back({"Status", buf});
    std::snprintf(buf, sizeof buf, "$%02X", m_cmd);
    out.state.push_back({"Command", buf});
    std::snprintf(buf, sizeof buf, "$%02X", m_ctrl);
    out.state.push_back({"Control", buf});
    std::snprintf(buf, sizeof buf, "%g bps, %d data bits%s, %s stop", bitRate(),
                  8 - ((m_ctrl >> 5) & 3), (m_cmd & CMD_PARITY_ON) ? " + parity" : "",
                  (m_ctrl & 0x80) ? "2" : "1");
    out.state.push_back({"Format", buf});
    std::string link = m_link == Link::Loopback ? "loopback"
                     : m_link == Link::Tcp ? ("tcp:" + std::to_string(m_tcpPort) +
                                              (m_clientFd >= 0 ? " (client connected)" : " (waiting)"))
                     : "none";
    out.state.push_back({"Link", link});
    out.state.push_back({"Sent / received", std::to_string(m_sent) + " / " + std::to_string(m_received)});
    if (m_overruns) out.state.push_back({"Overruns", std::to_string(m_overruns)});
}
