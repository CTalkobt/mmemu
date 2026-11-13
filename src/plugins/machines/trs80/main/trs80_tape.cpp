// The TRS-80 cassette. The .cas pulse shapes, the 1034 us pause after the
// $A5 sync byte and the recording decoder follow xtrs's trs_cassette.c (Tim
// Mann; MIT-style licence); mmsim's own code.

#include "trs80_tape.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace trs80 {

namespace {

bool readFile(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

std::string lowerExt(const std::string& path) {
    const auto dot = path.rfind('.');
    std::string e = dot == std::string::npos ? "" : path.substr(dot + 1);
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return e;
}

uint32_t le32(const std::vector<uint8_t>& b, size_t at) {
    return (uint32_t)(b[at] | b[at + 1] << 8 | b[at + 2] << 16 | (uint32_t)b[at + 3] << 24);
}
uint16_t le16(const std::vector<uint8_t>& b, size_t at) { return (uint16_t)(b[at] | b[at + 1] << 8); }
void put32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back((uint8_t)(x >> (8 * i))); }
void put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back((uint8_t)x); v.push_back((uint8_t)(x >> 8)); }

// A bit's pulses: (microseconds from the last transition, the level then).
// A bit starts with a rise out of the middle (or, at 1500 baud, to high).
using Pulses = std::vector<std::pair<double, int>>;
const Pulses LOW_ZERO = {{0, 1}, {128, 2}, {128, 0}, {1871, 0}};
const Pulses LOW_ONE = {{0, 1}, {128, 2}, {128, 0}, {748, 1}, {128, 2}, {128, 0}, {860, 0}};
const Pulses HIGH_ZERO = {{0, 1}, {376, 2}, {376, 1}};
const Pulses HIGH_ONE = {{0, 1}, {188, 2}, {188, 1}};
const Pulses L1_ZERO = {{0, 1}, {125, 2}, {125, 0}, {3568, 0}};
const Pulses L1_ONE = {{0, 1}, {128, 2}, {128, 0}, {1673, 1}, {128, 2}, {128, 0}, {1673, 0}};

// Recording: microseconds between a clock and a data pulse (or the next clock)
constexpr double THRESH_500 = 1250.0;
constexpr double THRESH_1500 = 282.0;
constexpr double THRESH_250 = 2500.0;

// The output's levels in a .wav (0.46 V, 0.85 V, 0 V)
const uint8_t SAMPLE_OF[4] = {127, 254, 0, 127};

}  // namespace

bool Tape::mountTape(const std::string& path) {
    std::vector<uint8_t> b;
    if (!readFile(path, b) || b.empty()) return false;
    if (lowerExt(path) == "wav") {
        if (!loadWav(b)) return false;
    } else {
        loadCas(b);
    }
    m_path = path;
    m_play = true;
    return true;
}

void Tape::controlTape(const std::string& op) {
    if (op == "play") m_play = true;
    else if (op == "stop") m_play = false;
    else if (op == "rewind") {
        m_casPos = m_wavPos = 0;
        m_byte = m_bit = -1;
        m_pulses.clear();
        m_pulse = 0;
        m_pending = false;
        m_nextAt = m_now;
    } else if (op == "eject") {
        m_cas.clear();
        m_wav.clear();
        m_path.clear();
        m_play = false;
        controlTape("rewind");
    }
}

void Tape::loadCas(const std::vector<uint8_t>& bytes) {
    m_isWav = false;
    m_cas = bytes;
    m_wav.clear();
    controlTape("rewind");
}

