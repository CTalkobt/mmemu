// IM6402 CMOS UART, after the Intersil/Harris data sheet; MAME's im6402.cpp
// consulted. mmsim's own code.

#include "im6402.h"
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

Im6402::Im6402() = default;
Im6402::~Im6402() = default;

void Im6402::masterReset() {
    m_rbr = 0;
    m_oe = m_fe = m_pe = false;
    m_rxBusy = false;
    m_rxLeft = 0;
    m_txPos = 0;
    setDr(false);
    setTbre(true);
    setTre(true);
}

int Im6402::stopHalfBits() const {
    if (!(m_control & CTRL_SBS)) return 2;
    return wordLength() == 5 ? 3 : 4;
}

int Im6402::frameClocks() const {
    return (1 + wordLength() + (parityEnabled() ? 1 : 0)) * 16 + stopHalfBits() * 8;
}

uint8_t Im6402::status() const {
    return (uint8_t)((m_dr ? ST_DR : 0) | (m_oe ? ST_OE : 0) | (m_fe ? ST_FE : 0) | (m_pe ? ST_PE : 0) |
                     (m_tbre ? ST_TBRE : 0) | (m_tre ? ST_TRE : 0));
}

void Im6402::setDr(bool v) {
    if (m_dr == v) return;
    m_dr = v;
    if (onDr) onDr(v);
}

void Im6402::setTbre(bool v) {
    if (m_tbre == v) return;
    m_tbre = v;
    if (onTbre) onTbre(v);
}

void Im6402::setTre(bool v) {
    if (m_tre == v) return;
    m_tre = v;
    if (onTre) onTre(v);
}

void Im6402::dataReceivedReset() { setDr(false); }

void Im6402::startTx() {
    m_tsr = m_tbr;
    m_txFrame = frameClocks();
    m_txPos = 0;
    setTbre(true);
    setTre(false);
}

void Im6402::writeTbr(uint8_t v) {
    m_tbr = v;
    setTbre(false);
    if (m_tre) startTx();
}

bool Im6402::tro() const {
    if (m_tre) return true;
    const int bit = (int)(m_txPos / 16);
    const int wl = wordLength();
    if (bit == 0) return false;                              // start
    if (bit <= wl) return (m_tsr >> (bit - 1)) & 1;          // data, LSB first
    if (parityEnabled() && bit == wl + 1) {
        int ones = 0;
        for (int i = 0; i < wl; ++i) ones += (m_tsr >> i) & 1;
        return evenParity() ? (ones & 1) : !(ones & 1);
    }
    return true;                                             // stop
}

void Im6402::tickTx(uint64_t clocks) {
    while (clocks && !m_tre) {
        const uint64_t left = (uint64_t)m_txFrame - m_txPos;
        if (clocks < left) {
            m_txPos += clocks;
            return;
        }
        clocks -= left;
        const uint8_t b = (uint8_t)(m_tsr & ((1u << wordLength()) - 1));
        sent.push_back(b);
        if (m_endpoint) m_endpoint->send(b);
        if (!m_tbre) startTx();
        else {
            m_txPos = 0;
            setTre(true);
        }
    }
}

void Im6402::receive(uint8_t b, bool parityError, bool framingError) {
    m_injected.push_back({b, parityError, framingError});
}

void Im6402::tickRx(uint64_t clocks) {
    // Poll the link (a socket read) every 256 clocks, not on every call
    m_pollAcc += clocks;
    if (m_endpoint && m_pollAcc >= 256) {
        m_pollAcc = 0;
        m_endpoint->poll();
    }
    const uint64_t arrive = (uint64_t)(1 + wordLength() + (parityEnabled() ? 1 : 0)) * 16 + 8;
    const uint64_t tail = (uint64_t)frameClocks() - arrive;   // the rest of the stop bits
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
        }
        const uint64_t step = clocks < m_rxLeft ? clocks : m_rxLeft;
        const bool crossesArrival = m_rxLeft > tail && m_rxLeft - step <= tail;
        m_rxLeft -= step;
        clocks -= step;
        if (crossesArrival) {
            m_rbr = (uint8_t)(m_rxChar.b & ((1u << wordLength()) - 1));
            m_pe = parityEnabled() && m_rxChar.pe;
            m_fe = m_rxChar.fe;
            m_oe = m_dr;
            setDr(true);
        }
        if (!m_rxLeft) m_rxBusy = false;
    }
}

bool Im6402::attachSerial(const std::string& spec) {
    auto ep = makeSerialEndpoint(spec);
    if (!ep) return false;
    m_endpoint = std::shared_ptr<SerialEndpoint>(std::move(ep));
    return true;
}

std::vector<uint8_t> Im6402::saveState() const {
    std::vector<uint8_t> v;
    put(v, m_pins); put(v, m_control); put(v, m_rbr); put(v, m_tbr); put(v, m_tsr);
    put(v, (uint8_t)((m_dr ? 1 : 0) | (m_oe ? 2 : 0) | (m_fe ? 4 : 0) | (m_pe ? 8 : 0) | (m_tbre ? 16 : 0) |
                     (m_tre ? 32 : 0) | (m_rxBusy ? 64 : 0)));
    put(v, (uint16_t)m_txFrame); put(v, m_txPos);
    put(v, m_rxChar.b); put(v, (uint8_t)((m_rxChar.pe ? 1 : 0) | (m_rxChar.fe ? 2 : 0))); put(v, m_rxLeft);
    return v;
}

void Im6402::loadState(const std::vector<uint8_t>& s) {
    const uint8_t* p = s.data();
    const uint8_t* e = p + s.size();
    uint8_t f = 0, rf = 0;
    uint16_t frame = 0;
    get(p, e, m_pins); get(p, e, m_control); get(p, e, m_rbr); get(p, e, m_tbr); get(p, e, m_tsr);
    get(p, e, f);
    get(p, e, frame); get(p, e, m_txPos);
    get(p, e, m_rxChar.b); get(p, e, rf); get(p, e, m_rxLeft);
    m_dr = f & 1; m_oe = f & 2; m_fe = f & 4; m_pe = f & 8; m_tbre = f & 16; m_tre = f & 32; m_rxBusy = f & 64;
    m_txFrame = frame;
    m_rxChar.pe = rf & 1;
    m_rxChar.fe = rf & 2;
    if (onDr) onDr(m_dr);
}
