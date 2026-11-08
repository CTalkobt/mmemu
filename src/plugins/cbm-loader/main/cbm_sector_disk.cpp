#include "cbm_sector_disk.h"
#include "d64_parser.h"
#include "d71_parser.h"
#include "d80_parser.h"
#include "d81_parser.h"
#include "d82_parser.h"
#include <fstream>
#include "libdevices/main/disk_file.h"
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
    39, 0,      // header: disk name, ID, DOS type (VICE vdrive-bam.c: "the
                // first BAM block with the disk name is at 39/0")
    0x06, 0x18, // disk name at +$06, ID at +$18
    38, 0,      // BAM at 38/0
    0x06, 5, 50, // BAM entries: 38/0 tracks 1-50, then 38/3 (getFreeBlocks follows the chain)
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
    39, 0,      // header (as the 8050)
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
    std::vector<uint8_t> bytes;
    if (!diskfile::read(path, bytes)) return false;   // plain, .gz or .zip

    bool valid = false;
    for (int i = 0; i < m_geom.numValidSizes; ++i) {
        if (bytes.size() == m_geom.validSizes[i]) { valid = true; break; }
    }
    if (!valid) return false;

    m_data = std::move(bytes);
    m_path = path;
    m_writable = diskfile::writable(path);
    return true;
}

bool CbmSectorDisk::writeSector(int track, int sector, const uint8_t* data) {
    uint32_t off = getSectorOffset(track, sector);
    if (off == 0xFFFFFFFF || off + 256 > m_data.size()) return false;
    std::copy(data, data + 256, m_data.begin() + off);
    if (!m_writable) return false;
    return diskfile::patch(m_path, m_data, off, 256);
}

bool CbmSectorDisk::formatSectors() {
    if (!m_writable) return false;
    const size_t n = std::min(m_data.size(), m_geom.baseSize());
    for (size_t off = 0; off < n; off += 256) {
        m_data[off] = 0x4B;
        std::fill(m_data.begin() + (long)off + 1, m_data.begin() + (long)off + 256, 0x01);
    }
    return flushAll();
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

std::string CbmSectorDisk::getDosType() {
    // The two bytes after the ID and a shifted space: "2A" (1541), "2C"
    // (8050/8250), "3D" (1581)
    if (m_data.empty()) return "";
    uint32_t offset = getSectorOffset(m_geom.headerTrack, m_geom.headerSector);
    if (offset == 0xFFFFFFFF || offset + m_geom.diskIdOffset + 5 > m_data.size()) return "";
    const uint8_t* sec = &m_data[offset];
    return std::string{(char)sec[m_geom.diskIdOffset + 3], (char)sec[m_geom.diskIdOffset + 4]};
}

// Blocks free as CBM DOS counts them: the free-sector counts in the BAM,
// leaving out the directory track
uint16_t CbmSectorDisk::getFreeBlocks() const {
    if (m_data.empty()) return 0;
    auto sector = [this](int t, int s) -> const uint8_t* {
        uint32_t o = getSectorOffset(t, s);
        return (o == 0xFFFFFFFF || o + 256 > m_data.size()) ? nullptr : &m_data[o];
    };
    std::string kind = m_geom.name;
    int total = 0;
    if (kind == "D64" || kind == "D71") {
        const uint8_t* bam = sector(18, 0);
        if (!bam) return 0;
        for (int t = 1; t <= 35; ++t)
            if (t != 18) total += bam[4 + (t - 1) * 4];
        if (kind == "D71")                                // side 2: counts at $DD-$FF
            for (int t = 36; t <= 70; ++t)
                if (t != 53) total += bam[0xDD + (t - 36)];
    } else if (kind == "D81") {
        for (int b = 1; b <= 2; ++b) {                    // 40/1: tracks 1-40, 40/2: 41-80
            const uint8_t* bam = sector(40, b);
            if (!bam) return 0;
            for (int i = 0; i < 40; ++i) {
                int t = (b - 1) * 40 + i + 1;
                if (t != 40) total += bam[0x10 + i * 6];
            }
        }
    } else if (kind == "D80" || kind == "D82") {
        // Bitmap blocks from 38/0, chained while they stay on track 38; each
        // holds tracks [byte 4, byte 5) at 5 bytes per track from offset 6
        int t = 38, s = 0;
        for (int guard = 0; t == 38 && guard < 8; ++guard) {
            const uint8_t* bam = sector(t, s);
            if (!bam) break;
            int first = bam[4], last = bam[5];
            for (int tr = first; tr < last && tr <= m_geom.totalTracks; ++tr)
                if (tr != m_geom.dirTrack) total += bam[6 + (tr - first) * 5];
            t = bam[0];
            s = bam[1];
        }
    }
    return (uint16_t)std::min(total, 65535);
}


// ---------------------------------------------------------------------------
// Writing files (CBM DOS). BAM layouts and allocation after VICE
// (src/vdrive/vdrive-bam.c, GPL-2.0-or-later): a set bit = a free sector.
// ---------------------------------------------------------------------------

namespace {
// The name as stored: PETSCII, padded with $A0 to 16 bytes
void storeName(uint8_t* at, const std::string& name) {
    for (int i = 0; i < 16; ++i) at[i] = i < (int)name.size() ? (uint8_t)name[(size_t)i] : 0xA0;
}
std::string entryName(const uint8_t* e) {
    std::string n;
    for (int i = 0; i < 16 && e[5 + i] != 0xA0; ++i) n += (char)e[5 + i];
    return n;
}
bool wildMatch(const std::string& pattern, const std::string& name) {
    for (size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] == '*') return true;
        if (i >= name.size()) return false;
        if (pattern[i] != '?' && pattern[i] != name[i]) return false;
    }
    return pattern.size() == name.size();
}
}  // namespace

