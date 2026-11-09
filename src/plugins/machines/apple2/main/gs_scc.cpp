// The IIgs's Zilog 8530 SCC, after KEGS by Kent Dickey (GPL-3.0; scc.c:
// scc_read_reg, scc_write_reg, scc_regen_clocks, scc_evaluate_ints,
// scc_do_event and the Rx/Tx paths).

#include "gs_scc.h"
#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace apple2 {

namespace {
constexpr double XTAL_HZ = 3686400.0;            // the baud rate crystal
constexpr double PCLK_HZ = 28636363.0 / 8.0;     // the system clock / 8
constexpr uint32_t DPLL_BRG = 0x100, DPLL_RTXC = 0x200;
}  // namespace

GsScc::GsScc(double slowHz) : m_slowHz(slowHz) { reset(); }

GsScc::~GsScc() { closeTcp(0); closeTcp(1); }

// scc_reset: both channels; DCD high on port 1 only (KEGS)
void GsScc::reset() {
    for (int p = 0; p < 2; ++p) {
        Channel& c = m_ch[p];
        c.mode = c.ptr = 0;
        c.inBuf.clear();
        c.dcd = p == 0;
        c.wantRx = c.wantTx = c.wantExt = false;
        hardResetPort(p);
    }
}

void GsScc::hardResetPort(int p) {
    resetPort(p);
    Channel& c = m_ch[p];
    c.reg[14] = 0; c.reg[13] = 0; c.reg[12] = 0; c.reg[11] = 0x08; c.reg[10] = 0;
    c.reg[7] = 0; c.reg[6] = 0; c.reg[5] = 0; c.reg[4] = 0x04; c.reg[3] = 0; c.reg[2] = 0; c.reg[1] = 0;
    m_ch[0].reg[9] = 0;                                 // interrupts off
    regenClocks(p);
}

void GsScc::resetPort(int p) {
    Channel& c = m_ch[p];
    c.reg[15] = 0xF8;
    c.reg[14] &= 0x03;
    c.reg[10] = 0;
    c.reg[5] &= 0x65;
    c.reg[4] |= 0x04;
    c.reg[3] &= 0xFE;
    c.reg[1] &= 0xFE;
    c.brZero = false;
    c.txEmpty = true;
    c.wantRx = c.wantTx = c.wantExt = false;
    c.rxQueue.clear();
    c.txLeft = c.rxLeft = c.brLeft = -1;
    regenClocks(p);
}

// scc_regen_clocks: cycles a character takes, from the clock sources,
// multiplier and character format
void GsScc::regenClocks(int p) {
    Channel& c = m_ch[p];
    const double perXtal = m_slowHz / XTAL_HZ, perPclk = m_slowHz / PCLK_HZ;
    static const double MULT[4] = {1, 16, 32, 64};
    static const double BITS[4] = {5, 7, 6, 8};
    const double mult = MULT[(c.reg[4] >> 6) & 3];
    double br = 0.01;
    if (c.reg[14] & 1) br = (c.reg[14] & 2) ? perPclk : perXtal;
    br *= (double)(((c.reg[13] << 8) | c.reg[12]) + 2) * 2.0;
    double dpll = 0.1;
    if (c.reg[14] & DPLL_BRG) dpll = br;
    else if (c.reg[14] & DPLL_RTXC) dpll = perXtal;
    auto src = [&](int sel) { return sel == 0 ? perXtal : sel == 2 ? br : sel == 3 ? dpll : 1.0; };
    const double tx = src((c.reg[11] >> 3) & 3) * mult, rx = src((c.reg[11] >> 5) & 3) * mult;
    double size = BITS[(c.reg[5] >> 5) & 3];
    switch ((c.reg[4] >> 2) & 3) {
        case 1: size += 2.0; break;                     // start + 1 stop
        case 2: size += 2.5; break;
        case 3: size += 3.0; break;
    }
    if (c.reg[4] & 1) size += 1.0;                      // parity
    if (c.reg[14] & 0x10) size = 1.0;                   // local loopback: quick
    c.brCycles = br;
    c.txCycles = tx * size;
    c.rxCycles = rx * size;
}

