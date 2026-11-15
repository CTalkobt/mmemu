// Intel 8155 / Oki MSM81C55 RAM-I/O-timer, after the Intel and Oki data
// sheets; MAME's i8155.cpp consulted for the timer readback. mmsim's own code.

#include "i8155.h"
#include <algorithm>

namespace {
template <class T> void put(std::vector<uint8_t>& v, T x) {
    for (size_t i = 0; i < sizeof(T); ++i) v.push_back((uint8_t)((uint64_t)x >> (8 * i)));
}
template <class T> void get(const uint8_t*& p, const uint8_t* e, T& x) {
    uint64_t r = 0;
    for (size_t i = 0; i < sizeof(T) && p < e; ++i) r |= (uint64_t)*p++ << (8 * i);
    x = (T)r;
}
constexpr uint8_t CMD_TIMER = 0xC0;

}  // namespace

void I8155::reset() {
    m_command = 0;
    m_out = {0, 0, 0};
    for (auto& s : m_strobe) s = Strobe{};
    m_tcFlag = false;
    stopTimer();
    emitPc();
}

I8155::PcMode I8155::pcMode() const {
    switch (m_command & 0x0C) {
    case 0x00: return PcMode::Alt1;
    case 0x0C: return PcMode::Alt2;
    case 0x04: return PcMode::Alt3;
    default: return PcMode::Alt4;
    }
}

bool I8155::strobed(int port) const {
    const PcMode m = pcMode();
    return port == 0 ? (m == PcMode::Alt3 || m == PcMode::Alt4) : (port == 1 && m == PcMode::Alt4);
}

uint8_t I8155::inputPins(int port) const {
    const auto& f = port == 0 ? inPa : port == 1 ? inPb : inPc;
    return f ? f() : 0xFF;
}

uint8_t I8155::pcPins() const {
    const Strobe& a = m_strobe[0];
    const Strobe& b = m_strobe[1];
    const uint8_t aBits = (uint8_t)((a.intr ? 1 : 0) | (a.bf ? 2 : 0) | (a.stb ? 4 : 0));
    switch (pcMode()) {
    case PcMode::Alt1: return inputPins(2) & 0x3F;
    case PcMode::Alt2: return m_out[2] & 0x3F;
    case PcMode::Alt3: return (uint8_t)(aBits | (m_out[2] & 0x38));
    default: return (uint8_t)(aBits | (b.intr ? 8 : 0) | (b.bf ? 0x10 : 0) | (b.stb ? 0x20 : 0));
    }
}

void I8155::emitPc() {
    if (pcMode() != PcMode::Alt1 && outPc) outPc(pcPins());
}

uint8_t I8155::status() const {
    const Strobe& a = m_strobe[0];
    const Strobe& b = m_strobe[1];
    return (uint8_t)((a.intr ? 0x01 : 0) | (a.bf ? 0x02 : 0) | ((m_command & 0x10) ? 0x04 : 0) | (b.intr ? 0x08 : 0) |
                     (b.bf ? 0x10 : 0) | ((m_command & 0x20) ? 0x20 : 0) | (m_tcFlag ? 0x40 : 0));
}

uint8_t I8155::readPortRaw(int port) const {
    const bool output = port == 0 ? portAOutput() : portBOutput();
    if (output) return m_out[port];
    if (strobed(port)) return m_strobe[port].latched;
    return inputPins(port);
}

uint16_t I8155::timerReadback() const {
    if (!m_running) return m_countReg & 0x3FFF;
    const uint32_t h1 = (m_n + 1) / 2;
    const uint32_t cap = m_n & 0x3FFE;
    if (m_pos < h1) return (uint16_t)(std::min<uint32_t>(2 * (h1 - m_pos), cap) | 1);
    return (uint16_t)std::min<uint32_t>(2 * (m_n - m_pos), cap);
}

uint8_t I8155::peekIo(uint8_t reg) const {
    switch (reg & 7) {
    case 0: return status();
    case 1: return readPortRaw(0);
    case 2: return readPortRaw(1);
    case 3: return (uint8_t)(pcPins() | 0xC0);
    case 4: return (uint8_t)timerReadback();
    case 5: {
        const uint8_t mode = m_running ? m_mode : (uint8_t)(m_countReg >> 14);
        return (uint8_t)(((timerReadback() >> 8) & 0x3F) | (mode << 6));
    }
    default: return 0xFF;
    }
}

uint8_t I8155::readIo(uint8_t reg) {
    const uint8_t v = peekIo(reg);
    switch (reg & 7) {
    case 0: m_tcFlag = false; break;
    case 1:
    case 2: {
        const int port = (reg & 7) - 1;
        const bool output = port == 0 ? portAOutput() : portBOutput();
        if (strobed(port) && !output) {
            m_strobe[port].bf = false;
            setIntr(port, false);
            emitPc();
        }
        break;
    }
    default: break;
    }
    return v;
}

void I8155::writeIo(uint8_t reg, uint8_t v) {
    switch (reg & 7) {
    case 0: writeCommand(v); break;
    case 1: writePort(0, v); break;
    case 2: writePort(1, v); break;
    case 3:
        m_out[2] = v & 0x3F;
        emitPc();
        break;
    case 4: m_countReg = (uint16_t)((m_countReg & 0xFF00) | v); break;
    case 5: m_countReg = (uint16_t)((v << 8) | (m_countReg & 0x00FF)); break;
    default: break;
    }
}

void I8155::writePort(int port, uint8_t v) {
    m_out[port] = v;
    const bool output = port == 0 ? portAOutput() : portBOutput();
    if (!output) return;
    if (strobed(port)) {
        m_strobe[port].bf = true;
        setIntr(port, false);
        emitPc();
    }
    const auto& f = port == 0 ? outPa : outPb;
    if (f) f(v);
}

