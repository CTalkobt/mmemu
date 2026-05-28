#include "cbm_sector_disk.h"
#include "d64_parser.h"
#include "d71_parser.h"
#include "d80_parser.h"
#include "d81_parser.h"
#include "d82_parser.h"
#include <fstream>
#include <algorithm>
#include <cstring>

// ---------------------------------------------------------------------------
// Geometry tables
// ---------------------------------------------------------------------------

// D64: 1541 — 35 tracks
static const int s_d64Spt[] = {
    21,21,21,21,21,21,21,21,21,21,21,21,21,21,21,21,21, // T1-17
    19,19,19,19,19,19,19,                                 // T18-24
    18,18,18,18,18,18,                                     // T25-30
    17,17,17,17,17                                         // T31-35
};
static const size_t s_d64Sizes[] = { 174848, 175531, 196608 };
static const CbmDiskGeometry s_d64Geom = {
    "D64", 35, s_d64Spt,
    18, 1,      // dir track/sector
    18, 0,      // header (BAM) track/sector — disk name lives here
    0x90, 0xA2, // disk name / ID offsets within header sector
    18, 0,      // BAM track/sector
    4, 4, 35,   // BAM entry offset, entry size, track count
    s_d64Sizes, 3
};

// D71: 1571 — 70 tracks (double-sided D64)
static const int s_d71Spt[] = {
    21,21,21,21,21,21,21,21,21,21,21,21,21,21,21,21,21,
    19,19,19,19,19,19,19,
    18,18,18,18,18,18,
    17,17,17,17,17,
    // Side 2 (tracks 36-70): same pattern
    21,21,21,21,21,21,21,21,21,21,21,21,21,21,21,21,21,
    19,19,19,19,19,19,19,
    18,18,18,18,18,18,
    17,17,17,17,17
};
static const size_t s_d71Sizes[] = { 349696, 351062 };
static const CbmDiskGeometry s_d71Geom = {
    "D71", 70, s_d71Spt,
    18, 1,
    18, 0,
    0x90, 0xA2,
    18, 0,
    4, 4, 35,   // Primary BAM covers side 1 only
    s_d71Sizes, 2
};

// D81: 1581 — 80 tracks, 40 sectors each
static const int s_d81Spt[] = {
    40,40,40,40,40,40,40,40,40,40,40,40,40,40,40,40,40,40,40,40,
    40,40,40,40,40,40,40,40,40,40,40,40,40,40,40,40,40,40,40,40,
    40,40,40,40,40,40,40,40,40,40,40,40,40,40,40,40,40,40,40,40,
    40,40,40,40,40,40,40,40,40,40,40,40,40,40,40,40,40,40,40,40
};
static const size_t s_d81Sizes[] = { 819200, 822400 };
static const CbmDiskGeometry s_d81Geom = {
    "D81", 80, s_d81Spt,
    40, 3,      // dir starts at 40/3
    40, 0,      // header sector
    0x04, 0x16, // disk name at +$04, ID at +$16
    40, 1,      // BAM at 40/1
    0x10, 6, 40, // BAM entries: offset $10, 6 bytes each, 40 tracks per BAM sector
    s_d81Sizes, 2
};

// D80: 8050 — 77 tracks
static const int s_d80Spt[] = {
    29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,
    29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29, // T1-39
    27,27,27,27,27,27,27,27,27,27,27,27,27,27,                 // T40-53
    25,25,25,25,25,25,25,25,25,25,25,                           // T54-64
    23,23,23,23,23,23,23,23,23,23,23,23,23                     // T65-77
};
static const size_t s_d80Sizes[] = { 533248, 535331 };
static const CbmDiskGeometry s_d80Geom = {
    "D80", 77, s_d80Spt,
    39, 1,      // dir at 39/1
    38, 0,      // header sector
    0x06, 0x18, // disk name at +$06, ID at +$18
    38, 0,      // BAM at 38/0
    0x06, 5, 50, // BAM entries (approximate — D80 BAM is split across 38/0 and 38/3)
    s_d80Sizes, 2
};

