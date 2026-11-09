#pragma once

#include "libdevices/main/io_handler.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace apple2 {

/**
 * A 5.25" disk image (AppleWin DiskImageHelper.cpp):
 * - .dsk/.do (DOS order), .po (ProDOS order): 35 or more tracks of 16
 *   sectors, nibblized a track at a time as DOS 3.3 formats it (volume 254);
 * - .nib: 6656 nibbles a track, as recorded;
 * - .woz (WOZ1, WOZ2): the bit stream of each quarter track (TMAP, TRKS),
 *   with its length in bits and the optimal bit timing (INFO).
 */
class Disk2Image {
public:
    enum class Order { ProDOS, DOS, Nib, Woz };
    static constexpr int TRACK_BYTES = 4096;        // 16 x 256
    static constexpr int NIB_TRACK = 0x1A00;        // 6656 nibbles
    static constexpr int MAX_NIBBLES = 0x1A18;
    static constexpr int WOZ_EMPTY_TRACK = 6400;    // an unrecorded track: noise
    static constexpr int QUARTER_TRACKS = 160;

    /** Open `path`; the format from the extension and contents. */
    static std::unique_ptr<Disk2Image> open(const std::string& path, std::string* err = nullptr);
    /** A disk from bytes (tests); `ext` picks the format (".dsk", ".po", ".nib", ".woz"). */
    static std::unique_ptr<Disk2Image> fromBytes(std::vector<uint8_t> data, const std::string& ext,
                                                 std::string* err = nullptr);

    int tracks() const { return m_tracks; }
    Order order() const { return m_order; }
    bool isWoz() const { return m_order == Order::Woz; }
    bool writeProtected() const { return m_protect; }
    void setWriteProtected(bool p) { m_protect = p; }
    const std::string& path() const { return m_path; }
    const std::vector<uint8_t>& data() const { return m_data; }
    /** The largest track in bytes (the drive's track buffer). */
    int maxTrackBytes() const { return m_maxTrackBytes; }
    /** WOZ: a bit cell in 125 ns units (32 = 4 us). */
    int optimalBitTiming() const { return m_bitTiming; }
    /** WOZ: INFO's disk type, 1 = 5.25", 2 = 3.5" (1 for the other formats). */
    int diskType() const { return m_diskType; }
    /** WOZ: the disk boots with the 13-sector ROM (DOS 3.2). */
    bool bootSector13() const { return m_boot13; }

    /** The track's nibbles into `out` (maxTrackBytes()), the count returned. */
    int readTrack(int track, uint8_t* out, bool skew = true);
    /** Nibbles back into the image (and its file). */
    void writeTrack(int track, const uint8_t* nibbles, int count);

    /** WOZ: quarter track `qt`'s bits into `out`; the byte count returned,
     *  `bitCount` set. An unmapped quarter track reads as noise. */
    int wozReadTrack(int qt, uint8_t* out, uint32_t& bitCount) const;
    /** WOZ: `bytes` of bits back to quarter track `qt` (a new track is added
     *  for an unmapped one); the file is rewritten with its CRC. */
    void wozWriteTrack(int qt, const uint8_t* bits, int bytes);

    // The 6-and-2 code (Code62/Decode62), for the tests
    static void nibblizeSector(const uint8_t* sector, uint8_t* out343);
    static void denibblizeSector(const uint8_t* in343, uint8_t* sector);
    /** CRC-32 (the WOZ header's). */
    static uint32_t crc32(const uint8_t* p, size_t n);

private:
    int nibblize(int track, uint8_t* out) const;
    void denibblize(const uint8_t* nibbles, int count, uint8_t* trackOut) const;
    bool parseWoz(std::string* err);
    void saveFile(size_t at, size_t len);

    std::vector<uint8_t> m_data;
    std::string m_path;
    Order m_order = Order::DOS;
    int m_tracks = 0;
    bool m_protect = false;
    int m_maxTrackBytes = MAX_NIBBLES;
    // WOZ
    bool m_woz1 = false, m_boot13 = false;
    int m_diskType = 1;
    int m_bitTiming = 32;
    size_t m_tmap = 0, m_trks = 0, m_trksHdr = 0;
    bool m_trksLast = false;
};

/**
 * The Disk II interface card (AppleWin Disk.cpp, "authentic" speed): the P5
 * boot ROM (16 sectors, or 13 for a DOS 3.2 WOZ in drive 1) at $Cn00, the
 * switches at $C080+n*16: stepper phases 0-3 ($0-$7), motor ($8/$9), drive
 * 1/2 ($A/$B), the data latch ($C), write-protect sense/load ($D),
 * read/write mode ($E/$F).
 *
 * .dsk/.nib disks pass a nibble every 32 cycles; reads in quick succession
 * see a partial nibble. WOZ disks run bit by bit through the sequencer: a
 * bit cell per optimal bit timing, the MC3470's head window (fake bits after
 * three zeroes), the latch held 7 us after a nibble, quarter-track stepping,
 * writes shifted out a bit at a time.
 */
class Disk2Card : public IOHandler {
public:
    static constexpr int SPINNING_CYCLES = 1000 * 1000;   // the motor runs on ~1 s after off
    static constexpr int MOTOR_ON_UNTIL_LSS_STABLE_CYCLES = 0x2EC;