int CbmSectorDisk::sectorsOn(int track) const {
    return (track >= 1 && track <= m_geom.totalTracks) ? m_geom.sectorsPerTrack[track - 1] : 0;
}

int CbmSectorDisk::interleave() const {
    std::string k = m_geom.name;
    if (k == "D71") return 6;
    if (k == "D81") return 1;
    if (k == "D80") return 6;
    if (k == "D82") return 5;
    return 10;                                              // 1541
}

int CbmSectorDisk::allocTracks() const {
    std::string k = m_geom.name;
    if (k == "D64") return 35;                              // the standard BAM
    return m_geom.totalTracks;
}

bool CbmSectorDisk::bamEntry(int track, uint8_t*& count, uint8_t*& bits) {
    auto sec = [this](int t, int s) -> uint8_t* {
        uint32_t o = getSectorOffset(t, s);
        return (o == 0xFFFFFFFF || o + 256 > m_data.size()) ? nullptr : &m_data[o];
    };
    std::string k = m_geom.name;
    if (track < 1 || track > allocTracks()) return false;
    if (k == "D64" || (k == "D71" && track <= 35)) {
        uint8_t* b = sec(18, 0);
        if (!b) return false;
        count = &b[4 + (track - 1) * 4];
        bits = count + 1;
        return true;
    }
    if (k == "D71") {                                       // side 2: counts in 18/0, bits in 53/0
        uint8_t* b = sec(18, 0);
        uint8_t* b2 = sec(53, 0);
        if (!b || !b2) return false;
        count = &b[0xDD + (track - 36)];
        bits = &b2[(track - 36) * 3];
        return true;
    }
    if (k == "D81") {
        uint8_t* b = sec(40, track <= 40 ? 1 : 2);
        if (!b) return false;
        count = &b[0x10 + 6 * ((track - 1) % 40)];
        bits = count + 1;
        return true;
    }
    // D80 / D82: BAM blocks chained on track 38, each covering [b4, b5)
    int t = 38, s = 0;
    for (int guard = 0; t == 38 && guard < 8; ++guard) {
        uint8_t* b = sec(t, s);
        if (!b) return false;
        if (track >= b[4] && track < b[5]) {
            count = &b[6 + 5 * (track - b[4])];
            bits = count + 1;
            return true;
        }
        t = b[0];
        s = b[1];
    }
    return false;
}

bool CbmSectorDisk::isFree(int track, int sector) {
    uint8_t *c, *b;
    if (!bamEntry(track, c, b) || sector < 0 || sector >= sectorsOn(track)) return false;
    return b[sector >> 3] & (1 << (sector & 7));
}

bool CbmSectorDisk::allocate(int track, int sector) {
    uint8_t *c, *b;
    if (!isFree(track, sector) || !bamEntry(track, c, b)) return false;
    b[sector >> 3] &= (uint8_t)~(1 << (sector & 7));
    --*c;
    return true;
}

bool CbmSectorDisk::release(int track, int sector) {
    uint8_t *c, *b;
    if (!bamEntry(track, c, b) || sector < 0 || sector >= sectorsOn(track) || isFree(track, sector)) return false;
    b[sector >> 3] |= (uint8_t)(1 << (sector & 7));
    ++*c;
    return true;
}

