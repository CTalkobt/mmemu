#pragma once

#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

/**
 * A PC/XT hard disk controller (the Western Digital WD1002-WX1 the Burke &
 * Burke CoCo XT manual recommends, or an equivalent: DTC 5150CRH, Adaptec
 * 2072...), as the CoCo XT interface puts one on the CoCo's cartridge bus
 * (#356).
 *
 * Four ports (the XT's 320h-323h; the CoCo XT manual's table, A.1.2):
 *  - 0 data (read and write): command blocks, data and the completion byte;
 *  - 1 read: status; write: force reset;
 *  - 2 read: the jumpers (0 here); write: start command (select);
 *  - 3 write: select mode (the DMA/interrupt mask: bit 1 interrupts, bit 0
 *    DMA; kept; the CoCo XT doesn't use the interrupt).
 *
 * Status: bit 0 REQ (a byte may be moved), bit 1 I/O (1 = controller to
 * host), bit 2 C/D (1 = command or completion byte), bit 3 BUSY (selected),
 * bit 5 interrupt pending (with the mask's bit 1, at completion).
 *
 * A command: write 2 (select), six bytes to 0 (the DCB: command; drive << 5
 * | head; cylinder bits 8-9 << 6 | sector (0-based); cylinder bits 0-7;
 * block count (0 = 256); control), then the data phase, then the
 * completion byte (bit 1 error, bit 5 the drive) from 0, after which the
 * controller is idle. Commands: $00 test drive ready, $01 recalibrate, $03
 * request sense (4 bytes: error code with bit 7 = address valid, then the
 * DCB's bytes 1-3 for the failing sector), $04 format drive, $05 verify, $06
 * format track, $07 format bad track, $08 read, $0A write, $0B seek, $0C
 * initialize drive characteristics (8 bytes: cylinders (2, big-endian),
 * heads, reduced write current cylinder (2), write precompensation
 * cylinder (2), ECC burst length), $0E read sector buffer, $0F write sector
 * buffer, $E0 RAM diagnostic, $E3 drive diagnostic, $E4 controller
 * diagnostic. Sectors are 512 bytes, 17 to a track.
 *
 * Drives 0 and 1 are raw image files (sector 0 first, CHS order: LBA =
 * (cylinder x heads + head) x 17 + sector). Geometry comes from $0C (as the
 * host's driver sets it) or else from the image: 4 heads and as many
 * cylinders as the file holds (setGeometry overrides). Writes go to the file.
 *
 * Sources: Burke & Burke's OS-9 boot ROM for the CoCo XT-RTC (disassembled,
 * CoCo addresses $C56A-$C6F8) drives the controller exactly this way: reset
 * ($FF51 write), mask 0 ($FF53), wait for status & $09 == 0, select ($FF52),
 * each DCB byte when status & $0F == $0D (and $0F there means the command
 * ended early: read the completion byte), read data when it's $0B, write
 * $0C's 8 bytes when it's $09, the completion byte when status & $07 ==
 * $07; on completion bit 1 it issues $03 and takes sense codes 0 and $18
 * (corrected ECC) as success. Its DCBs: command, drive/head (0), cylinder
 * bits 8-9 / sector, cylinder bits 0-7, count (1), control. It uses $00,
 * $01, $03, $08, $0B and $0C (8 bytes: cylinders, heads, reduced write
 * current, precompensation, ECC burst). The command codes, the DCB and $0C
 * layouts, the completion byte (bit 1 error, bit 5 drive) and the sense
 * bytes agree with MAME's src/devices/bus/isa/hdc.cpp. Unverified: the
 * other commands, the sense codes beyond those, bit 4 (MAME: DMA request)
 * and the jumper port. 17 sectors a track is the MFM controllers' format
 * (an RLL controller would use 26).
 */
class XtHdc {
public:
    static constexpr int SECTOR_SIZE = 512;
    static constexpr int SECTORS_PER_TRACK = 17;
    static constexpr uint8_t ST_REQ = 0x01;
    static constexpr uint8_t ST_IO = 0x02;
    static constexpr uint8_t ST_CD = 0x04;
    static constexpr uint8_t ST_BUSY = 0x08;
    static constexpr uint8_t ST_INT = 0x20;
    // Sense error codes
    static constexpr uint8_t ERR_NONE = 0x00;
    static constexpr uint8_t ERR_NOT_READY = 0x04;
    static constexpr uint8_t ERR_RECORD_NOT_FOUND = 0x14;
    static constexpr uint8_t ERR_INVALID_COMMAND = 0x20;
    static constexpr uint8_t ERR_ILLEGAL_ADDRESS = 0x21;
    static constexpr uint8_t ERR_WRITE_FAULT = 0x03;

    XtHdc();
    ~XtHdc();

    void reset();
    /** Ports 0-3. */
    uint8_t read(int port);
    uint8_t peek(int port) const;
    void write(int port, uint8_t v);

    bool mount(int drive, const std::string& path);
    void eject(int drive);
    std::string imagePath(int drive) const;
    /** Override a drive's geometry (cylinders, heads). */
    void setGeometry(int drive, int cylinders, int heads);
    int cylinders(int drive) const;
    int heads(int drive) const;
    /** The interrupt output (completion with interrupts enabled). */
    bool irq() const { return m_irqPending && (m_mask & 0x02); }
    uint8_t status() const;

private:
    enum class Phase { Idle, Command, DataIn, DataOut, Status };
    struct Drive {
        std::string path;
        std::unique_ptr<std::fstream> file;
        bool readOnly = false;
        uint64_t size = 0;
        int cylinders = 0, heads = 0;
        bool geometrySet = false;
    };

    void execute();
    void complete(uint8_t sense);
    bool loadSector();
    bool storeSector();
    bool advance();
    int64_t lba() const;
    void dataByteIn();

    Drive m_drives[2];
    Phase m_phase = Phase::Idle;
    uint8_t m_dcb[6] = {};
    int m_dcbPos = 0;
    std::vector<uint8_t> m_buf;
    size_t m_pos = 0;
    uint8_t m_completion = 0;
    uint8_t m_sense[4] = {};
    uint8_t m_mask = 0;
    bool m_irqPending = false;
    // The transfer in progress
    int m_drive = 0, m_head = 0, m_cyl = 0, m_sector = 0, m_blocks = 0;
    uint8_t m_sectorBuffer[SECTOR_SIZE] = {};
};
