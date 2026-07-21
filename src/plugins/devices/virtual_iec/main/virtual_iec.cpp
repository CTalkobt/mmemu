#include "virtual_iec.h"
#include "include/util/logging.h"
#include "plugins/cbm-loader/main/d64_parser.h"
#include "plugins/cbm-loader/main/t64_parser.h"
#include "plugins/cbm-loader/main/g64_parser.h"
#include <fstream>
#include <iostream>
#include <iterator>
#include <cstdio>
#include <algorithm>
#include <cstdarg>

VirtualIECBus::VirtualIECBus(uint8_t deviceNumber)
    : m_deviceNumber(deviceNumber), m_state(IDLE), m_nextState(IDLE),
      m_isAddressed(false), m_expectAddressingByte(false), m_readyToSample(false),
      m_atnIn(false), m_clkIn(false), m_dataIn(false),
      m_clkOut(false), m_dataOut(false), m_lastClkIn(false),
      m_currentByte(0), m_bitCount(0), m_stateTimer(0), m_isLastByte(false), m_talkSubPhase(0),
      m_secondaryAddress(0), m_fileIdx(0), m_isSendingDir(false),
      m_track(0), m_sector(0), m_led(false)
{
}

uint8_t VirtualIECBus::readPort() {
    static uint64_t readCount = 0;
    readCount++;
    if (readCount == 1) {
        fprintf(stderr, "[VirtualIEC] readPort FIRST CALL!\n");
        fflush(stderr);
    }

    // IEC bus is active-low with inverting buffers on CIA2 input lines:
    //   Bus asserted (low) -> CIA reads 0; Bus released (high) -> CIA reads 1
    // Wired-OR: if either side asserts (pulls low), bus is low.
    bool clkBus = m_clkIn || m_clkOut;   // true = someone asserted CLK
    bool dataBus = m_dataIn || m_dataOut; // true = someone asserted DATA
    uint8_t val = 0;
    if (!clkBus) val |= (1 << 6);  // CLK released -> bit 6 high
    if (!dataBus) val |= (1 << 7); // DATA released -> bit 7 high
    if (m_atnIn) val |= (1 << 3);  // ATN echo (directly driven, not inverted)

    static uint8_t lastVal = 0xFF;
    if (readCount < 100 || (readCount % 10000 == 0)) {
        bool changed = (val != lastVal);
        fprintf(stderr, "[VirtualIEC] readPort #%llu: returned=$%02X clkBus=%d dataBus=%d atnIn=%d state=%d %s\n",
            (unsigned long long)readCount, val, clkBus, dataBus, m_atnIn, m_state,
            changed ? "CHANGED" : "stable");
        fflush(stderr);
        lastVal = val;
    }
    return val;
}

void VirtualIECBus::writePort(uint8_t val) {
    bool oldDataIn = m_dataIn;
    m_atnIn = (val >> 3) & 1;
    m_clkIn = (val >> 4) & 1;
    m_dataIn = (val >> 5) & 1;
    if (val != m_lastWriteVal) {
        m_lastWriteVal = val;
        fprintf(stderr, "[VirtualIEC] writePort: val=$%02X atn=%d clk=%d data=%d state=%d ph=%d timer=%lu\n",
            val, m_atnIn?1:0, m_clkIn?1:0, m_dataIn?1:0,
            m_state, m_talkSubPhase, (unsigned long)m_stateTimer);
    }
    if (m_dataIn != oldDataIn && (m_state == TALK_READY || m_state == TALK_FRAME)) {
        log("DATA_IN %d->%d state=%d ph=%d timer=%lu",
            oldDataIn?1:0, m_dataIn?1:0, m_state, m_talkSubPhase, (unsigned long)m_stateTimer);
    }
}

void VirtualIECBus::setDdr(uint8_t ddr) { (void)ddr; }

void VirtualIECBus::reset() {
    m_state = IDLE;
    m_nextState = IDLE;
    m_isAddressed = false;
    m_clkOut = false; m_dataOut = false;
    m_lastClkIn = false; m_currentByte = 0; m_bitCount = 0;
    m_stateTimer = 0; m_fileIdx = 0; m_isSendingDir = false; m_isLastByte = false; m_talkSubPhase = 0;
    m_filename.clear(); m_track = 0; m_sector = 0; m_led = false;
}

void VirtualIECBus::log(const char* fmt, ...) {
    char buf[256]; va_list args; va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args); va_end(args);
    fprintf(stderr, "[VirtualIEC] %s\n", buf);
    fflush(stderr);
    if (m_logger && m_logNamed) {
        m_logNamed(m_logger, SIM_LOG_INFO, buf);
    }
}

