#pragma once

#include "libdevices/main/io_handler.h"
#include <cstdint>
#include <string>
#include <vector>

namespace trs80 {

/**
 * The Radio Shack hard disk interface: a WD1010 at ports $C8-$CF and the
 * board's registers at $C0 (write protect switches, read) and $C1
 * (control: 4 software reset, 3 device enable, 2 wait enable), for up to
 * four drives. The register layout and the images follow xtrs (Tim Mann;
 * trs_hard.c, MIT-style licence); mmsim's own code.
 *
 * WD1010 registers: $C8 data, $C9 error (read) / precomp (write), $CA
 * sector count, $CB sector, $CC/$CD cylinder low/high, $CE size/drive/head
 * (bits 4-3 the drive, 2-0 the head), $CF status (read: 7 busy, 6 ready,
 * 5 write fault, 4 seek complete, 3 DRQ, 1 command in progress, 0 error)
 * / command (write: $1x restore, $2x read, $3x write, $4x verify, $5x
 * format, $6x init, $7x seek; bit 2 of read/write: multiple sectors).
 * Sectors are 256 bytes, 32 a track (as Radio Shack's driver, RSHARD,
 * formats them); commands finish at once (busy is never seen).
 *
 * Images are Matthew Reed's hard disk format (`.hdv`): a 256-byte header
 * ($56 $CB, version, checksum...; byte 7 bit 7 write protected, byte 28
 * the cylinders, 29 the sectors a cylinder) and the sectors in order of
 * cylinder, head, sector. Writes go back to the file (on eject, at exit,
 * or flush()). `create()` makes a
 * blank image (formatted by the DOS: LDOS's or LS-DOS's HDFORMAT).
 */
class HardDisk : public IOHandler {
public:
    ~HardDisk() override;
    const char* name() const override { return "Hard disk"; }
    uint32_t baseAddr() const override { return 0xC0; }
    uint32_t addrMask() const override { return 0; }
    bool ioRead(IBus*, uint32_t, uint8_t*) override { return false; }
    bool ioWrite(IBus*, uint32_t, uint8_t) override { return false; }
    void reset() override;
    void tick(uint64_t) override {}
    /** Unit 1-4 = hard drive 0-3; `.hdv` images. */
    bool mountDisk(int unit, const std::string& path) override;
    void ejectDisk(int unit) override;
    bool getDiskStatus(int unit, int& track, int& sector, bool& led) override;
    std::string getMountedDiskPath(int unit) override;
    void getDeviceInfo(DeviceInfo& out) const override;

    /** The board fitted (else its ports read $FF). */
    void setFitted(bool on) { m_fitted = on; }
    bool fitted() const { return m_fitted; }
    /** A port $C0-$CF. */
    uint8_t in(uint8_t port);
    uint8_t peek(uint8_t port) const;
    void out(uint8_t port, uint8_t v);

    /** Mount an image's bytes (path empty: not saved). False if not a Reed image. */
    bool load(int drive, std::vector<uint8_t> image, const std::string& path, std::string* err = nullptr);
    /** The image of drive `d` (tests). */
    const std::vector<uint8_t>& image(int d) const { return m_drive[d & 3].image; }
    /** Save any written drive to its file. */
    void flush();
    /** A blank Reed image: `cyls` cylinders, `heads` heads of 32 sectors. */
    static std::vector<uint8_t> create(int cyls, int heads);

    /** Each drive's geometry as the header gives it. */
    struct Drive {
        std::vector<uint8_t> image;
        std::string path;
        bool writeProtected = false, dirty = false;
        int cyls = 0, heads = 0, secs = 32;
        bool present() const { return !image.empty(); }
    };
    const Drive& drive(int d) const { return m_drive[d & 3]; }

private:
    bool anyDrive() const;
    void command(uint8_t v);
    bool findSector();
    uint8_t dataIn();
    void dataOut(uint8_t v);
    void nextSector();

    bool m_fitted = false;
    Drive m_drive[4];
    uint8_t m_control = 0, m_data = 0, m_error = 0, m_secCount = 0, m_sector = 0;
    uint16_t m_cyl = 0;
    uint8_t m_sdh = 0, m_status = 0, m_command = 0;
    size_t m_offset = 0;            // the current sector's byte in the image
    int m_done = -1;                // bytes of the sector moved (-1: no transfer)
};

}  // namespace trs80