bool Tape::loadWav(const std::vector<uint8_t>& b) {
    if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) || std::memcmp(b.data() + 8, "WAVE", 4)) return false;
    int channels = 0, rate = 0, bits = 0;
    size_t dataAt = 0, dataLen = 0;
    for (size_t at = 12; at + 8 <= b.size();) {
        const uint32_t len = le32(b, at + 4);
        if (!std::memcmp(&b[at], "fmt ", 4) && at + 24 <= b.size()) {
            if (le16(b, at + 8) != 1) return false;          // PCM only
            channels = le16(b, at + 10);
            rate = (int)le32(b, at + 12);
            bits = le16(b, at + 22);
        } else if (!std::memcmp(&b[at], "data", 4)) {
            dataAt = at + 8;
            dataLen = std::min<size_t>(len, b.size() - dataAt);
        }
        at += 8 + len + (len & 1);
    }
    if (!dataAt || channels < 1 || rate <= 0 || (bits != 8 && bits != 16)) return false;
    const size_t frame = (size_t)channels * (bits / 8);
    std::vector<int> s;                         // the first channel, centred, 8 bits
    for (size_t at = dataAt; at + frame <= dataAt + dataLen; at += frame)
        s.push_back(bits == 8 ? (int)b[at] - 128 : (int16_t)le16(b, at) / 256);
    int peak = 0;
    for (int v : s) peak = std::max(peak, std::abs(v));
    const int floor = std::max(4, peak / 4);
    m_wav.clear();
    int level = 0;
    size_t run = 0;
    const double ticksPerSample = m_hz / rate;
    for (int v : s) {
        const int l = v > floor ? 1 : v < -floor ? 2 : 0;
        if (l != level) {
            m_wav.push_back({(uint32_t)std::llround(run * ticksPerSample), (uint8_t)l});
            level = l;
            run = 0;
        }
        ++run;
    }
    m_wav.push_back({(uint32_t)std::llround(run * ticksPerSample), 0});
    m_isWav = true;
    m_cas.clear();
    controlTape("rewind");
    return true;
}

bool Tape::nextTransition() {
    if (m_isWav) {
        if (m_wavPos >= m_wav.size()) return false;
        m_nextAt += m_wav[m_wavPos].ticks;
        m_next = m_wav[m_wavPos].level;
        ++m_wavPos;
        return true;
    }
    if (m_pulse >= m_pulses.size()) {
        if (m_bit < 0) {
            if (m_casPos > m_cas.size()) return false;
            // One zero byte after the end, as xtrs (some images need it)
            m_byte = m_casPos < m_cas.size() ? m_cas[m_casPos] : 0;
            ++m_casPos;
            m_bit = 7;
        }
        const Speed sp = speed ? speed() : Speed::Baud500;
        const bool one = (m_byte >> m_bit) & 1;
        m_pulses = sp == Speed::Baud1500 ? (one ? HIGH_ONE : HIGH_ZERO)
                 : sp == Speed::Baud250 ? (one ? L1_ONE : L1_ZERO) : (one ? LOW_ONE : LOW_ZERO);
        // BASIC clears its variables after the sync byte: time for it
        if (m_bit == 0 && m_byte == 0xA5 && sp == Speed::Baud500) m_pulses.back().first += 1034;
        m_pulse = 0;
        --m_bit;
    }
    m_nextAt += us(m_pulses[m_pulse].first);
    m_next = m_pulses[m_pulse].second;
    ++m_pulse;
    return true;
}

void Tape::setMotor(uint64_t now, bool on) {
    advance(now);
    if (on && !m_motor) {
        // Playback picks up at the start of the byte it was in
        if (!m_isWav && m_bit >= 0 && m_casPos > 0) --m_casPos;
        if (!m_isWav) { m_bit = -1; m_pulses.clear(); m_pulse = 0; }
        m_pending = false;
        m_nextAt = now;
        m_value = 0;
    }
    if (!on && m_motor && m_recording) m_rec.push_back({now, 0});
    m_motor = on;
}

void Tape::advance(uint64_t now) {
    if (!playing()) { m_now = now; return; }
    for (;;) {
        if (!m_pending) {
            if (!nextTransition()) break;
            m_pending = true;
        }
        if (m_nextAt > now) break;
        // A rise out of the middle sets the 500 baud flip-flop
        if (m_next != 0 && m_value == 0) m_flipflop = true;
        if (onEdge && m_next != m_value) {
            if (m_next == 2 && m_lastNonzero != 2) onEdge(false);
            else if (m_next == 1 && m_lastNonzero != 1) onEdge(true);
        }
        m_value = m_next;
        if (m_value) m_lastNonzero = m_value;
        m_pending = false;
    }
    m_now = now;
}

