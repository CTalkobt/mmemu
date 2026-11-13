#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

/**
 * Track-level floppy disk images for the WD17xx controller (#330, #331).
 *
 * A disk is held as raw tracks, the way DMK stores them: each track is the
 * byte stream one revolution of the head sees, plus a table of where its ID
 * address marks ($FE) are. Every slot is one double-density (MFM) byte time;
 * a single-density (FM) byte takes two slots and is stored twice, so single,
 * double and mixed density tracks share one time base. A 5.25" 300 RPM
 * double-density track is 6250 byte times (DMK's usual 0x1900 track holds
 * 6272).
 *
 * Sector images (JVC, VDK, JV1, JV3) are turned into formatted tracks when
 * they are loaded and read back out of the tracks when they are saved, so the
 * controller only ever sees tracks; DMK is loaded and saved as is.
 *
 * | Format | Machines | Geometry |
 * |--------|----------|----------|
 * | JVC (`.dsk`, `.jvc`, `.os9`) | CoCo, Dragon | optional header (file size mod 256): sectors/track, sides, size code, first sector, attribute flag; default 18 x 256-byte DD sectors, 1 side; headerless OS-9 images by their LSN0 |
 * | VDK (`.vdk`) | Dragon, CoCo | "dk" header with cylinders, heads, write protect; 18 x 256 DD |
 * | DMK (`.dmk`, `.dsk`) | CoCo, TRS-80 | raw tracks with IDAM tables |
 * | JV1 (`.jv1`, `.dsk`) | TRS-80 Model I | 10 x 256 SD sectors numbered 0-9, one side; track 17 (the directory) has the $FA data mark |
 * | JV3 (`.jv3`, `.dsk`) | TRS-80 | 2901 sector headers (track, sector, flags: density, data mark, side, CRC error, size) per block |
 */

/** Disk image file formats. */
enum class FloppyFormat { Auto, JVC, VDK, DMK, JV1, JV3 };

/** One raw track. */
struct FloppyTrack {
    /** Offset flag in `idams`: the address mark is double density. */
    static constexpr uint16_t IDAM_DD = 0x8000;
    static constexpr uint16_t IDAM_OFFSET = 0x3FFF;
    /** DMK's limit on IDAMs a track. */
    static constexpr int MAX_IDAMS = 64;

    std::vector<uint8_t> data;              // one byte per MFM byte time; FM bytes doubled
    std::vector<uint16_t> idams;            // slot of each $FE (| IDAM_DD), ascending

    int length() const { return (int)data.size(); }
    uint8_t at(uint64_t slot) const { return data.empty() ? 0 : data[slot % data.size()]; }
    /** Record an IDAM at `slot` (replacing any already there). */
    void addIdam(int slot, bool dd);
    /** Drop IDAMs in the slots [from, from + n) (they were overwritten). */
    void clearIdams(int from, int n);
};

/** A sector found on a track by FloppyDisk::scanTrack. */
struct FloppySector {
    uint8_t track = 0, side = 0, sector = 0, sizeCode = 0;   // the ID field
    bool dd = false;                        // its density
    bool idCrcOk = true;
    bool hasData = false;                   // a data mark followed the ID
    uint8_t dam = 0xFB;                     // $FB data, $F8 deleted ($F9/$FA on FM)
    bool dataCrcOk = true;
    std::vector<uint8_t> data;              // 128 << (sizeCode & 3) bytes
};

/** A sector to lay down with FloppyDisk::formatTrack. */
struct FloppySectorSpec {
    uint8_t track = 0, side = 0, sector = 1, sizeCode = 1;
    bool dd = true;
    uint8_t dam = 0xFB;
    bool badCrc = false;                    // write a wrong data CRC (JV3's error flag)
    std::vector<uint8_t> data;              // padded/truncated to the size code's length
};

/** CRC-16/CCITT (poly $1021) as the WD17xx computes it. */
uint16_t floppyCrc(uint16_t crc, uint8_t b);
constexpr uint16_t FLOPPY_CRC_INIT = 0xFFFF;
/** The CRC an MFM address mark starts from: $FFFF after three $A1 syncs. */
uint16_t floppyCrcAfterA1();

/** A whole disk: cylinders x heads raw tracks, and where it came from. */
class FloppyDisk {
public:
    /** Data bytes of a 5.25" double-density track (DMK 0x1900 less its table). */
    static constexpr int TRACK_LEN_DD = 0x1900 - 128;
    static constexpr int MAX_CYLINDERS = 96;
    static constexpr int MAX_HEADS = 2;