uint32_t GsScc::pending() const {
    if (!mie()) return 0;
    uint32_t v = 0;
    for (int p = 0; p < 2; ++p) {
        const Channel& c = m_ch[p];
        const uint32_t bits = (c.wantExt ? 1u : 0u) | (c.wantTx ? 2u : 0u) | (c.wantRx ? 4u : 0u);
        v |= p == 0 ? bits << 3 : bits;
    }
    return v;
}

bool GsScc::irq() const { return pending() != 0; }

// scc_read_reg
uint8_t GsScc::readReg(int p) {
    Channel& c = m_ch[p];
    c.mode = 0;
    uint32_t ret = 0;
    switch (c.ptr) {
        case 0: case 4:
            ret = 0x60;                                 // CTS, Tx underrun
            if (c.dcd) ret |= 0x08;
            if (!c.rxQueue.empty()) ret |= 0x01;
            if (c.txEmpty) ret |= 0x04;
            if (c.brZero) ret |= 0x02;
            break;
        case 1: case 5: ret = 0x07; break;              // all sent
        case 2: case 6:
            if (p == 0) ret = m_ch[0].reg[2];
            else {                                      // RR2B: the vector with the pending interrupt
                const uint32_t v = pending();
                uint32_t code = 3;
                if (v & 0x20) code = 6; else if (v & 0x10) code = 4; else if (v & 0x08) code = 5;
                else if (v & 0x04) code = 2; else if (v & 0x02) code = 0; else if (v & 0x01) code = 1;
                ret = code << 1;
                if (m_ch[0].reg[9] & 0x10) ret = ((ret << 1) & 0x10) | ((ret << 3) & 0x20) | ((ret << 5) & 0x40);
            }
            break;
        case 3: case 7: ret = p == 0 ? pending() : 0; break;
        case 8: ret = readData(p); break;
        case 9: case 13: ret = c.reg[13]; break;
        case 10: case 14: ret = 0; break;
        case 11: case 15: ret = c.reg[15]; break;
        case 12: ret = c.reg[12]; break;
    }
    c.ptr = 0;
    return (uint8_t)ret;
}

// scc_write_reg
void GsScc::writeReg(int p, uint8_t val) {
    Channel& c = m_ch[p];
    int regnum = c.ptr & 0xF;
    if (c.mode == 0) {
        if ((val & 0xF0) == 0) { c.ptr = val & 0xF; regnum = 0; c.mode = 1; }
    } else {
        c.ptr = 0;
        c.mode = 0;
    }
    const uint32_t changed = (c.reg[regnum] ^ val) & 0xFF;
    switch (regnum) {
        case 0:
            switch ((val >> 3) & 7) {
                case 2: c.wantExt = false; break;       // reset ext/status interrupts
                case 5: c.wantTx = false; break;        // reset Tx interrupt pending
                case 7:                                 // reset the highest interrupt under service
                    if (c.wantRx) c.wantRx = false;
                    else if (c.wantTx) c.wantTx = false;
                    else c.wantExt = false;
                    break;
            }
            return;
        case 8: writeData(p, val); return;
        case 9:
            if (val & 0x80) resetPort(0);
            if (val & 0x40) resetPort(1);
            if ((val & 0xC0) == 0xC0) { hardResetPort(0); hardResetPort(1); }
            m_ch[0].reg[9] = val;                       // shared by both channels
            maybeBrEvent(0);
            maybeBrEvent(1);
            return;
        case 14: {
            uint32_t v = val | (c.reg[14] & ~0xFFu);
            switch ((v >> 5) & 7) {
                case 1: case 2: case 3: case 6: case 7: v &= 0xFF; break;
                case 4: v = (v & 0xFF) | DPLL_BRG; break;
                case 5: v = (v & 0xFF) | DPLL_RTXC; break;
            }
            const bool diff = v != c.reg[14];
            c.reg[14] = v;
            if (diff) regenClocks(p);
            maybeBrEvent(p);
            return;
        }
        case 15:
            c.reg[15] = val;
            maybeBrEvent(p);
            return;
        default:
            c.reg[regnum] = val;
            if ((regnum == 4 || regnum == 11 || regnum == 12 || regnum == 13 || (regnum == 5 && (changed & 0x60))) && changed)
                regenClocks(p);
            return;
    }
}