// vdrive_bam_alloc_worker: the first free sector on a track from `sector`
bool CbmSectorDisk::allocOnTrack(int track, int& sector) {
    const int n = sectorsOn(track);
    if (!n) return false;
    int s = sector % n;
    for (int i = 0; i < n; ++i) {
        if (allocate(track, s)) { sector = s; return true; }
        if (++s >= n) s = 0;
    }
    return false;
}

// vdrive_bam_alloc_first_free_sector: outward from the directory track
bool CbmSectorDisk::allocFirst(int& track, int& sector) {
    const int dir = m_geom.dirTrack, n = allocTracks();
    for (int d = 1; d <= n; ++d) {
        int s = 0;
        if (dir - d >= 1 && allocOnTrack(dir - d, s)) { track = dir - d; sector = s; return true; }
        s = 0;
        if (dir + d <= n && allocOnTrack(dir + d, s)) { track = dir + d; sector = s; return true; }
    }
    return false;
}

// vdrive_bam_alloc_next_free_sector_interleave: the same track at the
// interleave, then away from the directory track, then the other side
bool CbmSectorDisk::allocNext(int& track, int& sector, int il) {
    const int split = m_geom.dirTrack, n = allocTracks();
    auto addIl = [this, il](int t, int s) {
        int max = sectorsOn(t);
        if (!max) return 0;
        if (s >= max) s = 0;
        s += il;
        if (s >= max) { s -= max; if (s) --s; }
        return s;
    };
    int s = addIl(track, sector);
    if (allocOnTrack(track, s)) { sector = s; return true; }
    int t = track, cur = sector;
    for (int pass = 0; pass < 3; ++pass) {
        if (t > 0 && t < split) {
            for (int tt = t; tt >= 1; --tt) {
                int ss = addIl(tt, cur);
                if (allocOnTrack(tt, ss)) { track = tt; sector = ss; return true; }
            }
            t = split + 1;
        } else {
            for (int tt = t; tt <= n; ++tt) {
                if (tt == split) continue;
                int ss = addIl(tt, cur);
                if (allocOnTrack(tt, ss)) { track = tt; sector = ss; return true; }
            }
            t = split - 1;
        }
        cur = 0;
    }
    return false;
}

bool CbmSectorDisk::findEntry(const std::string& name, uint32_t& entryOffset) const {
    int t = m_geom.dirTrack, s = m_geom.dirSector;
    for (int guard = 0; t != 0 && guard < 200; ++guard) {
        uint32_t o = getSectorOffset(t, s);
        if (o == 0xFFFFFFFF || o + 256 > m_data.size()) return false;
        for (int i = 0; i < 8; ++i) {
            const uint8_t* e = &m_data[o + (uint32_t)i * 32];
            if ((e[2] & 0x07) && entryName(e) == name) { entryOffset = o + (uint32_t)i * 32; return true; }
        }
        t = m_data[o];
        s = m_data[o + 1];
    }
    return false;
}

// A free directory entry; a full chain gets a new sector on the directory
// track (3 sectors on, 1 on the 1581)
bool CbmSectorDisk::findSlot(uint32_t& entryOffset) {
    int t = m_geom.dirTrack, s = m_geom.dirSector;
    uint32_t last = 0;
    for (int guard = 0; t != 0 && guard < 200; ++guard) {
        uint32_t o = getSectorOffset(t, s);
        if (o == 0xFFFFFFFF || o + 256 > m_data.size()) return false;
        for (int i = 0; i < 8; ++i)
            if (m_data[o + (uint32_t)i * 32 + 2] == 0) { entryOffset = o + (uint32_t)i * 32; return true; }
        last = o;
        t = m_data[o];
        s = m_data[o + 1];
    }
    const int dir = m_geom.dirTrack, n = sectorsOn(dir), step = std::string(m_geom.name) == "D81" ? 1 : 3;
    int lastSector = 0;
    for (int i = 0; i < n; ++i)
        if (getSectorOffset(dir, i) == last) lastSector = i;
    for (int i = 1; i <= n; ++i) {
        int cand = (lastSector + step * i) % n;
        if (allocate(dir, cand)) {
            uint32_t o = getSectorOffset(dir, cand);
            std::fill(m_data.begin() + o, m_data.begin() + o + 256, 0);
            m_data[o + 1] = 0xFF;
            m_data[last] = (uint8_t)dir;
            m_data[last + 1] = (uint8_t)cand;
            entryOffset = o;
            return true;
        }
    }
    return false;
}

