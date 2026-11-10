#include "snes_msu1.h"

#include <algorithm>

namespace snes {

namespace {

bool exists(const std::string& path) { return (bool)std::ifstream(path, std::ios::binary); }

// Paul Bourke's Hermite interpolation, as Mesen's HermiteResampler
int16_t hermite(const double v[4], double mu) {
    const double mu2 = mu * mu, mu3 = mu2 * mu;
    const double m0 = (v[1] - v[0]) / 2 + (v[2] - v[1]) / 2;
    const double m1 = (v[2] - v[1]) / 2 + (v[3] - v[2]) / 2;
    const double a0 = 2 * mu3 - 3 * mu2 + 1, a1 = mu3 - 2 * mu2 + mu, a2 = mu3 - mu2, a3 = -2 * mu3 + 3 * mu2;
    return (int16_t)std::clamp(a0 * v[1] + a1 * m0 + a2 * m1 + a3 * v[2], -32768.0, 32767.0);
}

void push(double v[4], int16_t s) { v[0] = v[1]; v[1] = v[2]; v[2] = v[3]; v[3] = s; }

constexpr double RATIO = 44100.0 / 32000.0;     // source samples per DSP sample

}  // namespace

std::unique_ptr<Msu1> Msu1::open(const std::string& romPath) {
    if (romPath.empty()) return nullptr;
    const auto slash = romPath.find_last_of('/');
    const std::string folder = slash == std::string::npos ? "." : romPath.substr(0, slash);
    std::string name = slash == std::string::npos ? romPath : romPath.substr(slash + 1);
    const auto dot = name.find_last_of('.');
    if (dot != std::string::npos) name = name.substr(0, dot);
    const std::string stem = folder + "/" + name;
    if (exists(stem + ".msu")) return std::make_unique<Msu1>(stem + ".msu", stem);
    if (exists(folder + "/msu1.rom")) return std::make_unique<Msu1>(folder + "/msu1.rom", folder + "/track");
    return nullptr;
}

Msu1::Msu1(const std::string& dataPath, const std::string& trackPrefix) : m_dataPath(dataPath), m_trackPrefix(trackPrefix) {
    powerOn();
}

void Msu1::powerOn() {
    m_data.close();
    m_data.clear();
    m_data.open(m_dataPath, std::ios::binary);
    m_dataSize = 0;
    if (m_data) {
        m_data.seekg(0, std::ios::end);
        m_dataSize = (uint32_t)m_data.tellg();
        m_data.seekg(0, std::ios::beg);
    }
    m_volume = 100;
    m_trackSelect = 0;
    m_tmpDataPointer = m_dataPointer = 0;
    m_repeat = m_paused = m_trackMissing = false;
    m_track.close();
    m_track.clear();
    m_trackSize = m_trackOffset = 0;
    m_loopOffset = 8;
    m_done = true;
    std::fill(std::begin(m_prevL), std::end(m_prevL), 0.0);
    std::fill(std::begin(m_prevR), std::end(m_prevR), 0.0);
    m_fraction = 0;
}

uint8_t Msu1::read(uint16_t a, bool peek) {
    switch (a & 7) {
    case 0:
        return (uint8_t)(DATA_BUSY << 7 | AUDIO_BUSY << 6 | m_repeat << 5 | (!m_paused) << 4 | m_trackMissing << 3 | 0x01);
    case 1:
        // The data port: the next byte of the data file, 0 past its end
        if (DATA_BUSY || m_dataPointer >= m_dataSize) return 0;
        if (peek) return (uint8_t)m_data.peek();
        ++m_dataPointer;
        return (uint8_t)m_data.get();
    default:
        return (uint8_t)"S-MSU1"[(a & 7) - 2];
    }
}

void Msu1::write(uint16_t a, uint8_t v) {
    switch (a & 7) {
    case 0: m_tmpDataPointer = (m_tmpDataPointer & 0xFFFFFF00) | v; break;
    case 1: m_tmpDataPointer = (m_tmpDataPointer & 0xFFFF00FF) | (uint32_t)v << 8; break;
    case 2: m_tmpDataPointer = (m_tmpDataPointer & 0xFF00FFFF) | (uint32_t)v << 16; break;
    case 3:
        m_tmpDataPointer = (m_tmpDataPointer & 0x00FFFFFF) | (uint32_t)v << 24;
        m_dataPointer = m_tmpDataPointer;
        m_data.clear();
        m_data.seekg(m_dataPointer, std::ios::beg);
        break;
    case 4: m_trackSelect = (uint16_t)((m_trackSelect & 0xFF00) | v); break;
    case 5:
        m_trackSelect = (uint16_t)((m_trackSelect & 0x00FF) | v << 8);
        loadTrack();
        break;
    case 6: m_volume = v; break;
    case 7:
        if (!AUDIO_BUSY) {
            m_repeat = v & 0x02;
            m_paused = !(v & 0x01);
        }
        break;
    }
}

void Msu1::loadTrack(uint32_t startOffset) {
    // Mesen's PcmReader::Init: a file under 12 bytes counts as missing
    m_track.close();
    m_track.clear();
    m_track.open(m_trackPrefix + "-" + std::to_string(m_trackSelect) + ".pcm", std::ios::binary);
    m_trackMissing = true;
    m_done = true;
    if (!m_track) return;
    m_track.seekg(0, std::ios::end);
    m_trackSize = (uint32_t)m_track.tellg();
    if (m_trackSize < 12) return;
    uint8_t b[4] = {};
    m_track.seekg(4, std::ios::beg);
    m_track.read((char*)b, 4);
    m_loopOffset = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
    m_trackOffset = startOffset;
    m_track.seekg(m_trackOffset, std::ios::beg);
    std::fill(std::begin(m_prevL), std::end(m_prevL), 0.0);
    std::fill(std::begin(m_prevR), std::end(m_prevR), 0.0);
    m_fraction = 0;
    m_trackMissing = false;
    m_done = false;
}

bool Msu1::nextSample(int16_t& l, int16_t& r) {
    if (m_trackOffset + 4 > m_trackSize) {
        // The end: back to the loop point when repeating (a loop point past the end ends it)
        const uint64_t loop = (uint64_t)m_loopOffset * 4 + 8;
        if (!m_repeat || loop + 4 > m_trackSize) return false;
        m_trackOffset = (uint32_t)loop;
        m_track.clear();
        m_track.seekg(m_trackOffset, std::ios::beg);
    }
    uint8_t b[4] = {};
    m_track.read((char*)b, 4);
    m_trackOffset += 4;
    l = (int16_t)(b[0] | b[1] << 8);
    r = (int16_t)(b[2] | b[3] << 8);
    return true;
}

void Msu1::mix(int32_t& left, int32_t& right) {
    if (m_paused) return;
    if (m_done) { m_paused = true; return; }       // the track over (or missing): playing clears
    while (m_fraction > 1.0) {
        int16_t l, r;
        if (!nextSample(l, r)) { m_done = true; m_paused = true; return; }
        push(m_prevL, l);
        push(m_prevR, r);
        m_fraction -= 1.0;
    }
    left += hermite(m_prevL, m_fraction) * m_volume / 255;
    right += hermite(m_prevR, m_fraction) * m_volume / 255;
    m_fraction += RATIO;
}

}  // namespace snes
