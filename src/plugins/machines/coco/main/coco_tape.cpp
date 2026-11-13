// The CoCo cassette: .cas and .wav playback through the comparator, and
// recording from the DAC.

#include "coco_tape.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>

namespace coco {

namespace {

constexpr double MASTER_HZ = 14318180.0;
constexpr uint64_t HALF_SPLIT = (Tape::TICKS_BIT0 / 2 + Tape::TICKS_BIT1 / 2) / 2;   // short vs long half-cycle
constexpr uint64_t GAP = 20000;              // a half-cycle longer than this is silence
constexpr int WAV_RATE = 22050;

std::string extOf(const std::string& path) {
    const auto dot = path.find_last_of('.');
    std::string e = dot == std::string::npos ? "" : path.substr(dot + 1);
    for (auto& c : e) c = (char)std::tolower((unsigned char)c);
    return e;
}

uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
void put32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back((uint8_t)(x >> (8 * i))); }
void put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back((uint8_t)x); v.push_back((uint8_t)(x >> 8)); }

}  // namespace

bool Tape::mountTape(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const std::string e = extOf(path);
    if (e == "wav") {
        if (!loadWav(bytes)) return false;
    } else if (e == "cas") {
        loadCas(bytes);
    } else {
        return false;
    }
    m_path = path;
    m_play = true;
    return true;
}

void Tape::controlTape(const std::string& op) {
    if (op == "play") m_play = true;
    else if (op == "stop") m_play = false;
    else if (op == "rewind") { m_pos = 0; m_left = 0; m_level = true; }
}

void Tape::loadCas(const std::vector<uint8_t>& bytes) {
    m_pulses.clear();
    m_pulses.reserve(bytes.size() * 16 + 1);
    m_pulses.push_back({(uint32_t)(MASTER_HZ / 20), true});          // 50 ms of quiet tape first
    for (uint8_t b : bytes) {
        for (int i = 0; i < 8; ++i) {
            const uint32_t half = ((b >> i) & 1 ? TICKS_BIT1 : TICKS_BIT0) / 2;
            // The first half of each cycle below the comparator's threshold
            // (PA0 reads 1), the second above (0), as XRoar plays CAS files
            m_pulses.push_back({half, true});
            m_pulses.push_back({half, false});
        }
    }
    m_pos = 0;
    m_left = 0;
    m_level = true;
}

bool Tape::loadWav(const std::vector<uint8_t>& b) {
    if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) || std::memcmp(b.data() + 8, "WAVE", 4)) return false;
    uint16_t fmt = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    const uint8_t* data = nullptr;
    uint32_t dataLen = 0;
    for (size_t p = 12; p + 8 <= b.size();) {
        const uint32_t len = le32(&b[p + 4]);
        const uint8_t* body = &b[p + 8];
        const size_t avail = std::min<size_t>(len, b.size() - (p + 8));
        if (!std::memcmp(&b[p], "fmt ", 4) && avail >= 16) {
            fmt = le16(body);
            channels = le16(body + 2);
            rate = le32(body + 4);
            bits = le16(body + 14);
        } else if (!std::memcmp(&b[p], "data", 4)) {
            data = body;
            dataLen = (uint32_t)avail;
        }
        p += 8 + len + (len & 1);
    }
    if (fmt != 1 || !data || !rate || !channels || (bits != 8 && bits != 16)) return false;
    const size_t frame = (size_t)channels * (bits / 8);
    const size_t n = dataLen / frame;
    m_pulses.clear();
    const double ticksPerSample = MASTER_HZ / rate;
    double acc = 0;
    bool positive = false;
    for (size_t i = 0; i < n; ++i) {
        // The first channel, signed
        const uint8_t* s = data + i * frame;
        const int v = bits == 8 ? (int)s[0] - 128 : (int16_t)le16(s);
        const bool pos = positive ? v >= -(bits == 8 ? 2 : 512) : v > (bits == 8 ? 2 : 512);   // a little hysteresis
        if (pos != positive && acc >= 1.0) {
            m_pulses.push_back({(uint32_t)acc, !positive});   // above the threshold reads 0
            acc = 0;
        }
        positive = pos;
        acc += ticksPerSample;
    }
    if (acc >= 1.0) m_pulses.push_back({(uint32_t)acc, !positive});
    m_pos = 0;
    m_left = 0;
    m_level = true;
    return true;
}

