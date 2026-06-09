#include "sdcard.h"
#include <cstring>
#include <cstdio>

// Status flag bits
static constexpr uint8_t SD_ST_BUSY0     = 0x01;
static constexpr uint8_t SD_ST_BUSY1     = 0x02;
static constexpr uint8_t SD_ST_RESET     = 0x04;
static constexpr uint8_t SD_ST_MAPPED    = 0x08;
static constexpr uint8_t SD_ST_SDHC      = 0x10;
static constexpr uint8_t SD_ST_FSM_ERROR = 0x20;
static constexpr uint8_t SD_ST_ERROR     = 0x40;
static constexpr uint8_t SD_ST_EXT_BUS   = 0x80;

SdCardDevice::SdCardDevice(uint32_t base)
    : m_base(base), m_status(0), m_bufferMapped(false),
      m_sdhcMode(true), m_imageSize(0) {
    std::memset(m_regs, 0, sizeof(m_regs));
    std::memset(m_sectorBuf, 0, sizeof(m_sectorBuf));
}

void SdCardDevice::reset() {
    std::memset(m_regs, 0, sizeof(m_regs));
    std::memset(m_sectorBuf, 0, sizeof(m_sectorBuf));
    m_status = SD_ST_SDHC;  // SDHC mode enabled by default
    m_bufferMapped = false;
    m_sdhcMode = true;
}

bool SdCardDevice::ioRead(IBus* /*bus*/, uint32_t addr, uint8_t* val) {
    uint32_t offset = addr - m_base;

    // Sector buffer at $DE00-$DFFF
    if (addr >= 0xDE00 && addr <= 0xDFFF) {
        return bufferRead(addr, val);
    }

    if (offset >= 20) return false;

    if (offset == 0x00) {
        // Status register
        *val = m_status;
    } else {
        *val = m_regs[offset];
    }
    return true;
}

bool SdCardDevice::ioWrite(IBus* /*bus*/, uint32_t addr, uint8_t val) {
    uint32_t offset = addr - m_base;

    // Sector buffer at $DE00-$DFFF
    if (addr >= 0xDE00 && addr <= 0xDFFF) {
        return bufferWrite(addr, val);
    }

    if (offset >= 20) return false;

    m_regs[offset] = val;

    if (offset == 0x00) {
        // Command register — execute command
        executeCommand(val);
    }

    return true;
}

void SdCardDevice::executeCommand(uint8_t cmd) {
    // Clear error flags on any command
    m_status &= ~(SD_ST_ERROR | SD_ST_FSM_ERROR);

    switch (cmd) {
        case 0x00: // Reset
        case 0x10:
            m_status = SD_ST_RESET | SD_ST_SDHC;
            m_sdhcMode = true;
            // Clear sector address
            m_regs[0x01] = m_regs[0x02] = m_regs[0x03] = m_regs[0x04] = 0;
            break;

        case 0x01: // End reset
        case 0x11:
            m_status = SD_ST_SDHC;  // Clear reset flag, keep SDHC
            break;

        case 0x02: { // Read sector
            uint32_t sector = m_regs[0x01] | (m_regs[0x02] << 8) |
                              (m_regs[0x03] << 16) | (m_regs[0x04] << 24);
            if (!readSector(sector)) {
                m_status |= SD_ST_ERROR;
            }
            break;
        }

        case 0x03: { // Write sector
            uint32_t sector = m_regs[0x01] | (m_regs[0x02] << 8) |
                              (m_regs[0x03] << 16) | (m_regs[0x04] << 24);
            if (!writeSector(sector)) {
                m_status |= SD_ST_ERROR;
            }
            break;
        }

        case 0x40: // Reset bus
            m_status &= ~SD_ST_EXT_BUS;
            break;

        case 0x81: // Map buffer
            m_bufferMapped = true;
            m_status |= SD_ST_MAPPED;
            break;

        case 0x82: // Unmap buffer
            m_bufferMapped = false;
            m_status &= ~SD_ST_MAPPED;
            break;

        case 0xC0: // Select internal SD bus
            m_status &= ~SD_ST_EXT_BUS;
            break;

        case 0xC1: // Select external SD bus
            m_status |= SD_ST_EXT_BUS;
            break;

        case 0x53: { // Flash read — reads sector from SD image (same as cmd $02)
            uint32_t sector = m_regs[0x01] | (m_regs[0x02] << 8) |
                              (m_regs[0x03] << 16) | (m_regs[0x04] << 24);
            if (!readSector(sector)) {
                // If no image mounted, fill with $FF (xemu behavior)
                std::memset(m_sectorBuf, 0xFF, 512);
            }
            break;
        }

        case 0x57: // Write SD card config (ignored in emulation)
            break;

        default:
            // Unknown command — not an error, just ignored
            break;
    }
}

