#pragma once
#include "libdevices/main/io_handler.h"
#include "libdevices/main/device_info.h"
#include "libdevices/main/disk_file.h"
#include <cstdint>
#include <string>
#include <vector>

class SdCardDevice;

/**
 * MEGA65 / C65 F011 floppy controller ($D080-$D09F), drives 0 and 1.
 *
 * Follows xemu's f011_core.c: commands complete at once (BUSY clears before
 * the next instruction), and a read or write moves a whole 512-byte sector
 * between the disk and the controller's buffer at $FFD6C00 (the SD card
 * device's disk buffer + $C00), at the FDC buffer pointer.
 *
 * Each drive reads one of:
 *  - a host .d81 file attached as unit 8 (drive 0) or 9 (drive 1) with
 *    mountDisk (CLI `disk mount`, GUI drive pane, MCP), plain or as
 *    `.d81.gz` / a `.zip` (#233): held in memory, a plain file patched per
 *    sector, an archive saved once writes stop (diskfile::WriteBack), or
 *  - the D81 image HYPPO mounted inside the SD card image: $D68B bits
 *    0+1 / 3+4 (image enabled, present), start sector $D68C-$D68F /
 *    $D690-$D693. Bit 2 / 5 is set when the image may be written: the
 *    VHDL returns "not write protected" there, whatever the book's D0WP
 *    label suggests.
 * A host file takes precedence. Physical track 0-79, side 0-1 and sector
 * 1-10 map to offset track*10240 + side*5120 + (sector-1)*512.
 *
 * Registers: $D080 CONTROL, $D081 COMMAND, $D082 STATUS A, $D083 STATUS B,
 * $D084 TRACK, $D085 SECTOR, $D086 SIDE, $D087 DATA, $D088 CLOCK, $D089 STEP,
 * $D08A P CODE.
 */
class F011Fdc : public IOHandler {
public:
    explicit F011Fdc(SdCardDevice* sd);
    ~F011Fdc() override;

    const char* name() const override { return m_name.c_str(); }
    uint32_t    baseAddr() const override { return 0xD080; }
    uint32_t    addrMask() const override { return 0x1F; }
    void setName(const std::string& n) override { m_name = n; }
    void setBaseAddr(uint32_t) override {}

    void reset() override;
    bool ioRead (IBus* bus, uint32_t addr, uint8_t* val) override;
    /**
     * The registers as they are now: no media refresh, no pending command
     * run, no buffer pointer or flag change (a read of $D087 advances the
     * CPU pointer; ioRead also runs a command left BUSY).
     */
    bool ioPeek (IBus* bus, uint32_t addr, uint8_t* val) override;
    bool ioWrite(IBus* bus, uint32_t addr, uint8_t val) override;
    void tick(uint64_t) override;
    void getDeviceInfo(DeviceInfo& out) const override;

    /** Units 8 and 9 are drives 0 and 1: attach a host .d81 (800 KB). */
    bool mountDisk(int unit, const std::string& path) override;
    void ejectDisk(int unit) override;
    bool getDiskStatus(int unit, int& track, int& sector, bool& led) override;
    std::string getMountedDiskPath(int unit) override;

    /** The FDC buffer pointer (disk side), for $D689/$D688 reads. */
    uint16_t fdcPointer() const { return m_ptrFdc; }
    /** STATUS A of the selected drive (for $D689 bit 1). */
    uint8_t statusA() const { return m_drv[m_drive & 1].statA; }

    static constexpr uint32_t D81_SIZE = 819200;

private:
    struct Drive {
        uint8_t statA = 0, statB = 0, headTrack = 0;
        bool haveDisk = false, canWrite = false;
        bool         attached = false;  // a host .d81
        std::vector<uint8_t> image;     // its contents
        std::string  path;
        bool         fileReadOnly = false;
        diskfile::WriteBack writeBack;  // an archive's pending save
        uint64_t     source = 0;    // which disk: 0 none, 1 + SD start sector, or a host file serial
        uint64_t     fileSerial = 0;
    };

    void refreshMedia();
    void saveImage(Drive& v);
    void setDisk(int d, bool haveDisk, bool canWrite);
    bool diskOffset(uint32_t& off) const;
    bool readDisk(int d, uint32_t off, uint8_t* buf);
    bool writeDisk(int d, uint32_t off, const uint8_t* buf);
    void readSector();
    void writeSector();
    void executeCommand();
    uint8_t* buffer() const;

    SdCardDevice* m_sd;
    std::string   m_name{"F011 FDC"};
    Drive    m_drv[2];
    int      m_drive = 0;           // selected drive (CONTROL bits 0-2; only 0 and 1 exist)
    uint8_t  m_control = 0, m_cmd = 0, m_track = 0, m_sector = 0, m_side = 0;
    uint8_t  m_clock = 0xFF, m_step = 0xFF;
    int      m_curCmd = -1;         // -1 none, 0x100 control write only
    uint16_t m_ptrCpu = 0, m_ptrFdc = 0;
    uint16_t m_swapMask = 0;
    uint64_t m_nextSerial = 1ull << 40;     // host-file sources, above any SD sector
};