// scc_read_data: the oldest character in the FIFO
uint8_t GsScc::readData(int p) {
    Channel& c = m_ch[p];
    if (c.link == Link::Tcp) pollTcp(p);
    uint8_t ret = 0;
    if (!c.rxQueue.empty()) {
        ret = c.rxQueue.front();
        c.rxQueue.pop_front();
        rxEvent(p);
        rxInt(p);
    }
    return ret;
}

// scc_write_data: out (or back in, in local loopback); the buffer is full
// for a character's time
void GsScc::writeData(int p, uint8_t v) {
    Channel& c = m_ch[p];
    if (c.reg[14] & 0x10) receive(p, v);
    else if (c.txEmpty) transmit(p, v);
    c.txEmpty = false;
    if (c.txLeft < 0) c.txLeft = c.txCycles;
}

void GsScc::transmit(int p, uint8_t v) {
    Channel& c = m_ch[p];
    if ((c.reg[5] & 0x60) != 0x60) v &= 0x7F;           // under 8 bits a character
    ++c.sent;
    if (c.txHook) c.txHook(v);
    if (c.link == Link::Loopback) receive(p, v);
    else if (c.link == Link::Tcp && c.clientFd >= 0) {
        if (::send(c.clientFd, &v, 1, MSG_NOSIGNAL) < 0 && errno != EAGAIN) { ::close(c.clientFd); c.clientFd = -1; }
    }
}

// scc_add_to_readbuf
void GsScc::receive(int p, uint8_t v) {
    Channel& c = m_ch[p];
    if ((c.reg[5] & 0x60) != 0x60) v &= 0x7F;
    if (c.inBuf.size() < 4096) c.inBuf.push_back(v);
    ++c.received;
    rxEvent(p);
    rxInt(p);
}

// scc_maybe_rx_event: the next character into the FIFO, one a character time
void GsScc::rxEvent(int p) {
    Channel& c = m_ch[p];
    if (c.rxLeft >= 0 || c.inBuf.empty() || c.rxQueue.size() >= 3) return;
    c.rxQueue.push_back(c.inBuf.front());
    c.inBuf.pop_front();
    rxInt(p);
    c.rxLeft = c.rxCycles;
}

// scc_maybe_rx_int: WR1 Rx interrupt modes 1 (first) and 2 (all)
void GsScc::rxInt(int p) {
    Channel& c = m_ch[p];
    if (c.rxQueue.empty()) { c.wantRx = false; return; }
    const int mode = (int)((c.reg[1] >> 3) & 3);
    if (mode == 1 || mode == 2) c.wantRx = true;
}

// scc_handle_tx_event: the buffer is empty again
void GsScc::txEvent(int p) {
    Channel& c = m_ch[p];
    c.txEmpty = true;
    if (c.reg[1] & 0x02) c.wantTx = true;
}

// scc_maybe_br_event: the baud rate generator's zero count, with ext/status
// interrupts on
void GsScc::maybeBrEvent(int p) {
    Channel& c = m_ch[p];
    if (!(c.reg[14] & 1) || c.brLeft >= 0) return;
    if (!(c.reg[15] & 0x02) || !mie()) return;
    c.brLeft = std::max(c.brCycles, 1.0);
}