bool CbmSectorDisk::flushAll() {
    if (!m_writable) return false;
    return diskfile::patch(m_path, m_data, 0, m_data.size());
}

int CbmSectorDisk::writeFile(const std::string& name, int type, const std::vector<uint8_t>& data, bool replace) {
    if (!m_writable) return 26;
    if (name.empty() || name.size() > 16 || name.find_first_of("*?,:=") != std::string::npos) return 33;
    uint32_t existing = 0;
    const bool exists = findEntry(name, existing);
    if (exists && !replace) return 63;
    const std::vector<uint8_t> backup = m_data;            // undone on DISK FULL
    const int blocks = data.empty() ? 1 : (int)((data.size() + 253) / 254);
    uint32_t slot = 0;
    if (!findSlot(slot)) { m_data = backup; return 72; }
    std::vector<std::pair<int, int>> chain;
    int t = 0, s = 0;
    for (int i = 0; i < blocks; ++i) {
        bool ok = i == 0 ? allocFirst(t, s) : allocNext(t, s, interleave());
        if (!ok) { m_data = backup; return 72; }
        chain.push_back({t, s});
    }
    for (int i = 0; i < blocks; ++i) {
        uint32_t o = getSectorOffset(chain[(size_t)i].first, chain[(size_t)i].second);
        std::fill(m_data.begin() + o, m_data.begin() + o + 256, 0);
        size_t from = (size_t)i * 254, len = std::min<size_t>(254, data.size() > from ? data.size() - from : 0);
        std::copy(data.begin() + (long)from, data.begin() + (long)(from + len), m_data.begin() + o + 2);
        if (i + 1 < blocks) {
            m_data[o] = (uint8_t)chain[(size_t)i + 1].first;
            m_data[o + 1] = (uint8_t)chain[(size_t)i + 1].second;
        } else {
            m_data[o] = 0;
            m_data[o + 1] = (uint8_t)(len + 1);             // index of the last byte used
        }
    }
    if (exists) {                                           // @: the old file goes
        uint8_t* e = &m_data[existing];
        for (int tt = e[3], ss = e[4], guard = 0; tt && guard < 10000; ++guard) {
            uint32_t o = getSectorOffset(tt, ss);
            if (o == 0xFFFFFFFF) break;
            release(tt, ss);
            int nt = m_data[o], ns = m_data[o + 1];
            tt = nt; ss = ns;
        }
        e[2] = 0;
    }
    uint8_t* e = &m_data[slot];
    std::fill(e + 2, e + 32, 0);
    e[2] = (uint8_t)(0x80 | (type & 0x07));
    e[3] = (uint8_t)chain[0].first;
    e[4] = (uint8_t)chain[0].second;
    storeName(e + 5, name);
    e[30] = (uint8_t)(blocks & 0xFF);
    e[31] = (uint8_t)(blocks >> 8);
    return flushAll() ? 0 : 25;
}

int CbmSectorDisk::scratch(const std::string& pattern, int& count) {
    count = 0;
    if (!m_writable) return 26;
    int t = m_geom.dirTrack, s = m_geom.dirSector;
    for (int guard = 0; t != 0 && guard < 200; ++guard) {
        uint32_t o = getSectorOffset(t, s);
        if (o == 0xFFFFFFFF || o + 256 > m_data.size()) break;
        for (int i = 0; i < 8; ++i) {
            uint8_t* e = &m_data[o + (uint32_t)i * 32];
            if (!(e[2] & 0x07) || (e[2] & 0x40) || !wildMatch(pattern, entryName(e))) continue;   // locked: kept
            for (int ft = e[3], fs = e[4], g = 0; ft && g < 10000; ++g) {
                uint32_t fo = getSectorOffset(ft, fs);
                if (fo == 0xFFFFFFFF) break;
                release(ft, fs);
                int nt = m_data[fo], ns = m_data[fo + 1];
                ft = nt; fs = ns;
            }
            e[2] = 0;
            ++count;
        }
        t = m_data[o];
        s = m_data[o + 1];
    }
    return flushAll() || !count ? 1 : 25;                   // 01,FILES SCRATCHED
}

int CbmSectorDisk::rename(const std::string& newName, const std::string& oldName) {
    if (!m_writable) return 26;
    uint32_t o = 0, dummy = 0;
    if (findEntry(newName, dummy)) return 63;
    if (!findEntry(oldName, o)) return 62;
    if (newName.empty() || newName.size() > 16) return 33;
    storeName(&m_data[o + 5], newName);
    return flushAll() ? 0 : 25;
}
