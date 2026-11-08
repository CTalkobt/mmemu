#include "g64_parser.h"
#include "libdevices/main/disk_file.h"
#include <fstream>
#include <cstring>

G64Parser::G64Parser() {}

bool G64Parser::open(const std::string& path) {
    if (!diskfile::read(path, m_data)) return false;      // plain, .gz or .zip
    const size_t size = m_data.size();
    
    if (size < 12) return false;
    
    // Header "GCR-1541"
    if (std::memcmp(m_data.data(), "GCR-1541", 8) != 0) return false;
    
    uint8_t version = m_data[8];
    uint8_t numTracks = m_data[9];
    uint16_t trackSize = m_data[10] | (m_data[11] << 8);
    
    m_tracks.clear();
    for (int i = 0; i < numTracks; ++i) {
        uint32_t offsetOff = 12 + i * 4;
        uint32_t speedOff = 12 + numTracks * 4 + i * 4;
        
        if (speedOff + 4 > size) break;
        
        TrackInfo info;
        info.offset = m_data[offsetOff] | (m_data[offsetOff+1] << 8) | 
                      (m_data[offsetOff+2] << 16) | (m_data[offsetOff+3] << 24);
        info.speed = m_data[speedOff] | (m_data[speedOff+1] << 8) | 
                     (m_data[speedOff+2] << 16) | (m_data[speedOff+3] << 24);
        
        m_tracks.push_back(info);
    }
    
    return true;
}

std::vector<CbmDirEntry> G64Parser::getDirectory() const {
    // Implementing directory parsing for G64 is complex because it requires
    // decoding GCR to sectors. For Phase 15.3, we provide the basic support.
    return std::vector<CbmDirEntry>();
}

bool G64Parser::readFile(const std::string& filename, std::vector<uint8_t>& data) {
    // Requires GCR decoder.
    return false;
}

bool G64Parser::getTrackData(int track, std::vector<uint8_t>& gcrData) {
    if (track < 1 || track > (int)m_tracks.size()) return false;
    
    uint32_t offset = m_tracks[track-1].offset;
    if (offset == 0 || offset >= m_data.size()) return false;
    
    uint16_t actualSize = m_data[offset] | (m_data[offset+1] << 8);
    if (offset + 2 + actualSize > m_data.size()) return false;
    
    gcrData.assign(m_data.begin() + offset + 2, m_data.begin() + offset + 2 + actualSize);
    return true;
}