void GsScc::tick(uint64_t cycles) {
    const double dt = (double)cycles;
    for (int p = 0; p < 2; ++p) {
        Channel& c = m_ch[p];
        if (c.txLeft >= 0 && (c.txLeft -= dt) < 0) { c.txLeft = -1; txEvent(p); }
        if (c.rxLeft >= 0 && (c.rxLeft -= dt) < 0) { c.rxLeft = -1; rxEvent(p); rxInt(p); }
        if (c.brLeft >= 0 && (c.brLeft -= dt) < 0) {
            c.brLeft = -1;
            if (c.reg[15] & 0x02) c.wantExt = true;
            maybeBrEvent(p);
        }
    }
    if (m_pollCountdown > cycles) { m_pollCountdown -= cycles; return; }
    m_pollCountdown = 2000;
    for (int p = 0; p < 2; ++p) if (m_ch[p].link == Link::Tcp) pollTcp(p);
}

// The host side: "none", "loopback", "tcp:PORT" (a listener on 127.0.0.1);
// port 1 first, then port 2
bool GsScc::attachSerial(const std::string& spec) {
    if (spec == "none" || spec.empty()) {
        for (int p = 0; p < 2; ++p) { closeTcp(p); m_ch[p].link = Link::None; }
        return true;
    }
    int p = m_ch[0].link == Link::None ? 0 : m_ch[1].link == Link::None ? 1 : -1;
    if (p < 0) return false;
    Channel& c = m_ch[p];
    if (spec == "loopback") { c.link = Link::Loopback; return true; }
    if (spec.rfind("tcp:", 0) != 0) return false;
    const int port = std::atoi(spec.c_str() + 4);
    if (port <= 0 || port > 65535) return false;
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(fd, (sockaddr*)&a, sizeof a) < 0 || ::listen(fd, 1) < 0) { ::close(fd); return false; }
    ::fcntl(fd, F_SETFL, O_NONBLOCK);
    c.listenFd = fd;
    c.link = Link::Tcp;
    return true;
}

void GsScc::pollTcp(int p) {
    Channel& c = m_ch[p];
    if (c.clientFd < 0 && c.listenFd >= 0) {
        const int fd = ::accept(c.listenFd, nullptr, nullptr);
        if (fd >= 0) { ::fcntl(fd, F_SETFL, O_NONBLOCK); c.clientFd = fd; }
    }
    if (c.clientFd < 0 || c.inBuf.size() > 256) return;     // the far end waits
    uint8_t buf[64];
    const ssize_t n = ::recv(c.clientFd, buf, sizeof buf, 0);
    if (n > 0) for (ssize_t i = 0; i < n; ++i) receive(p, buf[i]);
    else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) { ::close(c.clientFd); c.clientFd = -1; }
}

void GsScc::closeTcp(int p) {
    Channel& c = m_ch[p];
    if (c.clientFd >= 0) ::close(c.clientFd);
    if (c.listenFd >= 0) ::close(c.listenFd);
    c.clientFd = c.listenFd = -1;
}

void GsScc::getDeviceInfo(DeviceInfo& out) const {
    out.name = "SCC";
    char buf[96];
    for (int p = 0; p < 2; ++p) {
        const Channel& c = m_ch[p];
        const double baud = c.txCycles > 0 ? m_slowHz / c.txCycles * 10.0 : 0;
        const char* link = c.link == Link::Tcp ? (c.clientFd >= 0 ? "tcp (connected)" : "tcp (listening)")
                         : c.link == Link::Loopback ? "loopback" : "none";
        std::snprintf(buf, sizeof buf, "~%.0f baud, %s, sent %llu, received %llu", baud, link,
                      (unsigned long long)c.sent, (unsigned long long)c.received);
        out.state.push_back({p == 0 ? "Port 1 (A)" : "Port 2 (B)", buf});
    }
}

}  // namespace apple2