void Tape::output(uint64_t now, int level) {
    level &= 3;
    if (level == 3) level = 0;
    if (m_recording && m_motor && level != m_outLevel) m_rec.push_back({now, level});
    m_outLevel = level;
}

bool Tape::startTapeRecord() {
    m_recording = true;
    m_rec.clear();
    m_play = false;
    return true;
}

void Tape::stopTapeRecord() { m_recording = false; }

bool Tape::saveTapeRecording(const std::string& path) {
    const std::vector<uint8_t> data = lowerExt(path) == "wav" ? recordedWav() : recordedCas();
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write((const char*)data.data(), (std::streamsize)data.size());
    return (bool)f;
}

std::vector<uint8_t> Tape::recordedCas() const {
    enum { INITIAL, GOTCLK500, GOTDAT500, HIGH1500, L1, GOTCLK250, GOTDAT250 } st = INITIAL;
    std::vector<uint8_t> out;
    int value = 0, byte = 0, bitnum = 0;
    uint64_t last = m_rec.empty() ? 0 : m_rec.front().first, riseAt = last;
    auto putBit = [&](int bit) {
        if (--bitnum < 0) bitnum = 7;
        byte |= bit << bitnum;
        if (bitnum == 0) { out.push_back((uint8_t)byte); byte = 0; }
    };
    for (const auto& [t, v] : m_rec) {
        if (v == value) continue;
        const double d = (double)(t - last) * 1e6 / m_hz;
        int bit = -1;
        switch (st) {
        case INITIAL:
            if (value == 2 && v == 0) st = GOTCLK500;          // end of a clock pulse
            else if (value == 2 && v == 1) {
                // 1500 baud: the cycle just ended was the first bit (its high half's length)
                bit = (double)(last - riseAt) * 1e6 / m_hz < THRESH_1500;
                st = HIGH1500;
            }
            break;
        case GOTCLK500:
            if (value == 0 && v == 1) {
                if (d > THRESH_250) { bit = 0; st = L1; }
                else if (d > THRESH_500) { bit = 0; st = INITIAL; }
                else { bit = 1; st = GOTDAT500; }
            }
            break;
        case GOTDAT500:
            if (value == 2 && v == 0) st = INITIAL;
            break;
        case HIGH1500:
            if (value == 1 && v == 2) bit = d < THRESH_1500;
            break;
        case L1:
            if (value == 2 && v == 0) st = GOTCLK250;
            break;
        case GOTCLK250:
            if (value == 0 && v == 1) {
                if (d > THRESH_250) { bit = 0; st = L1; }
                else { bit = 1; st = GOTDAT250; }
            }
            break;
        case GOTDAT250:
            if (value == 2 && v == 0) st = L1;
            break;
        }
        if (bit >= 0) putBit(bit);
        if (v == 1) riseAt = t;
        value = v;
        last = t;
    }
    if (bitnum != 0) out.push_back((uint8_t)byte);
    return out;
}

std::vector<uint8_t> Tape::recordedWav() const {
    constexpr int RATE = 22050;
    std::vector<uint8_t> samples;
    if (!m_rec.empty()) {
        const uint64_t t0 = m_rec.front().first;
        int level = 0;
        size_t done = 0;
        for (const auto& [t, v] : m_rec) {
            const size_t upto = (size_t)((double)(t - t0) * RATE / m_hz);
            if (upto > done) samples.insert(samples.end(), upto - done, SAMPLE_OF[level & 3]);
            done = std::max(done, upto);
            level = v;
        }
        samples.insert(samples.end(), RATE / 10, SAMPLE_OF[0]);
    }
    std::vector<uint8_t> w = {'R', 'I', 'F', 'F'};
    put32(w, (uint32_t)(36 + samples.size()));
    w.insert(w.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    put32(w, 16);
    put16(w, 1);
    put16(w, 1);
    put32(w, RATE);
    put32(w, RATE);
    put16(w, 1);
    put16(w, 8);
    w.insert(w.end(), {'d', 'a', 't', 'a'});
    put32(w, (uint32_t)samples.size());
    w.insert(w.end(), samples.begin(), samples.end());
    return w;
}

}  // namespace trs80