void I8155::writeCommand(uint8_t v) {
    const uint8_t old = m_command;
    m_command = v;
    if ((v & 0x01) && !(old & 0x01) && outPa) outPa(m_out[0]);
    if ((v & 0x02) && !(old & 0x02) && outPb) outPb(m_out[1]);
    if ((v & 0x0C) != (old & 0x0C)) emitPc();
    switch (v & CMD_TIMER) {
    case 0x40: stopTimer(); break;
    case 0x80:
        if (m_running) m_stopAtTc = true;
        break;
    case 0xC0:
        if (m_running) {
            m_reloadPending = true;
            m_stopAtTc = false;
        } else {
            loadTimer();
        }
        break;
    default: break;
    }
}

void I8155::setIntr(int port, bool v) {
    Strobe& s = m_strobe[port];
    if (s.intr == v) return;
    s.intr = v;
    const auto& f = port == 0 ? onIntrA : onIntrB;
    if (f) f(v);
}

void I8155::setStrobe(int port, bool level) {
    Strobe& s = m_strobe[port];
    if (s.stb == level) return;
    s.stb = level;
    if (!strobed(port)) return;
    const bool output = port == 0 ? portAOutput() : portBOutput();
    const bool inte = m_command & (port == 0 ? 0x10 : 0x20);
    if (!level) {
        // Falling: an input latches its pins; an output's data has been taken
        if (output) s.bf = false;
        else {
            s.latched = inputPins(port);
            s.bf = true;
        }
    } else if (inte) {
        setIntr(port, true);
    }
    emitPc();
}

void I8155::setStrobeA(bool level) { setStrobe(0, level); }
void I8155::setStrobeB(bool level) { setStrobe(1, level); }

void I8155::setTo(bool level) {
    if (level == m_to) return;
    m_to = level;
    if (level) ++m_edges;
    if (onTimerOut) onTimerOut(level);
}

bool I8155::toForPos(uint32_t pos) const {
    if (m_mode & 2) return pos != (uint32_t)(m_n - 1);     // pulse at TC
    return pos < (uint32_t)((m_n + 1) / 2);                // square wave, longer half first
}

void I8155::loadTimer() {
    m_n = m_countReg & 0x3FFF;
    m_mode = (uint8_t)(m_countReg >> 14);
    m_reloadPending = m_stopAtTc = false;
    if (m_n < 2) {
        stopTimer();
        return;
    }
    m_pos = 0;
    m_running = true;
    setTo(toForPos(0));
}

void I8155::stopTimer() {
    m_running = false;
    m_reloadPending = m_stopAtTc = false;
    setTo(true);
}

uint64_t I8155::tick(uint64_t clocks) {
    m_edges = 0;
    while (clocks && m_running) {
        // The next point where TIMER OUT may change: the half, the TC clock, the end
        uint32_t next = m_n;
        if (m_mode & 2) {
            if (m_pos < (uint32_t)(m_n - 1)) next = m_n - 1;
        } else if (m_pos < (uint32_t)((m_n + 1) / 2)) {
            next = (m_n + 1) / 2;
        }
        const uint64_t step = std::min<uint64_t>(clocks, next - m_pos);
        m_pos += (uint32_t)step;
        clocks -= step;
        if (m_pos >= m_n) {
            m_tcFlag = true;
            if (m_reloadPending) {
                loadTimer();
                continue;
            }
            if (!(m_mode & 1) || m_stopAtTc) {
                stopTimer();
                break;
            }
            m_pos = 0;
        }
        setTo(toForPos(m_pos));
    }
    return m_edges;
}

std::vector<uint8_t> I8155::saveState() const {
    std::vector<uint8_t> v(m_ram.begin(), m_ram.end());
    put(v, m_command);
    for (uint8_t o : m_out) put(v, o);
    for (const Strobe& s : m_strobe) {
        put(v, (uint8_t)((s.stb ? 1 : 0) | (s.bf ? 2 : 0) | (s.intr ? 4 : 0)));
        put(v, s.latched);
    }
    put(v, m_countReg); put(v, m_n); put(v, m_mode); put(v, m_pos);
    put(v, (uint8_t)((m_running ? 1 : 0) | (m_to ? 2 : 0) | (m_tcFlag ? 4 : 0) | (m_reloadPending ? 8 : 0) |
                     (m_stopAtTc ? 16 : 0)));
    return v;
}

void I8155::loadState(const std::vector<uint8_t>& s) {
    if (s.size() < m_ram.size()) return;
    std::copy(s.begin(), s.begin() + m_ram.size(), m_ram.begin());
    const uint8_t* p = s.data() + m_ram.size();
    const uint8_t* e = s.data() + s.size();
    get(p, e, m_command);
    for (uint8_t& o : m_out) get(p, e, o);
    for (Strobe& st : m_strobe) {
        uint8_t f = 0;
        get(p, e, f);
        get(p, e, st.latched);
        st.stb = f & 1; st.bf = f & 2; st.intr = f & 4;
    }
    uint8_t f = 0;
    get(p, e, m_countReg); get(p, e, m_n); get(p, e, m_mode); get(p, e, m_pos);
    get(p, e, f);
    m_running = f & 1; m_to = f & 2; m_tcFlag = f & 4; m_reloadPending = f & 8; m_stopAtTc = f & 16;
    if (portAOutput() && outPa) outPa(m_out[0]);
    if (portBOutput() && outPb) outPb(m_out[1]);
    emitPc();
    if (onTimerOut) onTimerOut(m_to);
}
