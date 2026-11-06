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
    /** The DOS type after the disk ID ("2A", "2C", "3D"), as the directory header shows it. */
    std::string getDosType();
    /** The raw image (for a DOS reading files by track and sector). */
    const std::vector<uint8_t>& data() const { return m_data; }
    /** Byte offset of a track/sector, 0xFFFFFFFF if out of range. */
    uint32_t sectorOffset(int track, int sector) const { return getSectorOffset(track, sector); }
    /**
     * Write 256 bytes to a track/sector and back to the image file (as
     * VICE does). False if the sector is out of range or the file cannot be
     * written (it stays changed in memory only).
     */
    bool writeSector(int track, int sector, const uint8_t* data);
    /**
     * A low-level format: every sector of the image as a freshly formatted
     * 1541/1571 data block ($4B, then 255 x $01), written back to the file.
     * No directory (the host writes one). False if the file is read-only.
     */
    bool formatSectors();
    /** The file is writable (not read-only on the host). */
    bool writable() const { return m_writable; }

    // -----------------------------------------------------------------------
    // Writing files, as CBM DOS does (allocation after VICE vdrive-bam.c).
    // Each returns a DOS error code (0 = OK) and writes the changed image
    // back to its file.
    // -----------------------------------------------------------------------
    /** Write a file (type 1 SEQ, 2 PRG, 3 USR); `replace` scratches an
     *  existing one (`@:`) after the new one is written. 63 FILE EXISTS,
     *  72 DISK FULL, 26 WRITE PROTECT ON. */
    int writeFile(const std::string& name, int type, const std::vector<uint8_t>& data, bool replace = false);
    /** Scratch the files matching `pattern` (CBM wildcards); `count` gets
     *  how many. */
    int scratch(const std::string& pattern, int& count);
    /** Rename `oldName` to `newName`: 62 FILE NOT FOUND, 63 FILE EXISTS. */
    int rename(const std::string& newName, const std::string& oldName);
    /** Whether a track/sector is free in the BAM (false if outside it). */
    bool isFree(int track, int sector);

    const CbmDiskGeometry& geometry() const { return m_geom; }

protected:
    const CbmDiskGeometry& m_geom;
    std::vector<uint8_t> m_data;
    std::string m_path;
    bool m_writable = false;

    uint32_t getSectorOffset(int track, int sector) const;

    // BAM: the entry for a track (count byte, bitmap), allocation
    bool bamEntry(int track, uint8_t*& count, uint8_t*& bits);
    bool allocate(int track, int sector);
    bool release(int track, int sector);
    int  interleave() const;
    int  allocTracks() const;                // tracks the BAM covers
    bool allocFirst(int& track, int& sector);
    bool allocNext(int& track, int& sector, int interleave);
    bool allocOnTrack(int track, int& sector);
    int  sectorsOn(int track) const;
    // The directory: a free slot (allocating a sector if the chain is full)
    bool findSlot(uint32_t& entryOffset);
    bool findEntry(const std::string& name, uint32_t& entryOffset) const;
    bool flushAll();
};
