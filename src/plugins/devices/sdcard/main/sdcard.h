#pragma once
#include "libdevices/main/io_handler.h"
#include "libdevices/main/device_info.h"
#include <cstring>
#include <string>
#include <fstream>

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
 * Sector buffer: 512 bytes at $DE00-$DFFF (I/O mapped when SD_ST_MAPPED set)
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
    const uint8_t* sectorBuffer() const { return m_sectorBuf; }
    uint8_t* sectorBuffer() { return m_sectorBuf; }
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
    uint8_t m_sectorBuf[512];   // 512-byte sector buffer
    bool    m_bufferMapped;     // Buffer visible to CPU at $DE00
    bool    m_sdhcMode;         // SDHC addressing (sector-based, not byte-based)

    // SD card image
    std::fstream m_imageFile;
    uint64_t     m_imageSize;
};