    /** An unformatted disk (tracks filled with $4E, no IDAMs). */
    FloppyDisk(int cylinders, int heads, int trackLength = TRACK_LEN_DD);

    /** Load an image file; the format from the extension and contents unless given. */
    static std::unique_ptr<FloppyDisk> load(const std::string& path, std::string* err = nullptr,
                                            FloppyFormat fmt = FloppyFormat::Auto);
    /** From an image's bytes; `nameHint` (a file name) helps pick the format. */
    static std::unique_ptr<FloppyDisk> fromBytes(const std::vector<uint8_t>& bytes, FloppyFormat fmt,
                                                 std::string* err = nullptr,
                                                 const std::string& nameHint = "");
    /** The format an image's bytes and name look like. */
    static FloppyFormat detect(const std::vector<uint8_t>& bytes, const std::string& nameHint);
    static const char* formatName(FloppyFormat fmt);

    /** The disk as an image file's bytes. Sector formats keep the geometry
     *  they were loaded with (JVC/VDK/JV1); sectors not found are zero. */
    std::vector<uint8_t> toBytes(FloppyFormat fmt, std::string* err = nullptr) const;
    /** Write back to the file it came from, in its format. A disk with no
     *  file, or a file that can't be written, stays in memory (false). */
    bool save(std::string* err = nullptr);
    /** Write to `path` as `fmt`; it then becomes the disk's file. */
    bool saveAs(const std::string& path, FloppyFormat fmt, std::string* err = nullptr);

    int cylinders() const { return m_cyls; }
    int heads() const { return m_heads; }
    int trackLength() const { return m_trackLen; }
    /** A track, or null if the disk has none there. */
    FloppyTrack* track(int cyl, int head);
    const FloppyTrack* track(int cyl, int head) const;
    /** The track, adding cylinders/heads (unformatted) up to it if needed. */
    FloppyTrack* extend(int cyl, int head);

    bool writeProtected() const { return m_writeProtect; }
    void setWriteProtected(bool wp) { m_writeProtect = wp; }
    bool dirty() const { return m_dirty; }
    void markDirty() { m_dirty = true; }
    const std::string& path() const { return m_path; }
    FloppyFormat format() const { return m_format; }
    /** The file couldn't be opened for writing: changes stay in memory. */
    bool readOnlyFile() const { return m_readOnlyFile; }

    /** Lay down a formatted track (IBM gaps, the sectors in the order given;
     *  each sector in its own density). False if they can't fit even with
     *  the track lengthened to the DMK maximum. */
    bool formatTrack(int cyl, int head, const std::vector<FloppySectorSpec>& sectors);
    /** Every ID field on the track in rotation order, with the data behind it. */
    std::vector<FloppySector> scanTrack(int cyl, int head) const;
    /** Read the first sector with ID `sector` (and the cylinder as its track
     *  number); false if there is none or it has no data. */
    bool readSector(int cyl, int head, int sector, std::vector<uint8_t>& out, uint8_t* dam = nullptr) const;
    /** Rewrite a sector's data in place (with a fresh CRC). */
    bool writeSector(int cyl, int head, int sector, const std::vector<uint8_t>& data, uint8_t dam = 0xFB);

private:
    friend struct FloppyCodec;
    int index(int cyl, int head) const { return cyl * MAX_HEADS + head; }

    int m_cyls, m_heads, m_trackLen;
    std::vector<FloppyTrack> m_tracks;      // [cyl * MAX_HEADS + head]
    bool m_writeProtect = false, m_dirty = false, m_readOnlyFile = false;
    std::string m_path;
    FloppyFormat m_format = FloppyFormat::DMK;

    // What the sector formats were loaded with, to save them the same way
    int m_sectorsPerTrack = 18, m_sizeCode = 1, m_firstSector = 1;
    bool m_dd = true;
    std::vector<uint8_t> m_jvcHeader;       // JVC header bytes as loaded
    bool m_jvcAttr = false;                 // JVC sector attribute bytes
    bool m_jvcHeaderless = false;           // a headerless OS-9 image (geometry from LSN0)
    std::vector<uint8_t> m_vdkHeader;       // VDK header bytes as loaded
    bool m_dmkSingleDensity = false;        // DMK flag $40: FM bytes not doubled
};
