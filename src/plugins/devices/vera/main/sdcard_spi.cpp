// Ported from x16-emulator src/sdcard.c, Copyright (c) 2019 Michael Steil
// and contributors, 2-clause BSD license.

#include "sdcard_spi.h"
#include <cstring>

namespace {
enum : uint8_t {
    CMD0 = 0, CMD8 = 8, CMD9 = 9, CMD12 = 12, CMD13 = 13, CMD16 = 16, CMD17 = 17, CMD18 = 18,
    CMD24 = 24, CMD55 = 55, CMD58 = 58, ACMD41 = 0x80 | 41,
};
const uint8_t R2_INIT[] = {0x00, 0x00};
const uint8_t R2_UNINIT[] = {0x1F, 0xFF};
const uint8_t R3[] = {0xC0, 0xFF, 0x80, 0x00};
const uint8_t R7[] = {1, 0x00, 0x00, 0x01, 0xAA};
const uint8_t BAD_LBA[] = {0x00, 0x08};
}

bool SdCardSpi::attach(const std::string& path) {
    detach();
    m_file.open(path, std::ios::in | std::ios::out | std::ios::binary);
    m_readOnly = false;
    if (!m_file.is_open()) {
        m_file.open(path, std::ios::in | std::ios::binary);
        m_readOnly = true;
    }
    if (!m_file.is_open()) return false;
    m_file.seekg(0, std::ios::end);
    m_size = (uint64_t)m_file.tellg();
    m_path = path;
    m_initialized = false;
    m_idle = true;
    m_resp = nullptr;
    m_rxIdx = 0;
    return true;
}

void SdCardSpi::detach() {
    if (m_file.is_open()) m_file.close();
    m_path.clear();
    m_size = 0;
    m_resp = nullptr;
}

void SdCardSpi::select(bool on) {
    m_selected = on;
    m_rxIdx = 0;
}

void SdCardSpi::respondR1() {
    m_r1 = m_idle ? 1 : 0;
    m_resp = &m_r1;
    m_respLen = 1;
}

void SdCardSpi::respondCsd() {
    static const uint8_t base[21] = {
        0xFF, 0xFF, 0x00, 0xFF, 0xFE,               // dummies, R1, dummy, start block
        0x40, 0x0E, 0x00, 0x32, 0x5B, 0x59, 0x00,   // CSD v2
        0x00, 0x00, 0x00,                           // C_SIZE (set below)
        0x7F, 0x80, 0x0A, 0x40, 0x00, 0x01,
    };
    std::memcpy(m_csd, base, sizeof m_csd);
    uint64_t cSize = (m_size >> 19) - 1;            // 512 KB units
    m_csd[12] |= (uint8_t)((cSize >> 16) & 0x3F);
    m_csd[13] = (uint8_t)(cSize >> 8);
    m_csd[14] = (uint8_t)cSize;
    m_resp = m_csd;
    m_respLen = 21;
}

int SdCardSpi::loadBlock(uint8_t* dest) {
    dest[0] = 0xFE;                                 // data token
    if ((uint64_t)m_lba * 512 >= m_size) {
        dest[0] = 0x08;                             // error token: out of range
        return 1;
    }
    m_file.clear();
    m_file.seekg((std::streamoff)m_lba * 512);
    m_file.read((char*)&dest[1], 512);
    dest[513] = dest[514] = 0xFF;                   // CRC (not checked)
    return 1 + 512 + 2;
}

uint8_t SdCardSpi::handle(uint8_t in) {
    if (!m_selected || !m_file.is_open()) return 0xFF;
    uint8_t out = 0xFF;
    if (m_rxIdx == 0 && in == 0xFF) {
        if (m_resp) {
            out = m_resp[m_respPos++];
            if (m_respPos == m_respLen) {
                if (m_multiRead) {
                    m_lba++;
                    m_respLen = loadBlock(m_nextBlock);
                    if (m_respLen == 1) m_multiRead = false;
                    m_resp = m_nextBlock;
                    m_respPos = 0;
                } else {
                    m_resp = nullptr;
                    m_multiRead = false;
                }
            }
        }
        return out;
    }
    m_rx[m_rxIdx++] = in;
    if ((m_rx[0] & 0xC0) == 0x40 && m_rxIdx == 6) {
        m_rxIdx = 0;
        uint8_t cmd = m_rx[0] & 0x3F;
        if (m_acmd) { cmd |= 0x80; m_acmd = false; }
        m_lastCmd = cmd;
        uint32_t arg = ((uint32_t)m_rx[1] << 24) | ((uint32_t)m_rx[2] << 16) | ((uint32_t)m_rx[3] << 8) | m_rx[4];
        switch (cmd) {
            case CMD0:   m_idle = true; respondR1(); break;
            case CMD8:   m_resp = R7; m_respLen = sizeof R7; break;
            case CMD9:   respondCsd(); break;
            case ACMD41: m_idle = false; m_initialized = true; respondR1(); break;
            case CMD12:  m_multiRead = false; respondR1(); break;
            case CMD13:
                m_resp = m_initialized ? R2_INIT : R2_UNINIT;
                m_respLen = 2;
                break;
            case CMD18:
                m_multiRead = true;
                [[fallthrough]];
            case CMD17:
                m_lba = arg;
                m_readBlock[0] = 0;                 // R1
                m_respLen = 1 + loadBlock(&m_readBlock[1]);
                if (m_respLen == 2) m_multiRead = false;
                m_resp = m_readBlock;
                break;
            case CMD24:
                m_lba = arg;
                if ((uint64_t)m_lba * 512 >= m_size) { m_resp = BAD_LBA; m_respLen = 2; }
                else respondR1();
                break;
            case CMD55:  m_acmd = true; respondR1(); break;
            case CMD58:  m_resp = R3; m_respLen = sizeof R3; break;
            default:     respondR1(); break;
        }
        m_respPos = 0;
    } else if (m_rxIdx == 515) {
        // A data block after CMD24: token $FE, 512 bytes, CRC
        m_rxIdx = 0;
        if (m_lastCmd == CMD24 && m_rx[0] == 0xFE && !m_readOnly && (uint64_t)m_lba * 512 < m_size) {
            m_file.clear();
            m_file.seekp((std::streamoff)m_lba * 512);
            m_file.write((const char*)m_rx + 1, 512);
            m_file.flush();
        }
    }
    return out;
}
