// Intel 8251A USART, asynchronous mode, after the Intel 8251A data sheet
// (the mode and command instructions, the status read); MAME's i8251.cpp
// consulted for the reset sequence. mmsim's own code.

#include "i8251.h"
#include "plugins/devices/serial_endpoint/main/serial_endpoint.h"

namespace {
template <class T> void put(std::vector<uint8_t>& v, T x) {
    for (size_t i = 0; i < sizeof(T); ++i) v.push_back((uint8_t)((uint64_t)x >> (8 * i)));
}
template <class T> void get(const uint8_t*& p, const uint8_t* e, T& x) {
    uint64_t r = 0;
    for (size_t i = 0; i < sizeof(T) && p < e; ++i) r |= (uint64_t)*p++ << (8 * i);
    x = (T)r;
}
}  // namespace

I8251::I8251() { reset(); }
I8251::~I8251() = default;

void I8251::reset() {
    m_expectMode = true;
    m_command = 0;
    m_rbr = 0;
    m_rxRdy = m_pe = m_oe = m_fe = false;
    m_tbrEmpty = true;
    m_txBusy = false;
    m_txLeft = 0;
    m_rxBusy = false;
    m_rxLeft = m_rxTail = 0;
    m_injected.clear();
    updateRxRdyPin();
    updateTxRdyPin();
}

int I8251::factor() const {
    switch (m_mode & 3) {
    case 1: return 1;
    case 2: return 16;
    case 3: return 64;
    default: return 1;      // synchronous: a bit per clock
    }
}

int I8251::stopHalfBits() const {
    switch ((m_mode >> 6) & 3) {
    case 2: return 3;
    case 3: return 4;
    default: return 2;      // 01 one (00 is invalid in async mode: taken as one)
    }
}

int I8251::frameClocks() const {
    const int f = factor();
    return (1 + wordLength() + (parityEnabled() ? 1 : 0)) * f + stopHalfBits() * f / 2;
}

uint8_t I8251::status() const {
    uint8_t s = 0;
    if (m_tbrEmpty) s |= ST_TXRDY;
    if (m_rxRdy) s |= ST_RXRDY;
    if (txEmpty()) s |= ST_TXEMPTY;
    if (m_pe) s |= ST_PE;
    if (m_oe) s |= ST_OE;
    if (m_fe) s |= ST_FE;
    if (m_endpoint && m_endpoint->dsr()) s |= ST_DSR;
    return s;
}

void I8251::updateRxRdyPin() {
    const bool v = rxRdy();
    if (v == m_rxPin) return;
    m_rxPin = v;
    if (onRxRdy) onRxRdy(v);
}

void I8251::updateTxRdyPin() {
    const bool v = m_tbrEmpty && (m_command & CMD_TXEN);
    if (v == m_txPin) return;
    m_txPin = v;
    if (onTxRdy) onTxRdy(v);
}

uint8_t I8251::read(int offset) { return (offset & 1) ? status() : readData(); }

void I8251::write(int offset, uint8_t v) {
    if (offset & 1) writeControl(v);
    else writeData(v);
}

void I8251::writeControl(uint8_t v) {
    if (m_expectMode) {
        // Synchronous modes are followed by one or two sync characters; they
        // are taken here as commands would be (sync mode is not run)
        m_mode = v;
        m_expectMode = false;
        return;
    }
    if (v & CMD_IR) {
        reset();
        return;
    }
    m_command = (uint8_t)(v & ~(CMD_IR | CMD_ER));
    if (v & CMD_ER) m_pe = m_oe = m_fe = false;
    if (m_endpoint) {
        m_endpoint->setDtr(v & CMD_DTR);
        m_endpoint->setRts(v & CMD_RTS);
    }
    if ((m_command & CMD_TXEN) && !m_tbrEmpty && !m_txBusy) startTx();
    updateRxRdyPin();
    updateTxRdyPin();
}

void I8251::writeData(uint8_t v) {
    m_tbr = v;
    m_tbrEmpty = false;
    if ((m_command & CMD_TXEN) && !m_txBusy) startTx();
    updateTxRdyPin();
}

uint8_t I8251::readData() {
    m_rxRdy = false;
    updateRxRdyPin();
    return m_rbr;
}

void I8251::startTx() {
    m_tsr = m_tbr;
    m_tbrEmpty = true;
    m_txBusy = true;
    m_txLeft = (uint64_t)frameClocks();
}

