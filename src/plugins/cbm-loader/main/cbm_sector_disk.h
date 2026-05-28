#pragma once
#include "cbm_disk_image.h"
#include <vector>
#include <cstdint>
#include <cstddef>

struct CbmDiskGeometry {
    const char* name;
    int totalTracks;
    const int* sectorsPerTrack;     // Array[totalTracks]
    int dirTrack;                   // First directory sector track
    int dirSector;                  // First directory sector number
    int headerTrack;                // Sector containing disk name/ID
    int headerSector;
    int diskNameOffset;             // Byte offset within header sector
    int diskIdOffset;               // Byte offset within header sector
    int bamTrack;                   // Primary BAM sector
    int bamSector;
    int bamEntryOffset;             // Byte offset of first BAM entry in BAM sector
    int bamEntrySize;               // Bytes per BAM entry (4 for D64/D71, 6 for D81)
    int bamTrackCount;              // Tracks covered by primary BAM sector
    const size_t* validSizes;
    int numValidSizes;

    int totalSectors() const;
    size_t baseSize() const { return totalSectors() * 256; }
};

class CbmSectorDisk : public ICbmDiskImage {
public:
    explicit CbmSectorDisk(const CbmDiskGeometry& geom);

    bool open(const std::string& path) override;
    std::vector<CbmDirEntry> getDirectory() const override;
    bool readFile(const std::string& filename, std::vector<uint8_t>& data) override;
    std::string getDiskName() override;
    std::string getDiskId() override;
    uint16_t getFreeBlocks() const override;

    const CbmDiskGeometry& geometry() const { return m_geom; }

protected:
    const CbmDiskGeometry& m_geom;
    std::vector<uint8_t> m_data;

    uint32_t getSectorOffset(int track, int sector) const;
};
