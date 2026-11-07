#pragma once

// An MFM disk in an ImageDisk (.imd) file, as the 1571 reads and writes
// non-Commodore disks (CP/M formats: Kaypro, Osborne, Epson, IBM, ...).
//
// ImageDisk (Dave Dunfield): an ASCII header ("IMD v.vv: date time"), a
// comment, $1A, then each track: mode (0-2 FM, 3-5 MFM at 500/300/250
// kbps), cylinder, head (bit 7: a sector cylinder map follows, bit 6: a
// sector head map), sector count, sector size (0 = 128 ... 6 = 8192), the
// sector numbering map, the optional maps, then a record per sector:
// 0 unavailable, 1 data, 2 compressed (one fill byte), 3/4 with a deleted
// data mark, 5/6 with a data error, 7/8 both.

#include <cstdint>
#include <string>
#include <vector>

class ImdDisk {
public:
    struct Sector {
        uint8_t id = 0;          // the sector number in its header
        uint8_t cyl = 0;         // the cylinder in its header
        uint8_t head = 0;        // the head in its header
        uint8_t type = 1;        // the IMD record type (0 = no data)
        std::vector<uint8_t> data;
    };
    struct Track {
        uint8_t mode = 5;        // 250 kbps MFM
        uint8_t cyl = 0, head = 0;
        uint8_t sizeCode = 1;    // 128 << sizeCode bytes a sector
        bool cylMap = false, headMap = false;
        std::vector<Sector> sectors;   // in their physical order
    };

    /** Read an .imd file. False if it is not one. */
    bool open(const std::string& path);
    /** Write the disk back to its file. False if it cannot be written. */
    bool save();
    bool writable() const { return m_writable; }
    const std::string& path() const { return m_path; }

    /** The track at a physical cylinder and side, or nullptr. */
    Track* track(int cyl, int side);
    /** A sector by its header number on a cylinder and side, or nullptr. */
    Sector* sector(int cyl, int side, int id);
    /** Bytes in a sector of that track (0 if there is no track). */
    int sectorSize(int cyl, int side);
    const std::vector<Track>& tracks() const { return m_tracks; }

    /**
     * A low-level MFM format (the 1571's burst FORMAT MFM): cylinders 0 to
     * lastTrack on side `side` (both with doubleSided), `sectors` sectors of
     * 128 << sizeCode bytes filled with `fill`, numbered from `firstSector`
     * (or `table`: the numbers in physical order) laid out with
     * `interleave`, the header track numbers from `logicalTrack`. The image
     * file is written back.
     */
    bool format(int lastTrack, int sectors, int sizeCode, int firstSector, int interleave,
                int logicalTrack, int trackOffset, uint8_t fill, bool doubleSided, int side,
                const std::vector<uint8_t>& table);

private:
    std::string m_path, m_header;
    std::vector<Track> m_tracks;
    bool m_writable = false;
};