void VirtualIECBus::logDebug(const char* fmt, ...) {
    char buf[256]; va_list args; va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args); va_end(args);
    fprintf(stderr, "[VirtualIEC] %s\n", buf);
    fflush(stderr);
    if (m_logger && m_logNamed) {
        m_logNamed(m_logger, SIM_LOG_DEBUG, buf);
    }
}

void VirtualIECBus::tick(uint64_t cycles) {
    m_stateTimer += cycles;
    State oldState = m_state;
    switch (m_state) {
        case IDLE:
            if (m_atnIn) { m_state = ATTENTION; m_dataOut = true; m_stateTimer = 0; }
            break;
        case ATTENTION:
            // Device asserts DATA on entry (device present signal) and holds it.
            // Wait for CLK to be asserted (computer acknowledged), then transition
            // to ADDRESSING. DATA stays asserted — released only as byte ack.
            m_dataOut = true;
            if (m_clkIn && m_stateTimer > 20) {
                m_state = ADDRESSING;
                m_bitCount = 0; m_currentByte = 0;
                m_talkSubPhase = 0; // Phase 0: wait for CLK release
                m_readyToSample = false;
                m_lastClkIn = true;
                m_stateTimer = 0;
            }
            break;
        case ADDRESSING:
            // Receive bytes under ATN. Device holds DATA asserted throughout
            // the 8-bit transfer (KERNAL checks DATA at start of each bit
            // cycle and errors if not asserted). Byte ack = release DATA.
            if (!m_atnIn) { m_state = m_nextState; m_stateTimer = 0; break; }
            switch (m_talkSubPhase) {
            case 0: // Wait for CLK release (talker ready), then release DATA (ready for byte)
                if (!m_clkIn) {
                    m_dataOut = false; // Release DATA (listener ready for byte)
                    m_talkSubPhase = 1;
                    m_stateTimer = 0;
                }
                break;
            case 1: // Wait for CLK assert (byte start), with EOI detection
                if (m_clkIn) {
                    m_lastClkIn = true;
                    m_talkSubPhase = 2;
                } else if (m_stateTimer > 250) {
                    // EOI: CLK held released too long — ack by asserting DATA briefly
                    log("ADDRESSING EOI detected: timer=%lu clkIn=%d dataIn=%d",
                        (unsigned long)m_stateTimer, m_clkIn?1:0, m_dataIn?1:0);
                    m_dataOut = true; // Assert DATA (EOI ack pulse)
                    m_talkSubPhase = 3;
                    m_stateTimer = 0;
                }
                break;
            case 2: // Bit sampling — device NOT driving DATA (talker uses DATA for bits)
                handleBitTransfer();
                if (m_bitCount == 8) {
                    processCommand(m_currentByte);
                    log("Processed CMD: %02X, nextState: %d", m_currentByte, m_nextState);
                    m_dataOut = true; // Assert DATA = byte acknowledge
                    m_state = ACKNOWLEDGE;
                    m_bitCount = 0; m_currentByte = 0;
                    m_readyToSample = false;
                    m_stateTimer = 0;
                }
                break;
            case 3: // EOI ack pulse — DATA asserted briefly, then release
                if (m_stateTimer > 60) {
                    m_dataOut = false; // Release DATA (done with EOI ack)
                    m_talkSubPhase = 4;
                }
                break;
            case 4: // Post-EOI: wait for CLK assert (byte start)
                if (m_clkIn) {
                    m_lastClkIn = true;
                    m_talkSubPhase = 2;
                }
                break;
            }
            break;
        case ACKNOWLEDGE:
            if (m_atnIn) {
                // ATN still active — prepare for next command byte
                if (m_stateTimer > 80) {
                    m_state = ADDRESSING;
                    m_bitCount = 0; m_currentByte = 0;
                    m_talkSubPhase = 0; // Wait for CLK release
                    m_readyToSample = false;
                    m_lastClkIn = m_clkIn;
                    m_stateTimer = 0;
                }
            } else if (m_stateTimer > 80) {
                // ATN released — transition to next state
                // DATA is already released (byte ack). LISTENING/TURNAROUND
                // will re-assert DATA when appropriate.
                m_state = m_nextState;
                m_bitCount = 0; m_currentByte = 0;
                m_readyToSample = false;
                m_stateTimer = 0;
                m_talkSubPhase = 0;
            }
            break;
        case LISTENING:
            // Receive bytes without ATN — same protocol as ADDRESSING.
            // Hold DATA asserted during bits, release for ack.
            if (m_atnIn) { m_state = ATTENTION; m_dataOut = true; m_stateTimer = 0; break; }
            switch (m_talkSubPhase) {
            case 0: // Wait for CLK release (talker ready), then release DATA (ready for byte)
                if (!m_clkIn) {
                    m_dataOut = false; // Release DATA (listener ready for byte)
                    m_talkSubPhase = 1;
                    m_stateTimer = 0;
                }
                break;
            case 1: // Wait for CLK assert (byte start), with EOI detection
                if (m_clkIn) {
                    m_lastClkIn = true;
                    m_talkSubPhase = 2;
                } else if (m_stateTimer > 250) {
                    // EOI: CLK held released too long — ack by asserting DATA briefly
                    log("LISTENING EOI detected: timer=%lu clkIn=%d dataIn=%d",
                        (unsigned long)m_stateTimer, m_clkIn?1:0, m_dataIn?1:0);
                    m_dataOut = true; // Assert DATA (EOI ack pulse)
                    m_talkSubPhase = 3;
                    m_stateTimer = 0;
                }
                break;
            case 2: // Bit sampling — device NOT driving DATA
                handleBitTransfer();
                if (m_bitCount == 8) {
                    handleByteReceived(m_currentByte);
                    log("Received Byte: %02X", m_currentByte);
                    m_dataOut = true; // Assert DATA = byte acknowledge
                    m_bitCount = 0; m_currentByte = 0;
                    m_readyToSample = false;
                    m_state = ACKNOWLEDGE; // Hold ACK via ACKNOWLEDGE (same as ADDRESSING)
                    m_stateTimer = 0;
                }
                break;
            case 3: // EOI ack pulse — DATA asserted briefly, then release
                if (m_stateTimer > 60) {
                    m_dataOut = false; // Release DATA (done with EOI ack)
                    m_talkSubPhase = 4;
                }
                break;
            case 4: // Post-EOI: wait for CLK assert (byte start)
                if (m_clkIn) {
                    m_lastClkIn = true;
                    m_talkSubPhase = 2;
                }
                break;
            }
            break;
        case TURNAROUND:
            if (m_atnIn) { m_state = ATTENTION; m_dataOut = true; m_clkOut = false; m_stateTimer = 0; break; }
            if (!m_clkIn) {
                m_clkOut = true;
                m_dataOut = false;
                m_state = TALK_READY;
                m_talkSubPhase = 0;
                m_stateTimer = 0;
                m_bytesSent = 0;
            }
            break;
        case TALK_READY:
            if (m_atnIn) { m_state = ATTENTION; m_dataOut = true; m_clkOut = false; m_stateTimer = 0; break; }
            switch (m_talkSubPhase) {
            case 0: { // Step 0: hold CLK, wait for listener DATA true + Tbb
                m_clkOut = true;
                m_dataOut = false;
                if (m_dataIn && m_stateTimer >= 100) {
                    log("TALK_READY ph0->1: bytesSent=%d timer=%lu dataIn=%d",
                        m_bytesSent, (unsigned long)m_stateTimer, m_dataIn?1:0);
                    m_clkOut = false; // Step 1: release CLK (ready to send)
                    m_talkSubPhase = 1;
                    m_stateTimer = 0;
                }
                break;
            }
            case 1: { // Step 2: wait for listener to release DATA (ready for data)
                if (m_stateTimer == 1) {
                    log("TALK_READY ph1 START: dataIn=%d clkIn=%d clkOut=%d dataOut=%d byte#%d",
                        m_dataIn?1:0, m_clkIn?1:0, m_clkOut?1:0, m_dataOut?1:0, m_bytesSent);
                }
                if (!m_dataIn) {
                    m_currentByte = getNextByte();
                    m_isLastByte = (m_fileIdx >= m_fileBuffer.size());
                    log("TALK_READY ph1->TALKING: byte=%02X idx=%zu/%zu last=%d bytesSent=%d timer=%lu",
                        m_currentByte, m_fileIdx, m_fileBuffer.size(), m_isLastByte?1:0, m_bytesSent, (unsigned long)m_stateTimer);
                    if (m_isLastByte) {
                        m_talkSubPhase = 2;
                        m_stateTimer = 0;
                    } else {
                        m_state = TALKING;
                        m_bitCount = 0;
                        m_clkOut = true;
                        m_dataOut = !((m_currentByte >> 0) & 1);
                        m_stateTimer = 0;
                    }
                }
                if (m_stateTimer == 1001) {
                    log("TALK_READY ph1 TIMEOUT: dataIn=%d clkIn=%d atn=%d timer=%lu bytesSent=%d",
                        m_dataIn?1:0, m_clkIn?1:0, m_atnIn?1:0, (unsigned long)m_stateTimer, m_bytesSent);
                }
                break;
            }
            case 2: // EOI: hold CLK released >200us
                if (m_stateTimer >= 250) {
                    m_talkSubPhase = 3;
                }
                break;
            case 3: // Wait for listener EOI ack (DATA pulled)
                if (m_dataIn) {
                    m_talkSubPhase = 4;
                    m_stateTimer = 0;
                }
                break;
            case 4: // Wait for listener to release DATA after EOI ack
                if (!m_dataIn) {
                    m_state = TALKING;
                    m_bitCount = 0;
                    m_clkOut = true;
                    m_dataOut = !((m_currentByte >> 0) & 1);
                    m_stateTimer = 0;
                }
                break;
            }
            break;
        case TALKING:
            if (m_atnIn) { m_state = ATTENTION; m_dataOut = true; m_clkOut = false; m_stateTimer = 0; break; }
            if (m_clkOut) {
                if (m_stateTimer > 60) {
                    m_clkOut = false; m_stateTimer = 0;
                }
            } else {
                if (m_stateTimer > 60) {
                    m_bitCount++;
                    if (m_bitCount == 8) {
                        m_state = TALK_FRAME;
                        m_clkOut = true;
                        m_dataOut = false;
                        m_stateTimer = 0;
                    } else {
                        m_clkOut = true; m_dataOut = !((m_currentByte >> m_bitCount) & 1);
                        m_stateTimer = 0;
                    }
                }
            }
            break;
        case TALK_FRAME:
            if (m_atnIn) { m_state = ATTENTION; m_dataOut = true; m_clkOut = false; m_stateTimer = 0; break; }
            if (m_dataIn) {
                m_bytesSent++;
                log("TALK_FRAME: byte ack #%d, isLast=%d", m_bytesSent, m_isLastByte?1:0);
                if (m_isLastByte) {
                    m_state = IDLE;
                    m_clkOut = false;
                    m_dataOut = false;
                } else {
                    m_state = TALK_READY;
                    m_talkSubPhase = 0;
                    m_stateTimer = 0;
                }
            } else if (m_stateTimer > 1000) {
                log("Frame handshake timeout in TALK_FRAME");
                m_state = ERROR;
            }
            break;
        default: break;
    }
    if (m_state != oldState && (m_state != TALKING)) log("State: %d -> %d", oldState, m_state);
    m_led = (m_state != IDLE);
}