void Tape::clock(uint64_t ticks) {
    m_now += ticks;
    if (!m_play || !m_motor) return;
    while (ticks && m_pos < m_pulses.size()) {
        if (m_left == 0) {
            m_left = m_pulses[m_pos].ticks;
            m_level = m_pulses[m_pos].level;
        }
        const uint64_t s = std::min(ticks, m_left);
        m_left -= s;
        ticks -= s;
        if (m_left == 0 && ++m_pos < m_pulses.size()) {
            m_left = m_pulses[m_pos].ticks;
            m_level = m_pulses[m_pos].level;
        }
    }
}

bool Tape::startTapeRecord() {
    m_recording = true;
    m_rec.clear();
    m_lastDac = 0xFF;
    return true;
}

void Tape::stopTapeRecord() { m_recording = false; }

void Tape::output(uint8_t dac6) {
    if (!m_recording || !m_motor || dac6 == m_lastDac) return;
    m_lastDac = dac6;
    m_rec.emplace_back(m_now, dac6);
}

std::vector<uint8_t> Tape::recordedCas() const {
    std::vector<uint8_t> out;
    if (m_rec.size() < 2) return out;
    // Half-cycles: the times between crossings of the DAC's midpoint
    std::vector<uint64_t> halves;
    bool high = m_rec[0].second >= 32;
    uint64_t last = m_rec[0].first;
    for (const auto& r : m_rec) {
        const bool h = r.second >= 32;
        if (h == high) continue;
        halves.push_back(r.first - last);
        last = r.first;
        high = h;
    }
    halves.push_back(GAP + 1);           // ends the last burst

    std::vector<int> cls;                // 1 short, 0 long, per half in a burst
    auto flush = [&]() {
        std::vector<uint8_t> bits;
        size_t i = 0;
        while (i + 1 < cls.size()) {
            if (cls[i] == cls[i + 1]) { bits.push_back((uint8_t)cls[i]); i += 2; }
            else ++i;                    // out of step: resynchronise
        }
        // The last half-cycle before a silence runs into it: a whole bit
        if (i + 1 == cls.size()) bits.push_back((uint8_t)cls[i]);
        cls.clear();
        // Byte alignment: on the first $3C (the sync byte after the leader)
        size_t start = 0;
        for (size_t k = 0; k + 8 <= bits.size(); ++k) {
            uint8_t v = 0;
            for (int b = 0; b < 8; ++b) v |= (uint8_t)(bits[k + b] << b);
            if (v == 0x3C) { start = k % 8; break; }
        }
        for (size_t k = start; k + 8 <= bits.size(); k += 8) {
            uint8_t v = 0;
            for (int b = 0; b < 8; ++b) v |= (uint8_t)(bits[k + b] << b);
            out.push_back(v);
        }
    };
    for (uint64_t h : halves) {
        if (h > GAP) { flush(); continue; }
        cls.push_back(h < HALF_SPLIT ? 1 : 0);
    }
    return out;
}

std::vector<uint8_t> Tape::recordedWav() const {
    std::vector<uint8_t> samples;
    if (!m_rec.empty()) {
        const uint64_t t0 = m_rec.front().first, t1 = m_rec.back().first + (uint64_t)MASTER_HZ / 10;
        size_t k = 0;
        uint8_t level = m_rec[0].second;
        for (uint64_t i = 0;; ++i) {
            const uint64_t t = t0 + (uint64_t)((double)i * MASTER_HZ / WAV_RATE);
            if (t > t1) break;
            while (k < m_rec.size() && m_rec[k].first <= t) level = m_rec[k++].second;
            samples.push_back((uint8_t)(level * 4 + 2));
        }
    }
    std::vector<uint8_t> w;
    w.insert(w.end(), {'R', 'I', 'F', 'F'});
    put32(w, (uint32_t)(36 + samples.size()));
    w.insert(w.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    put32(w, 16);
    put16(w, 1);
    put16(w, 1);
    put32(w, WAV_RATE);
    put32(w, WAV_RATE);
    put16(w, 1);
    put16(w, 8);
    w.insert(w.end(), {'d', 'a', 't', 'a'});
    put32(w, (uint32_t)samples.size());
    w.insert(w.end(), samples.begin(), samples.end());
    return w;
}

bool Tape::saveTapeRecording(const std::string& path) {
    const std::string e = extOf(path);
    std::vector<uint8_t> data;
    if (e == "cas") data = recordedCas();
    else if (e == "wav") data = recordedWav();
    else return false;
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    return f && f.write((const char*)data.data(), (std::streamsize)data.size());
}

}  // namespace coco
