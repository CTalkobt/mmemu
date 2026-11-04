#pragma once
#include "libdevices/main/io_handler.h"
#include "libdevices/main/device_info.h"
#include <cstring>
#include <string>
#include <fstream>
#include <functional>

/**
 * MEGA65 SD Card Controller
 *
 * Register map (base $D680, 20 bytes):
 *
 *   $D680  CMD/STATUS  — Write: execute command; Read: status flags
 *   $D681  ADDR0       — Sector address byte 0 (LSB)
 *   $D682  ADDR1       — Sector address byte 1
 *   $D683  ADDR2       — Sector address byte 2
 *   $D684  ADDR3       — Sector address byte 3 (MSB)
 *   $D685  Reserved
 *   $D686  FILL        — Fill byte value for fill-mode writes
 *   $D687-$D688  Reserved
 *   $D689  BUFSEL      — Bit 7: 1=SD buffer, 0=FDC buffer
 *   $D68A  MOUNT0_INFO — Drive 0 mount info
 *   $D68B  MOUNT1_INFO — Drive 1 mount info
 *   $D68C-$D68F  MOUNT0_SEC — Drive 0 starting sector (32-bit LE)
 *   $D690-$D693  MOUNT1_SEC — Drive 1 starting sector (32-bit LE)
 *
 * Disk buffers: 4 KB at physical $FFD6000 (diskBuffer()): the F011 sector
 * buffer at +$C00, the SD card sector buffer at +$E00. Outside the
 * hypervisor, $DE00-$DFFF (when mapped by command $81) shows the buffer
 * $D689 bit 7 (BUFSEL) selects: 1 = SD card, 0 = F011; the hypervisor
 * always sees the SD card buffer there.
 *
 * $D68A-$D693 (disk image mount flags and start sectors) can only be
 * written from the hypervisor (MEGA65 book, F011 appendix).
 *
 * Commands:
 *   $00/$10 — Reset (clear sector regs, enable SDHC)
 *   $01/$11 — End reset (clear error flags)
 *   $02     — Read sector (512 bytes into buffer)
 *   $03     — Write sector (512 bytes from buffer)
 *   $40     — Reset bus (select SD bus)
 *   $81     — Map buffer (make accessible to CPU)
 *   $82     — Unmap buffer
 *   $C0     — Select internal SD bus
 *   $C1     — Select external SD bus
 */
class SdCardDevice : public IOHandler {
public:
    explicit SdCardDevice(uint32_t base = 0xD680);
    virtual ~SdCardDevice() = default;

    const char* name() const override { return m_name.c_str(); }
    uint32_t    baseAddr() const override { return m_base; }
    uint32_t    addrMask() const override { return 0x7F; }  // $D680-$D6FF

    void setName(const std::string& n) override { m_name = n; }
    void setBaseAddr(uint32_t a) override { m_base = a; }

    void reset() override;
    bool ioRead (IBus* bus, uint32_t addr, uint8_t* val) override;
    bool ioWrite(IBus* bus, uint32_t addr, uint8_t val) override;
    void tick(uint64_t) override {}
    void getDeviceInfo(DeviceInfo& out) const override;

    // Mount an SD card image file for sector read/write
    bool mountImage(const std::string& path);
    void unmount();
    bool isMounted() const { return m_imageFile.is_open(); }

    // Sector buffer access (for I/O mapping at $DE00-$DFFF)
    const uint8_t* sectorBuffer() const { return m_diskBuf + 0xE00; }
    uint8_t* sectorBuffer() { return m_diskBuf + 0xE00; }
    /** The 4 KB disk buffer area (physical $FFD6000-$FFD6FFF). */
    uint8_t* diskBuffer() { return m_diskBuf; }
    /** The F011 sector buffer ($FFD6C00). */
    uint8_t* fdcBuffer() { return m_diskBuf + 0xC00; }

    /** $D68B: per drive image enabled, present, writable (bits 0-2 / 3-5), MEGA image (6 / 7). */
    uint8_t  mountFlags() const { return m_regs[0x0B]; }
    /** Start sector of drive 0's or 1's disk image ($D68C / $D690). */
    uint32_t mountStartSector(int drive) const;
    /** Raw access to the SD card image (for the F011's disk images). */
    bool readImage(uint64_t offset, uint8_t* buf, size_t n);
    bool writeImage(uint64_t offset, const uint8_t* buf, size_t n);
    bool isWritable() const { return m_writable; }

    /** Hypervisor mode query (mount registers and buffer view). */
    void setHypervisorQuery(std::function<bool()> fn) { m_inHypervisor = std::move(fn); }
    /**
     * F011 state for $D688/$D689: bits 0-8 the FDC buffer pointer, bit 9 set
     * when a sector was read but not yet read by the CPU (EQ and DRQ).
     */
    void setFdcStatusQuery(std::function<uint16_t()> fn) { m_fdcStatus = std::move(fn); }
    bool isBufferMapped() const { return m_bufferMapped; }

    // Buffer I/O handler for $DE00-$DFFF region
    bool bufferRead(uint32_t addr, uint8_t* val) const;
    bool bufferWrite(uint32_t addr, uint8_t val);

private:
    void executeCommand(uint8_t cmd);
    bool readSector(uint32_t sector);
    bool writeSector(uint32_t sector);

    uint32_t m_base;
    std::string m_name{"SD Card"};

    // Status and registers
    uint8_t m_status;
    uint8_t m_regs[20];         // Register shadow $D680-$D693
    uint8_t m_diskBuf[4096];    // $FFD6000: F011 buffer at +$C00, SD buffer at +$E00
    bool    m_bufferMapped;     // Buffer visible to CPU at $DE00
    bool    m_sdhcMode;         // SDHC addressing (sector-based, not byte-based)

    // SD card image
    std::fstream m_imageFile;
    uint64_t     m_imageSize;
    bool         m_writable = false;

    std::function<bool()>    m_inHypervisor;
    std::function<uint16_t()> m_fdcStatus;
    bool inHypervisor() const { return m_inHypervisor ? m_inHypervisor() : true; }
};