// D82: 8250 — 154 tracks (double-sided D80)
static const int s_d82Spt[] = {
    // Side 1: tracks 1-77 (same as D80)
    29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,
    29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,
    27,27,27,27,27,27,27,27,27,27,27,27,27,27,
    25,25,25,25,25,25,25,25,25,25,25,
    23,23,23,23,23,23,23,23,23,23,23,23,23,
    // Side 2: tracks 78-154 (same pattern)
    29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,
    29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,
    27,27,27,27,27,27,27,27,27,27,27,27,27,27,
    25,25,25,25,25,25,25,25,25,25,25,
    23,23,23,23,23,23,23,23,23,23,23,23,23
};
static const size_t s_d82Sizes[] = { 1066496, 1070662 };
static const CbmDiskGeometry s_d82Geom = {
    "D82", 154, s_d82Spt,
    39, 1,
    38, 0,
    0x06, 0x18,
    38, 0,
    0x06, 5, 50,
    s_d82Sizes, 2
};

// ---------------------------------------------------------------------------
// Constructors for each format
// ---------------------------------------------------------------------------

D64Parser::D64Parser() : CbmSectorDisk(s_d64Geom) {}
D71Parser::D71Parser() : CbmSectorDisk(s_d71Geom) {}
D80Parser::D80Parser() : CbmSectorDisk(s_d80Geom) {}
D81Parser::D81Parser() : CbmSectorDisk(s_d81Geom) {}
D82Parser::D82Parser() : CbmSectorDisk(s_d82Geom) {}

// ---------------------------------------------------------------------------
// CbmDiskGeometry
// ---------------------------------------------------------------------------

int CbmDiskGeometry::totalSectors() const {
    int total = 0;
    for (int i = 0; i < totalTracks; ++i)
        total += sectorsPerTrack[i];
    return total;
}

// ---------------------------------------------------------------------------
// CbmSectorDisk
// ---------------------------------------------------------------------------

CbmSectorDisk::CbmSectorDisk(const CbmDiskGeometry& geom) : m_geom(geom) {}

bool CbmSectorDisk::open(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;

    file.seekg(0, std::ios::end);
    size_t size = file.tellg();
    file.seekg(0, std::ios::beg);

    bool valid = false;
    for (int i = 0; i < m_geom.numValidSizes; ++i) {
        if (size == m_geom.validSizes[i]) { valid = true; break; }
    }
    if (!valid) return false;

    m_data.resize(size);
    file.read(reinterpret_cast<char*>(m_data.data()), size);
    return true;
}

uint32_t CbmSectorDisk::getSectorOffset(int track, int sector) const {
    if (track < 1 || track > m_geom.totalTracks) return 0xFFFFFFFF;
    if (sector < 0 || sector >= m_geom.sectorsPerTrack[track - 1]) return 0xFFFFFFFF;

    uint32_t offset = 0;
    for (int i = 1; i < track; ++i)
        offset += m_geom.sectorsPerTrack[i - 1] * 256;
    offset += sector * 256;
    return offset;
}

std::vector<CbmDirEntry> CbmSectorDisk::getDirectory() const {
    std::vector<CbmDirEntry> entries;
    if (m_data.empty()) return entries;

    int track = m_geom.dirTrack;
    int sector = m_geom.dirSector;
    int safety = 0;

    while (track != 0 && safety++ < 200) {
        uint32_t offset = getSectorOffset(track, sector);
        if (offset == 0xFFFFFFFF || offset + 256 > m_data.size()) break;

        const uint8_t* secData = &m_data[offset];
        int nextTrack = secData[0];
        int nextSector = secData[1];

        for (int i = 0; i < 8; ++i) {
            const uint8_t* e = &secData[i * 32];
            uint8_t type = e[2];
            if (type == 0) continue;

            CbmDirEntry entry;
            char name[17];
            std::memcpy(name, &e[5], 16);
            name[16] = '\0';
            for (int j = 15; j >= 0; --j) {
                if (static_cast<uint8_t>(name[j]) == 0xA0) name[j] = '\0';
                else break;
            }
            entry.filename = name;
            entry.type = type;
            entry.sizeBlocks = e[30] | (e[31] << 8);
            entries.push_back(entry);
        }

        track = nextTrack;
        sector = nextSector;

        if (track == m_geom.dirTrack && sector == m_geom.dirSector) break;
    }

    return entries;
}

