#include "tap_parser.h"
#include <fstream>
#include <cstring>
#include <cstdio>

bool TapArchive::load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return false;

    f.read(m_header.signature, 12);
    m_c16 = std::strncmp(m_header.signature, "C16-TAPE-RAW", 12) == 0;
    if (!m_c16 && std::strncmp(m_header.signature, "C64-TAPE-RAW", 12) != 0) return false;

    f.read((char*)&m_header.version, 1);
    f.read((char*)m_header.reserved, 3);
    
    uint8_t sizeBytes[4];
    f.read((char*)sizeBytes, 4);
    m_header.dataSize = sizeBytes[0] | (sizeBytes[1] << 8) | (sizeBytes[2] << 16) | (sizeBytes[3] << 24);

    m_data.resize(m_header.dataSize);
    f.read((char*)m_data.data(), m_header.dataSize);

    return true;
}

void TapArchive::startRecording() {
    m_recordBuf.clear();
    m_recording = true;
}

void TapArchive::appendPulse(uint32_t cycles) {
    uint32_t val = cycles / 8;
    if (val > 0 && val <= 0xFF) {
        m_recordBuf.push_back((uint8_t)val);
    } else {
        // Long pulse: leading zero + 3 LE bytes of raw cycle count
        m_recordBuf.push_back(0x00);
        m_recordBuf.push_back((uint8_t)(cycles & 0xFF));
        m_recordBuf.push_back((uint8_t)((cycles >> 8) & 0xFF));
        m_recordBuf.push_back((uint8_t)((cycles >> 16) & 0xFF));
    }
}

bool TapArchive::saveRecording(const std::string& path) const {
    std::ofstream f(path, std::ios::binary);
    if (!f.is_open()) return false;

    // Header: 12-byte signature, version (C64: 1; C16: 2, half-waves),
    // platform and video standard (C16: 2, 0 PAL / 1 NTSC), a reserved byte
    f.write(m_recC16 ? "C16-TAPE-RAW" : "C64-TAPE-RAW", 12);
    uint8_t ver = m_recC16 ? 2 : 1;
    f.write((const char*)&ver, 1);
    uint8_t reserved[3] = {0, 0, 0};
    if (m_recC16) { reserved[0] = 2; reserved[1] = m_recNtsc ? 1 : 0; }
    f.write((const char*)reserved, 3);
    // Data size (little-endian 32-bit)
    uint32_t sz = (uint32_t)m_recordBuf.size();
    uint8_t sizeBytes[4] = {
        (uint8_t)(sz & 0xFF),
        (uint8_t)((sz >> 8) & 0xFF),
        (uint8_t)((sz >> 16) & 0xFF),
        (uint8_t)((sz >> 24) & 0xFF)
    };
    f.write((const char*)sizeBytes, 4);
    // Data
    if (!m_recordBuf.empty())
        f.write((const char*)m_recordBuf.data(), m_recordBuf.size());

    return f.good();
}

uint32_t TapArchive::nextPulse(uint32_t& offset) const {
    if (offset >= m_data.size()) return 0;

    uint8_t val = m_data[offset++];
    if (val > 0) {
        return val * 8;
    } else {
        // Overflow or special case. In version 0/1, it is followed by 3 bytes for 24-bit value.
        if (offset + 3 > m_data.size()) return 0;
        uint32_t longVal = m_data[offset] | (m_data[offset+1] << 8) | (m_data[offset+2] << 16);
        offset += 3;
        return longVal;
    }
}