bool SdCardDevice::readSector(uint32_t sector) {
    if (!m_imageFile.is_open()) {
        // No image mounted — return zeros (emulates empty card)
        std::memset(m_sectorBuf, 0, 512);
        return true;
    }

    uint64_t byteOffset = (uint64_t)sector * 512;
    if (byteOffset + 512 > m_imageSize) {
        std::memset(m_sectorBuf, 0, 512);
        return false;
    }

    m_imageFile.seekg(byteOffset);
    m_imageFile.read((char*)m_sectorBuf, 512);
    return m_imageFile.good();
}

bool SdCardDevice::writeSector(uint32_t sector) {
    if (!m_imageFile.is_open()) return false;

    uint64_t byteOffset = (uint64_t)sector * 512;
    if (byteOffset + 512 > m_imageSize) return false;

    m_imageFile.seekp(byteOffset);
    m_imageFile.write((char*)m_sectorBuf, 512);
    m_imageFile.flush();
    return m_imageFile.good();
}

bool SdCardDevice::bufferRead(uint32_t addr, uint8_t* val) const {
    if (!m_bufferMapped) { *val = 0xFF; return true; }
    uint32_t off = addr - 0xDE00;
    if (off < 512) {
        *val = m_sectorBuf[off];
        return true;
    }
    *val = 0xFF;
    return true;
}

bool SdCardDevice::bufferWrite(uint32_t addr, uint8_t val) {
    if (!m_bufferMapped) return true;
    uint32_t off = addr - 0xDE00;
    if (off < 512) {
        m_sectorBuf[off] = val;
    }
    return true;
}

bool SdCardDevice::mountImage(const std::string& path) {
    unmount();
    m_imageFile.open(path, std::ios::in | std::ios::out | std::ios::binary);
    if (!m_imageFile.is_open()) {
        // Try read-only
        m_imageFile.open(path, std::ios::in | std::ios::binary);
    }
    if (!m_imageFile.is_open()) return false;

    m_imageFile.seekg(0, std::ios::end);
    m_imageSize = m_imageFile.tellg();
    m_imageFile.seekg(0);
    return true;
}

void SdCardDevice::unmount() {
    if (m_imageFile.is_open()) m_imageFile.close();
    m_imageSize = 0;
}

void SdCardDevice::getDeviceInfo(DeviceInfo& out) const {
    out.name = m_name;
    out.baseAddr = m_base;
    out.addrMask = addrMask();

    out.registers.push_back({"CMD/STATUS", 0x00, m_status, "Status flags / command"});
    out.registers.push_back({"ADDR0", 0x01, m_regs[0x01], "Sector addr byte 0"});
    out.registers.push_back({"ADDR1", 0x02, m_regs[0x02], "Sector addr byte 1"});
    out.registers.push_back({"ADDR2", 0x03, m_regs[0x03], "Sector addr byte 2"});
    out.registers.push_back({"ADDR3", 0x04, m_regs[0x04], "Sector addr byte 3"});

    out.state.push_back({"Mounted", m_imageFile.is_open() ? "yes" : "no"});
    out.state.push_back({"Image Size", std::to_string(m_imageSize) + " bytes"});
    out.state.push_back({"Buffer Mapped", m_bufferMapped ? "yes" : "no"});
    out.state.push_back({"SDHC Mode", m_sdhcMode ? "yes" : "no"});
}
