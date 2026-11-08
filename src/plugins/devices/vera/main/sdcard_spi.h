#pragma once

// An SD card in SPI mode on VERA's SPI port (Commander X16): an image file
// (raw, e.g. FAT32) answers the commands the X16's DOS uses. A `.gz` or
// `.zip` image (#233) is held in memory and saved once writes stop.
//
// Ported from x16-emulator src/sdcard.c, Copyright (c) 2019 Michael Steil
// and contributors, 2-clause BSD license.

#include "libdevices/main/disk_file.h"
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

class SdCardSpi {
public:
    /** Open an image (read/write if possible). False if it cannot be opened. */
    bool attach(const std::string& path);
    void detach();
    ~SdCardSpi() { detach(); }
    bool attached() const { return m_file.is_open() || m_archive; }
    /** Call often: saves an archived image once writes have stopped. */
    void poll() { if (m_writeBack.due()) save(); }
    const std::string& path() const { return m_path; }
    bool readOnly() const { return m_readOnly; }

    /** Slave select asserted: start a new command. */
    void select(bool on);
    /** One byte exchanged: `in` from the host, the result from the card. */
    uint8_t handle(uint8_t in);
    /** Blocks read and written (for status displays). */
    uint32_t lastLba() const { return m_lba; }

private:
    void respondR1();
    void respondCsd();
    int loadBlock(uint8_t* dest);
    void save();

    bool m_archive = false;             // a .gz / .zip image, in m_mem
    std::vector<uint8_t> m_mem;
    diskfile::WriteBack m_writeBack;

    std::fstream m_file;
    std::string  m_path;
    uint64_t m_size = 0;
    bool m_readOnly = false;

    uint8_t m_rx[3 + 512] = {};
    int m_rxIdx = 0;
    uint32_t m_lba = 0;
    uint8_t m_lastCmd = 0;
    bool m_acmd = false, m_idle = true, m_initialized = false, m_multiRead = false;
    bool m_selected = false;

    const uint8_t* m_resp = nullptr;
    int m_respLen = 0, m_respPos = 0;
    uint8_t m_r1 = 0;
    uint8_t m_csd[21] = {};
    uint8_t m_readBlock[2 + 512 + 2] = {};
    uint8_t m_nextBlock[1 + 512 + 2] = {};
};
