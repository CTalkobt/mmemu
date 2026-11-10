#include "spi_sd_card.h"
#include <cstring>

namespace spectrum {

bool SpiSdCard::insert(const std::string& path, std::string& err) {
    eject();
    m_file.open(path, std::ios::in | std::ios::out | std::ios::binary);
    if (!m_file.is_open()) { err = "can't open the SD card image '" + path + "'"; return false; }
    m_file.seekg(0, std::ios::end);
    const std::streamoff size = m_file.tellg();
    if (size < 512 * 1024 || size % 512) {
        m_file.close();
        err = "an SD card image is a multiple of 512 bytes, 512 KB or more";
        return false;
    }
    m_sectors = (uint32_t)(size / 512);
    // Capacity = (C_SIZE + 1) x 512 KB (an SDHC CSD)
    m_cSize = (uint16_t)((m_sectors >> 10) - 1);
    m_path = path;
    m_blocksRead = m_blocksWritten = 0;
    reset();
    return true;
}

void SpiSdCard::eject() {
    if (m_file.is_open()) m_file.close();
    m_path.clear();
    m_sectors = 0;
    reset();
}

void SpiSdCard::reset() {
    m_r1 = IDLE;
    m_state = CMD;
    m_respLen = m_respPos = 0;
    m_cmd8 = m_cmd55 = false;
}

uint8_t SpiSdCard::read() {
    return m_respPos < m_respLen ? m_resp[m_respPos++] : 0xFF;
}

void SpiSdCard::write(uint8_t b) {
    if (!inserted()) return;
    switch (m_state) {
        case CMD:
            if ((b & 0xC0) == 0x40) {               // start bit 0, transmitter bit 1
                m_cmd = b & 0x3F;
                m_state = ARG0;
            }
            break;
        case ARG0: case ARG1: case ARG2: case ARG3:
            m_arg[m_state - ARG0] = b;
            m_state = (State)(m_state + 1);
            break;
        case CRC:
            command();
            m_r1 &= IDLE;
            m_state = m_cmd == 24 ? DATA_TOKEN : CMD;
            break;
        case DATA_TOKEN:
            if (b == 0xFE) { m_state = DATA; m_sendCount = 0; }
            break;
        case DATA:
            m_send[m_sendCount++] = b;
            if (m_sendCount == 512) m_state = DATA_CRC1;
            break;
        case DATA_CRC1: m_state = DATA_CRC2; break;
        case DATA_CRC2:
            writeBlock();
            m_state = CMD;
            break;
    }
}

void SpiSdCard::r7(uint32_t v) {
    m_resp[0] = m_r1;
    m_resp[1] = (uint8_t)(v >> 24);
    m_resp[2] = (uint8_t)(v >> 16);
    m_resp[3] = (uint8_t)(v >> 8);
    m_resp[4] = (uint8_t)v;
    m_respLen = 5;
    m_respPos = 0;
}

void SpiSdCard::command() {
    if (m_cmd55) {                                  // an application command
        m_cmd55 = false;
        switch (m_cmd) {
            case 13: case 22: case 23: case 42: case 51:
                m_r1 |= ILLEGAL;
                r1();
                return;
            case 41:                                // SD_SEND_OP_COND
                if (m_cmd8 && (m_arg[0] & 0x40)) m_r1 &= (uint8_t)~IDLE;
                r1();
                return;
            default: break;                         // a standard command after all
        }
    }
    switch (m_cmd) {
        case 0:                                     // GO_IDLE_STATE
            m_r1 |= IDLE;
            m_cmd8 = false;
            r1();
            break;
        case 8:                                     // SEND_IF_COND: voltage accepted, the check pattern
            m_cmd8 = true;
            r7(0x00000100u | m_arg[3]);
            break;
        case 9: csd(); break;
        case 10: cid(); break;
        case 17: readBlock(); break;
        case 24: r1(); break;                       // WRITE_BLOCK: the data follows
        case 32: case 33: case 38: r1(); break;     // erases: accepted, nothing erased
        case 55:                                    // APP_CMD
            m_cmd55 = true;
            r1();
            break;
        case 58: r7(0xC0000000u); break;            // READ_OCR: powered up, SDHC
        default:
            m_r1 |= ILLEGAL;
            r1();
            break;
    }
}

void SpiSdCard::readBlock() {
    if (m_r1 & IDLE) { m_r1 |= ILLEGAL; r1(); return; }
    const uint32_t sector = argument();
    if (sector >= m_sectors) { m_r1 |= PARAM_ERROR; r1(); return; }
    m_file.clear();
    m_file.seekg((std::streamoff)sector * 512);
    m_file.read((char*)&m_resp[2], 512);
    if (!m_file) {
        m_resp[0] = m_r1;
        m_resp[1] = 0x01;                           // data error token
        m_respLen = 2;
        m_respPos = 0;
        return;
    }
    m_resp[0] = m_r1;
    m_resp[1] = 0xFE;                               // data token
    m_resp[514] = m_resp[515] = 0;                  // CRC
    ++m_blocksRead;
    m_respLen = 516;
    m_respPos = 0;
}

void SpiSdCard::writeBlock() {
    if (m_r1 & IDLE) { m_r1 |= ILLEGAL; r1(); return; }
    const uint32_t sector = argument();
    if (sector >= m_sectors) { m_r1 |= PARAM_ERROR; r1(); return; }
    m_file.clear();
    m_file.seekp((std::streamoff)sector * 512);
    m_file.write((const char*)m_send, 512);
    m_file.flush();
    ++m_blocksWritten;
    m_resp[0] = 0x05;                               // data accepted
    m_resp[1] = 0x01;                               // busy over
    m_respLen = 2;
    m_respPos = 0;
}

void SpiSdCard::csd() {
    if (m_r1 & IDLE) { m_r1 |= ILLEGAL; r1(); return; }
    m_resp[0] = m_r1;
    m_resp[1] = 0xFE;
    uint8_t* c = &m_resp[2];
    std::memset(c, 0, 16);
    c[0] = 0x40;                                    // CSD version 2.0
    c[4] = 0x5B;                                    // command classes
    c[5] = 0x05 | 0x09;                             // READ_BL_LEN 9
    c[8] = (uint8_t)(m_cSize >> 8);
    c[9] = (uint8_t)m_cSize;
    c[10] = 0x40 | 0x3F;                            // ERASE_BLK_EN, SECTOR_SIZE
    c[11] = 0x80;
    c[12] = 0x10;                                   // WRITE_BL_LEN 9
    c[13] = 0x01;
    c[15] = 0x01;
    m_resp[18] = m_resp[19] = 0;
    m_respLen = 20;
    m_respPos = 0;
}

void SpiSdCard::cid() {
    if (m_r1 & IDLE) { m_r1 |= ILLEGAL; r1(); return; }
    m_resp[0] = m_r1;
    m_resp[1] = 0xFE;
    uint8_t* c = &m_resp[2];
    std::memset(c, 0, 16);
    std::memcpy(c + 1, "  ", 2);                   // OEM
    std::memcpy(c + 3, "mmem", 4);                  // product name
    c[15] = 0x01;
    m_resp[18] = m_resp[19] = 0;
    m_respLen = 20;
    m_respPos = 0;
}

}  // namespace spectrum
