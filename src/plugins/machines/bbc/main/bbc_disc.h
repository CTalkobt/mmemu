#pragma once

#include "libdevices/main/io_handler.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace bbc {

/**
 * An Acorn DFS disc image: .ssd (one side) or .dsd (two sides, the tracks
 * of side 0 and side 1 alternating), 10 sectors of 256 bytes a track,
 * 40 or 80 tracks (from the size; short images are padded). Writes go back
 * to the file.
 */
class DfsImage {
public:
    static constexpr int SECTORS = 10;
    static constexpr int SECTOR_SIZE = 256;
    static constexpr int TRACK_SIZE = SECTORS * SECTOR_SIZE;

    static std::unique_ptr<DfsImage> open(const std::string& path, std::string* err = nullptr);
    /** From bytes (tests); `doubleSided` for .dsd. */
    static std::unique_ptr<DfsImage> fromBytes(std::vector<uint8_t> data, bool doubleSided);

    int sides() const { return m_sides; }
    int tracks() const { return m_tracks; }
    bool writeProtected() const { return m_protect; }
    void setWriteProtected(bool p) { m_protect = p; }
    const std::string& path() const { return m_path; }
    const std::vector<uint8_t>& data() const { return m_data; }

    /** The sector's offset in the image, or -1 if there is none. */
    long offset(int track, int side, int sector) const;
    uint8_t read(long at) const { return m_data[(size_t)at]; }
    void write(long at, uint8_t v);
    /** Write changed bytes back to the file. */
    void flush();

private:
    std::vector<uint8_t> m_data;
    std::string m_path;
    int m_sides = 1, m_tracks = 80;
    bool m_protect = false, m_dirty = false;
};

/**
 * The Intel 8271 floppy disc controller at &FE80-&FE9F, after b-em
 * (i8271.c and the sector-image poll of sdf-acc.c): command, parameter,
 * status, result, reset and data registers; the commands DFS uses: read
 * and write (one 128-byte sector or several), verify, read ID, format,
 * seek, specify, drive status and the special registers. Data moves a byte
 * at a time by NMI (non-DMA): the 8271 raises its interrupt for each byte
 * and the NMI line follows it. Drives 0 and 1 (units 0 and 1); the side
 * comes from the drive control output (DFS drives 2 and 3).
 *
 * Timing as b-em: a byte every 272 cycles, seeks at the specified step
 * time, the motor off some revolutions after the last command.
 */
class I8271 : public IOHandler {
public:
    std::function<void(bool)> onNmi;

    I8271();
    ~I8271() override;
    const char* name() const override { return "8271 FDC"; }
    uint32_t baseAddr() const override { return 0; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void reset() override;
    void tick(uint64_t) override {}

    uint8_t read(uint16_t addr);
    void write(uint16_t addr, uint8_t v);
    /** Run `cycles` 2 MHz cycles. */
    void clock(uint64_t cycles);

    bool mountDisk(int unit, const std::string& path) override;
    void ejectDisk(int unit) override;
    bool getDiskStatus(int unit, int& track, int& sector, bool& led) override;
    std::string getMountedDiskPath(int unit) override;

    bool insert(int drive, std::unique_ptr<DfsImage> image);
    DfsImage* image(int drive) { return m_drive[drive & 1].image.get(); }
    int track(int drive) const { return m_drive[drive & 1].track; }
    bool motorOn() const { return m_motorOn; }
    uint8_t status() const { return m_status; }

private:
    struct Drive {
        std::unique_ptr<DfsImage> image;
        int track = 0;              // the head
    };
    enum State { IDLE, NOTFOUND, READSECTOR, WRITESECTOR, READADDR, FORMAT };

    void nmi();
    void spinUp();
    void spinDown();
    void setSpinDown();
    void seek();
    void prepOp(int sectors, int sectorSize, bool deleted);
    void driveStatus();
    void command();                 // all parameters in
    void callback();                // fdc_time ran out
    void poll();                    // a byte time
    void fail(uint8_t result);      // result with interrupt
    void data(uint8_t v);           // a byte read
    int getData(bool last);         // a byte to write, or -1
    void finishRead();
    void startRead(int sector);
    void startWrite(int sector);
    int side() const { return (m_drvout & 0x20) ? 1 : 0; }

    Drive m_drive[2];
    int m_cur = 0;
    // Registers (b-em's i8271 struct)
    uint8_t m_command = 0xFF, m_params[5] = {};
    int m_paramNum = 0, m_paramReq = 0;
    uint8_t m_status = 0, m_result = 0, m_data = 0, m_drvout = 0;
    int m_curTrack[2] = {};
    int m_curSector = 0, m_sectorsLeft = 0, m_sectorSize = 0, m_bytesLeft = 0;
    bool m_deleted = false;
    int m_phase = 0;
    bool m_written = false;
    uint32_t m_stepTime = 0, m_settleTime = 0;
    int m_unloadRevs = 0;
    // Timing (2 MHz cycles)
    bool m_motorOn = false;
    int64_t m_fdcTime = 0, m_motorSpin = 0;
    int m_pollTime = 0, m_pollDiv = 0, m_posn = 0;
    // The image side of b-em's sdf-acc
    State m_state = IDLE;
    int m_count = 0;
    long m_at = 0;
    int m_idTrack = 0, m_idSector = 0, m_formatSectors = 0, m_formatByte = 0;
    bool m_index = false;
};

}  // namespace bbc