void I8251::tickTx(uint64_t clocks) {
    while (clocks && m_txBusy) {
        if (clocks < m_txLeft) {
            m_txLeft -= clocks;
            return;
        }
        clocks -= m_txLeft;
        m_txLeft = 0;
        const uint8_t b = (uint8_t)(m_tsr & ((1u << wordLength()) - 1));
        sent.push_back(b);
        if (m_endpoint) m_endpoint->send(b);
        m_txBusy = false;
        if (!m_tbrEmpty && (m_command & CMD_TXEN)) startTx();
        updateTxRdyPin();
    }
}

void I8251::receive(uint8_t b, bool parityError, bool framingError) {
    m_injected.push_back({b, parityError, framingError});
}

void I8251::tickRx(uint64_t clocks) {
    m_pollAcc += clocks;
    if (m_endpoint && m_pollAcc >= 256) {
        m_pollAcc = 0;
        m_endpoint->poll();
    }
    const int f = factor();
    const uint64_t arrive = (uint64_t)(1 + wordLength() + (parityEnabled() ? 1 : 0)) * f + f / 2;
    const uint64_t frame = (uint64_t)frameClocks();
    const uint64_t tail = frame > arrive ? frame - arrive : 0;
    while (clocks) {
        if (!m_rxBusy) {
            Pending c{0, false, false};
            if (!m_injected.empty()) {
                c = m_injected.front();
                m_injected.erase(m_injected.begin());
            } else if (!m_endpoint || !m_endpoint->receive(c.b)) {
                return;
            }
            m_rxChar = c;
            m_rxBusy = true;
            m_rxLeft = arrive + tail;
            m_rxTail = tail;
        }
        const uint64_t step = clocks < m_rxLeft ? clocks : m_rxLeft;
        const bool crossesArrival = m_rxLeft > m_rxTail && m_rxLeft - step <= m_rxTail;
        m_rxLeft -= step;
        clocks -= step;
        if (crossesArrival && (m_command & CMD_RXE)) {
            // A disabled receiver ignores the line
            if (m_rxRdy) m_oe = true;
            m_rbr = (uint8_t)(m_rxChar.b & ((1u << wordLength()) - 1));
            if (parityEnabled() && m_rxChar.pe) m_pe = true;
            if (m_rxChar.fe) m_fe = true;
            m_rxRdy = true;
            updateRxRdyPin();
        }
        if (!m_rxLeft) m_rxBusy = false;
    }
}

void I8251::setEndpoint(std::shared_ptr<SerialEndpoint> ep) { m_endpoint = std::move(ep); }

bool I8251::attachSerial(const std::string& spec) {
    auto ep = makeSerialEndpoint(spec);
    if (!ep) return false;
    m_endpoint = std::shared_ptr<SerialEndpoint>(std::move(ep));
    return true;
}

std::vector<uint8_t> I8251::saveState() const {
    std::vector<uint8_t> v;
    put(v, m_mode); put(v, m_command); put(v, m_rbr); put(v, m_tbr); put(v, m_tsr);
    put(v, (uint8_t)((m_expectMode ? 1 : 0) | (m_rxRdy ? 2 : 0) | (m_pe ? 4 : 0) | (m_oe ? 8 : 0) |
                     (m_fe ? 16 : 0) | (m_tbrEmpty ? 32 : 0) | (m_txBusy ? 64 : 0) | (m_rxBusy ? 128 : 0)));
    put(v, m_txLeft); put(v, m_rxLeft); put(v, m_rxTail);
    put(v, m_rxChar.b); put(v, (uint8_t)((m_rxChar.pe ? 1 : 0) | (m_rxChar.fe ? 2 : 0)));
    return v;
}

void I8251::loadState(const std::vector<uint8_t>& s) {
    const uint8_t* p = s.data();
    const uint8_t* e = p + s.size();
    uint8_t f = 0, rf = 0;
    get(p, e, m_mode); get(p, e, m_command); get(p, e, m_rbr); get(p, e, m_tbr); get(p, e, m_tsr);
    get(p, e, f);
    get(p, e, m_txLeft); get(p, e, m_rxLeft); get(p, e, m_rxTail);
    get(p, e, m_rxChar.b); get(p, e, rf);
    m_expectMode = f & 1; m_rxRdy = f & 2; m_pe = f & 4; m_oe = f & 8; m_fe = f & 16;
    m_tbrEmpty = f & 32; m_txBusy = f & 64; m_rxBusy = f & 128;
    m_rxChar.pe = rf & 1;
    m_rxChar.fe = rf & 2;
    m_rxPin = !rxRdy();
    m_txPin = !(m_tbrEmpty && (m_command & CMD_TXEN));
    updateRxRdyPin();
    updateTxRdyPin();
}