bool CbmSectorDisk::readFile(const std::string& filename, std::vector<uint8_t>& data) {
    if (m_data.empty()) return false;

    int startTrack = 0, startSector = 0;

    // Walk directory to find the file's start track/sector
    int dt = m_geom.dirTrack, ds = m_geom.dirSector;
    int safety = 0;
    while (dt != 0 && safety++ < 200) {
        uint32_t offset = getSectorOffset(dt, ds);
        if (offset == 0xFFFFFFFF || offset + 256 > m_data.size()) break;
        const uint8_t* secData = &m_data[offset];

        for (int i = 0; i < 8; ++i) {
            const uint8_t* e = &secData[i * 32];
            if (e[2] == 0) continue;

            char name[17];
            std::memcpy(name, &e[5], 16);
            name[16] = '\0';
            for (int j = 15; j >= 0; --j) {
                if (static_cast<uint8_t>(name[j]) == 0xA0) name[j] = '\0';
                else break;
            }

            if (filename == name) {
                startTrack = e[3];
                startSector = e[4];
                goto found;
            }
        }

        dt = secData[0];
        ds = secData[1];
    }
    return false;

found:
    data.clear();
    int t = startTrack, s = startSector;
    safety = 0;
    while (t != 0 && safety++ < 10000) {
        uint32_t offset = getSectorOffset(t, s);
        if (offset == 0xFFFFFFFF || offset + 256 > m_data.size()) break;
        const uint8_t* secData = &m_data[offset];

        int nextT = secData[0];
        int nextS = secData[1];

        if (nextT == 0) {
            // Last sector: nextS is number of data bytes used (including byte index 1)
            for (int i = 2; i <= nextS; ++i)
                data.push_back(secData[i]);
        } else {
            for (int i = 2; i < 256; ++i)
                data.push_back(secData[i]);
        }

        t = nextT;
        s = nextS;
    }
    return true;
}

std::string CbmSectorDisk::getDiskName() {
    if (m_data.empty()) return "";

    uint32_t offset = getSectorOffset(m_geom.headerTrack, m_geom.headerSector);
    if (offset == 0xFFFFFFFF || offset + m_geom.diskNameOffset + 16 > m_data.size())
        return "";

    const uint8_t* sec = &m_data[offset];
    char name[17];
    std::memcpy(name, &sec[m_geom.diskNameOffset], 16);
    name[16] = '\0';

    for (int i = 15; i >= 0; --i) {
        if (static_cast<uint8_t>(name[i]) == 0xA0) name[i] = '\0';
        else break;
    }
    return name;
}

std::string CbmSectorDisk::getDiskId() {
    if (m_data.empty()) return "";

    uint32_t offset = getSectorOffset(m_geom.headerTrack, m_geom.headerSector);
    if (offset == 0xFFFFFFFF || offset + m_geom.diskIdOffset + 2 > m_data.size())
        return "";

    const uint8_t* sec = &m_data[offset];
    char id[3];
    id[0] = static_cast<char>(sec[m_geom.diskIdOffset]);
    id[1] = static_cast<char>(sec[m_geom.diskIdOffset + 1]);
    id[2] = '\0';
    return id;
}

uint16_t CbmSectorDisk::getFreeBlocks() const {
    if (m_data.empty()) return 0;

    // Try reading free counts from BAM
    uint32_t bamOff = getSectorOffset(m_geom.bamTrack, m_geom.bamSector);
    if (bamOff != 0xFFFFFFFF && bamOff + 256 <= m_data.size()) {
        uint16_t total = 0;
        for (int i = 0; i < m_geom.bamTrackCount; ++i) {
            uint32_t entryOff = bamOff + m_geom.bamEntryOffset + i * m_geom.bamEntrySize;
            if (entryOff >= m_data.size()) break;
            total += m_data[entryOff]; // First byte of each BAM entry is free count
        }
        if (total > 0) return total;
    }

    // Fallback: total sectors minus directory track minus used blocks
    int totalSec = m_geom.totalSectors();
    int dirTrackSectors = m_geom.sectorsPerTrack[m_geom.dirTrack - 1];
    uint16_t available = totalSec - dirTrackSectors;

    uint16_t used = 0;
    auto entries = getDirectory();
    for (const auto& e : entries)
        used += e.sizeBlocks;

    return (used > available) ? 0 : available - used;
}