    /** `rom`: the 16-sector boot ROM; `rom13`: the 13-sector one (may be empty). */
    explicit Disk2Card(std::vector<uint8_t> rom, std::vector<uint8_t> rom13 = {});
    ~Disk2Card() override;

    const char* name() const override { return "Disk II"; }
    uint32_t baseAddr() const override { return 0; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void reset() override { reset(true); }
    void tick(uint64_t) override {}
    /** After each instruction: the bus clock `now`, `cycles` since the last. */
    void update(uint64_t now, uint64_t cycles);

    /** RESET (the switches off); a power cycle also stops the drives. */
    void reset(bool powerCycle);

    /** $C0n0-$C0nF at cycle `now`: the value (even addresses: the latch),
     *  or -1 for the floating bus. `wr` set for a write. */
    int io(uint16_t addr, const uint8_t* wr, uint64_t now);
    uint8_t rom(uint8_t offset) const { return (m_use13 ? m_rom13 : m_rom)[offset]; }

    // Drives 1 and 2 are units 1 and 2
    bool mountDisk(int unit, const std::string& path) override;
    void ejectDisk(int unit) override;
    bool getDiskStatus(int unit, int& track, int& sector, bool& led) override;
    std::string getMountedDiskPath(int unit) override;

    bool insert(int drive, std::unique_ptr<Disk2Image> image);
    Disk2Image* image(int drive) { return m_drive[drive & 1].disk.image.get(); }
    int phase(int drive) const { return m_drive[drive & 1].phase; }
    /** The head in quarter tracks (WOZ stops between half tracks). */
    int quarterTrack(int drive) const { return m_drive[drive & 1].quarter; }
    bool motorOn() const { return m_motorOn; }
    int currentDrive() const { return m_curr; }
    bool rom13() const { return m_use13; }

private:
    struct Floppy {
        std::unique_ptr<Disk2Image> image;
        std::vector<uint8_t> track;
        int nibbles = 0;
        int byte = 0;
        bool trackData = false, dirty = false;
        // WOZ bit stream
        uint32_t bitOffset = 0, bitCount = 0, initialBitOffset = 0;
        uint8_t bitMask = 0x80;
        double extraCycles = 0;
        int revs = 0;
        int longestSyncStart = -1, longestSyncRun = 0;
    };
    struct Drive {
        Floppy disk;
        int phase = 0;              // half tracks 0-79
        int quarter = 0;            // quarter tracks (phase * 2, +-1 between)
        int spinning = 0;
        int writeLight = 0;
        uint64_t motorOnCycle = 0;
        uint8_t headWindow = 0;
    };
    enum { READ_SEQ = 0, SHIFT_WRITE = 1, CHECK_WP = 2, LOAD_WRITE = 3 };

    bool woz() const;
    int seqFunction() const { return (m_writeMode ? 1 : 0) | (m_loadMode ? 2 : 0); }
    void checkSpinning(bool stateChanged, uint64_t now);
    void controlMotor(uint16_t addr, uint64_t now);
    void controlStepper(uint16_t addr, uint64_t now);
    void stepperDeferred();
    void enable(uint16_t addr, uint64_t now);
    void readWrite(uint64_t now);
    void loadWriteProtect(uint64_t now);
    void setSequencer(uint16_t addr, uint64_t now);
    void readTrack(int drive, uint64_t now);
    void flush(int drive);
    void syncStepper(uint64_t now);
    void emptyLatch(const Drive& d, uint64_t now);
    void chooseRom();
    // WOZ
    uint32_t bitCellDelta(uint64_t now);
    void advanceBits(Floppy& f, uint32_t cells);
    void bitOffsets(Floppy& f);
    void incBit(Floppy& f);
    void resetSequencer();
    void latchWoz(bool write, uint64_t now);
    void readBitsWoz(uint32_t cells);
    void loadWriteWoz(uint32_t cells);
    void shiftWriteWoz(uint64_t now);
    void findTrackSeam(Floppy& f);
    void preJitter(int phase);
    void jitter(int phase, Floppy& f);
    void seamJitter(int quarter, Floppy& f);
    bool chance(int num, int den);

    std::vector<uint8_t> m_rom, m_rom13;
    bool m_use13 = false;
    Drive m_drive[2];
    int m_curr = 0;
    bool m_motorOn = false;
    int m_magnets = 0;
    bool m_writeMode = false, m_loadMode = false;
    uint8_t m_latch = 0;
    uint64_t m_lastCycle = 0, m_lastReadLatch = 0, m_now = 0;
    // A stepper change takes effect 10 cycles later, so two adjacent
    // magnets turned off in quick succession don't move the head (GH#1110)
    bool m_deferred = false;
    uint16_t m_deferredAddr = 0;
    uint64_t m_deferredAt = 0;
    uint32_t m_rand = 0x1234567;
    // The logic state sequencer (WOZ)
    uint8_t m_shiftReg = 0;
    int m_latchDelay = 0;
    bool m_writeStarted = false;
    int m_t00Idx = 0;
    bool m_t00Found = false;
};

}  // namespace apple2
