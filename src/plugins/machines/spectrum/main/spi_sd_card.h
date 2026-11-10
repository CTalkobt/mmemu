#pragma once

#include <cstdint>
#include <fstream>
#include <string>

namespace spectrum {

/**
 * An SD card (SDHC) in SPI mode, after libspectrum's mmc.c (Fuse's DivMMC
 * and ZXMMC): a byte out gives a byte back.
 *
 * - Commands: GO_IDLE_STATE (0), SEND_IF_COND (8), SEND_CSD (9), SEND_CID
 *   (10), READ_SINGLE_BLOCK (17), WRITE_BLOCK (24), APP_CMD (55), READ_OCR
 *   (58), and ACMD41 (SD_SEND_OP_COND: out of idle once CMD8 came first and
 *   HCS is set). Others answer R1 with "illegal command"; erases are
 *   accepted without erasing.
 * - Addresses are block numbers (SDHC). The image is a raw card (MBR and
 *   partitions, or a bare FAT volume), any multiple of 512 bytes; writes
 *   go straight to the file.
 */
class SpiSdCard {
public:
    /** An image file; false with `err` set if it can't be opened. */
    bool insert(const std::string& path, std::string& err);
    void eject();
    bool inserted() const { return m_file.is_open(); }
    const std::string& path() const { return m_path; }
    uint32_t sectors() const { return m_sectors; }
    /** Blocks read and written since inserted. */
    uint64_t blocksRead() const { return m_blocksRead; }
    uint64_t blocksWritten() const { return m_blocksWritten; }

    /** Card reset (power on). */
    void reset();
    /** The next byte of the card's response ($FF when it has none). */
    uint8_t read();
    /** A byte to the card. */
    void write(uint8_t b);

private:
    enum State { CMD, ARG0, ARG1, ARG2, ARG3, CRC, DATA_TOKEN, DATA, DATA_CRC1, DATA_CRC2 };
    static constexpr uint8_t IDLE = 0x01, ERASE_RESET = 0x02, ILLEGAL = 0x04, PARAM_ERROR = 0x40;

    void command();
    void r1() { m_resp[0] = m_r1; m_respLen = 1; m_respPos = 0; }
    void r7(uint32_t v);
    void readBlock();
    void writeBlock();
    void csd();
    void cid();
    uint32_t argument() const {
        return (uint32_t)m_arg[0] << 24 | (uint32_t)m_arg[1] << 16 | (uint32_t)m_arg[2] << 8 | m_arg[3];
    }

    std::fstream m_file;
    std::string m_path;
    uint32_t m_sectors = 0;
    uint16_t m_cSize = 0;
    State m_state = CMD;
    uint8_t m_r1 = IDLE;
    uint8_t m_cmd = 0;
    uint8_t m_arg[4] = {};
    bool m_cmd8 = false, m_cmd55 = false;
    uint8_t m_send[512] = {};
    int m_sendCount = 0;
    uint8_t m_resp[516] = {};
    int m_respLen = 0, m_respPos = 0;
    uint64_t m_blocksRead = 0, m_blocksWritten = 0;
};

}  // namespace spectrum