void VirtualIECBus::handleBitTransfer() {
    if (m_lastClkIn && !m_clkIn) {
        // IEC bus: DATA asserted (bit5=1) = logic 0, DATA released (bit5=0) = logic 1
        m_currentByte = (m_currentByte >> 1) | (m_dataIn ? 0x00 : 0x80);
        m_bitCount++;
    }
    m_lastClkIn = m_clkIn;
}

void VirtualIECBus::handleByteReceived(uint8_t byte) {
    if (m_secondaryAddress == 0 || m_secondaryAddress == 1) m_filename += (char)byte;
}

uint8_t VirtualIECBus::getNextByte() {
    return (m_fileIdx < m_fileBuffer.size()) ? m_fileBuffer[m_fileIdx++] : 0;
}

void VirtualIECBus::processCommand(uint8_t cmd) {
    // Global commands first (exact match — must precede LISTEN/TALK range checks
    // since UNLISTEN $3F falls in the LISTEN range and UNTALK $5F in TALK range)
    if (cmd == 0x3F) { // UNLISTEN
        m_nextState = IDLE;
        m_isAddressed = false;
        return;
    }
    if (cmd == 0x5F) { // UNTALK
        m_nextState = IDLE;
        m_isAddressed = false;
        m_clkOut = false;
        return;
    }

    uint8_t action = cmd & 0xE0;

    if (action == 0x20) { // LISTEN ($20-$3E)
        uint8_t device = cmd & 0x1F;
        if (device == m_deviceNumber) {
            m_isAddressed = true;
            m_nextState = LISTENING;
            m_filename.clear();
        } else {
            m_isAddressed = false;
        }
    } else if (action == 0x40) { // TALK ($40-$5E)
        uint8_t device = cmd & 0x1F;
        if (device == m_deviceNumber) {
            m_isAddressed = true;
            m_nextState = TURNAROUND;
            if (m_filename == "$" || m_filename == "*") generateDirectoryListing();
            else if (!m_filename.empty()) {
                std::string ext = m_mountedPath.substr(m_mountedPath.find_last_of('.') + 1);
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                ICbmDiskImage* p = (ext == "d64") ? (ICbmDiskImage*)new D64Parser() : (ext == "t64") ? (ICbmDiskImage*)new T64Parser() : (ext == "g64") ? (ICbmDiskImage*)new G64Parser() : nullptr;
                if (p && p->open(m_mountedPath)) p->readFile(m_filename, m_fileBuffer);
                m_fileIdx = 0; delete p;
            }
        } else {
            m_isAddressed = false;
        }
    } else if ((cmd & 0xF0) == 0xF0) { // OPEN ($F0-$FF)
        if (m_isAddressed) {
            m_secondaryAddress = cmd & 0x0F;
            m_filename.clear();
        }
    } else if ((cmd & 0xF0) == 0xE0) { // CLOSE ($E0-$EF)
        if (m_isAddressed) {
            m_secondaryAddress = cmd & 0x0F;
            m_fileBuffer.clear();
            m_fileIdx = 0;
            m_filename.clear();
        }
    } else if (action == 0x60) { // SECOND ($60-$7F)
        if (m_isAddressed) {
            m_secondaryAddress = cmd & 0x0F;
        }
    }
}

