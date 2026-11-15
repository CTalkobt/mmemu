// MC14412 modem and its phone line (see mc14412.h). mmsim's own code.

#include "mc14412.h"
#include <cmath>
#include <cstring>

void Mc14412::setLine(std::shared_ptr<SerialEndpoint> line) {
    m_line = std::move(line);
    m_spec = m_line ? m_line->describe() : "none";
    m_carrier = false;
    m_cdWait = 0;
}

bool Mc14412::attachLine(const std::string& spec) {
    auto ep = makeSerialEndpoint(spec);
    if (!ep) return false;
    setLine(std::shared_ptr<SerialEndpoint>(std::move(ep)));
    return true;
}

void Mc14412::setEnable(bool on) {
    m_enable = on;
}

void Mc14412::setOffHook(bool off) {
    if (off != m_offHook) hook(off);
}

void Mc14412::hook(bool off) {
    if (off) {
        const double brk = (double)(m_now - m_hookChange) / m_hz;
        if (m_inBreak && brk < PULSE_MAX_BREAK) {
            ++m_pulses;                     // a dial pulse's break
        } else {
            m_pulses = 0;                   // picked up after hanging up: a new call
            m_dialled.clear();
        }
    } else {
        m_carrier = false;
        m_cdWait = 0;
    }
    m_inBreak = !off;                       // a break follows a make (not the line idle since power-on)
    m_offHook = off;
    m_hookChange = m_now;
}

bool Mc14412::lineSignal() const {
    if (!m_carrier) return false;
    const double cycles = (double)m_now / m_hz * toneHz();
    return cycles - std::floor(cycles) < 0.5;
}

bool Mc14412::ring() const {
    if (m_offHook || !farEnd()) return false;
    const double t = (double)(m_now - m_ringStart) / m_hz;
    return std::fmod(t, 6.0) < 2.0;
}

void Mc14412::tick(uint64_t clocks) {
    m_now += clocks;
    const bool far = farEnd();
    if (far && !m_farPrev) m_ringStart = m_now;
    m_farPrev = far;
    // A digit ends when the line stays off hook long enough after its pulses
    if (m_offHook && m_pulses > 0 && (double)(m_now - m_hookChange) / m_hz > DIGIT_GAP) {
        m_dialled += (char)('0' + m_pulses % 10);
        m_pulses = 0;
    }
    if (m_offHook && far) {
        m_cdWait += clocks;
        if ((double)m_cdWait / m_hz >= CARRIER_DELAY) m_carrier = true;
    } else {
        m_cdWait = 0;
        m_carrier = false;
    }
}

void Mc14412::send(uint8_t b) {
    if (!connected()) return;
    m_line->send(b);
    sentToLine.push_back(b);
}

bool Mc14412::receive(uint8_t& b) {
    if (!m_line) return false;
    if (!connected()) {
        // The line is silent to the UART: nothing arrives (what the far end
        // sends before the connection is lost, as tones nobody demodulates)
        uint8_t drop;
        if (!m_offHook) return false;       // on hook: leave it queued for the call
        while (m_line->receive(drop)) {}
        return false;
    }
    return m_line->receive(b);
}

void Mc14412::poll() {
    if (m_line) m_line->poll();
}

std::string Mc14412::describe() const {
    std::string s = "modem:" + (m_line ? m_line->describe() : std::string("none"));
    s += m_enable ? " [on" : " [off";
    s += m_offHook ? ", off hook" : ", on hook";
    s += m_originate ? ", originate" : ", answer";
    if (m_carrier) s += ", carrier";
    if (!m_dialled.empty()) s += ", dialled " + m_dialled;
    return s + "]";
}

std::vector<uint8_t> Mc14412::saveState() const {
    std::vector<uint8_t> v;
    auto put = [&v](const void* p, size_t n) {
        v.insert(v.end(), (const uint8_t*)p, (const uint8_t*)p + n);
    };
    const uint8_t flags = (uint8_t)(m_enable | m_offHook << 1 | m_originate << 2 | m_carrier << 3);
    put(&flags, 1);
    put(&m_now, 8); put(&m_cdWait, 8); put(&m_hookChange, 8); put(&m_ringStart, 8); put(&m_pulses, 4);
    return v;
}

void Mc14412::loadState(const std::vector<uint8_t>& s) {
    if (s.size() < 1 + 8 * 4 + 4) return;
    const uint8_t* p = s.data();
    const uint8_t f = *p++;
    m_enable = f & 1; m_offHook = f & 2; m_originate = f & 4; m_carrier = f & 8;
    std::memcpy(&m_now, p, 8); p += 8;
    std::memcpy(&m_cdWait, p, 8); p += 8;
    std::memcpy(&m_hookChange, p, 8); p += 8;
    std::memcpy(&m_ringStart, p, 8); p += 8;
    std::memcpy(&m_pulses, p, 4);
}