bool VirtualIECBus::mountDisk(int unit, const std::string& path) {
    if (unit != m_deviceNumber) return false;
    m_mountedPath = path; return true;
}

void VirtualIECBus::ejectDisk(int unit) { if (unit == m_deviceNumber) { m_mountedPath.clear(); m_fileBuffer.clear(); } }
bool VirtualIECBus::getDiskStatus(int unit, int& t, int& s, bool& led) {
    if (unit != m_deviceNumber) return false;
    t = m_mountedPath.empty() ? 0 : 18;
    s = 0;
    led = m_led;
    return true;
}
std::string VirtualIECBus::getMountedDiskPath(int unit) { return (unit == m_deviceNumber) ? m_mountedPath : ""; }

void VirtualIECBus::generateDirectoryListing() {
    m_fileBuffer.clear(); m_fileIdx = 0; m_isSendingDir = true;

    // Load address header
    m_fileBuffer.push_back(0x01);
    m_fileBuffer.push_back(0x08);

    // Helper to add a BASIC line: stores placeholder for link, line number, data, terminator
    auto addLine = [this](uint16_t lineNum, const std::string& lineData) {
        size_t linkPos = m_fileBuffer.size();
        m_fileBuffer.push_back(0x00); // Link lo (placeholder)
        m_fileBuffer.push_back(0x00); // Link hi (placeholder)
        m_fileBuffer.push_back(lineNum & 0xFF); // Line number lo
        m_fileBuffer.push_back(lineNum >> 8);   // Line number hi
        for (char c : lineData) {
            m_fileBuffer.push_back((uint8_t)c);
        }
        m_fileBuffer.push_back(0x00); // Line terminator
        return linkPos;
    };

    // File entries and disk info
    std::vector<CbmDirEntry> entries;
    std::string diskName;
    std::string diskId = "00";
    uint16_t freeBlocks = 0;

    if (!m_mountedPath.empty()) {
        std::string ext = m_mountedPath.substr(m_mountedPath.find_last_of('.') + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        ICbmDiskImage* p = (ext == "d64") ? (ICbmDiskImage*)new D64Parser() : (ext == "t64") ? (ICbmDiskImage*)new T64Parser() : (ext == "g64") ? (ICbmDiskImage*)new G64Parser() : nullptr;
        if (p && p->open(m_mountedPath)) {
            entries = p->getDirectory();
            diskName = p->getDiskName();
            diskId = p->getDiskId();
            freeBlocks = p->getFreeBlocks();
        }
        delete p;
    } else {
	freeBlocks = 9999;
    }

    // Pad disk name to 16 characters
    if (diskName.empty()) diskName = "UNNAMED";
    while (diskName.length() < 16) diskName += ' ';

    // Header line with disk name (RVS ON, disk name in quotes, 2-char ID, 2-digit disk num)
    std::string header = "\x12\"";
    header += diskName;
    header += "\"";
    if (diskId.length() >= 2) {
        header.push_back((uint8_t)diskId[0]);
        header.push_back((uint8_t)diskId[1]);
    } else {
        header.push_back('0');
        header.push_back('0');
    }
    addLine(0, header);

    for (const auto& e : entries) {
        std::string fileEntry;

        // Format block count as ASCII decimal (right-aligned in field)
        std::string blockStr = std::to_string(e.sizeBlocks);
        // Right-align in 5 characters
        while (blockStr.length() < 5) {
            blockStr = " " + blockStr;
        }
        fileEntry += blockStr;
        fileEntry += " \"";

        // Filename (max 16 chars, padded with spaces)
        std::string n = e.filename;
        if (n.length() > 16) n = n.substr(0, 16);
        fileEntry += n;

        // Pad to 16 characters
        while (n.length() < 16) {
            fileEntry += ' ';
            n += ' ';
        }
        fileEntry += "\" PRG";

        // Use block count as line number (as per Commodore convention)
        addLine(e.sizeBlocks, fileEntry);
    }

    // Footer line  
    addLine(freeBlocks, "BLOCKS FREE.             ");

    // Now fix up the link pointers
    size_t pos = 2; // Start after load address
    while (pos < m_fileBuffer.size()) {
        // Check if we have space for link and line number
        if (pos + 4 > m_fileBuffer.size()) break;

        // Find end of this line (the 0x00 terminator)
        size_t lineEnd = pos + 4; // Skip link + line number
        while (lineEnd < m_fileBuffer.size() && m_fileBuffer[lineEnd] != 0x00) {
            lineEnd++;
        }

        if (lineEnd >= m_fileBuffer.size()) break; // Didn't find terminator

        // Next line starts after the terminator
        size_t nextLinePos = lineEnd + 1;

        // Check if there's another line after this one
        if (nextLinePos + 4 > m_fileBuffer.size()) {
            // This is the last line, set link to 0x00 0x00
            m_fileBuffer[pos] = 0x00;
            m_fileBuffer[pos + 1] = 0x00;
            break;
        }

        // Calculate address of next line in BASIC memory
        // Buffer index nextLinePos maps to address 0x0801 + nextLinePos
        uint16_t nextAddr = 0x0801 + (uint16_t)nextLinePos;
        m_fileBuffer[pos] = nextAddr & 0xFF;
        m_fileBuffer[pos + 1] = nextAddr >> 8;

        // Move to next line
        pos = nextLinePos;
    }
}

void VirtualIECBus::getDeviceInfo(DeviceInfo& out) const {
    out.name = "VirtualIEC";
    out.baseAddr = 0;
    out.addrMask = 0;

    const char* stateStr = "UNKNOWN";
    switch (m_state) {
        case IDLE: stateStr = "IDLE"; break;
        case ATTENTION: stateStr = "ATTENTION"; break;
        case ADDRESSING: stateStr = "ADDRESSING"; break;
        case ACKNOWLEDGE: stateStr = "ACKNOWLEDGE"; break;
        case LISTENING: stateStr = "LISTENING"; break;
        case TURNAROUND: stateStr = "TURNAROUND"; break;
        case TALK_READY: stateStr = "TALK_READY"; break;
        case TALKING: stateStr = "TALKING"; break;
        case TALK_FRAME: stateStr = "TALK_FRAME"; break;
        case ERROR: stateStr = "ERROR"; break;
    }
    out.state.push_back({"State", stateStr});
    out.state.push_back({"Device Number", std::to_string(m_deviceNumber)});
    out.state.push_back({"Is Addressed", m_isAddressed ? "yes" : "no"});
    
    out.state.push_back({"ATN In", m_atnIn ? "LO (asserted)" : "HI (released)"});
    out.state.push_back({"CLK In", m_clkIn ? "LO" : "HI"});
    out.state.push_back({"DATA In", m_dataIn ? "LO" : "HI"});
    out.state.push_back({"CLK Out", m_clkOut ? "LO" : "HI"});
    out.state.push_back({"DATA Out", m_dataOut ? "LO" : "HI"});

    if (!m_mountedPath.empty()) {
        out.state.push_back({"Mounted Disk", m_mountedPath});
        out.state.push_back({"Track", std::to_string(m_track)});
        out.state.push_back({"Sector", std::to_string(m_sector)});
        out.state.push_back({"Activity LED", m_led ? "ON" : "OFF"});
    } else {
        out.state.push_back({"Mounted Disk", "none"});
    }
}
