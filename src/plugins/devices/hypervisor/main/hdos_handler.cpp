#include "hdos_handler.h"
#include "plugins/45gs02/main/cpu45gs02.h"
#include "libmem/main/ibus.h"

#include <cstring>
#include <algorithm>
#include <filesystem>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>

namespace fs = std::filesystem;

HdosHandler::HdosHandler() {
    m_rootDir = ".";
    m_cwd = ".";
}

HdosHandler::~HdosHandler() {
    closeAll();
}

void HdosHandler::setRootDir(const std::string& path) {
    m_rootDir = path;
    if (!m_rootDir.empty() && m_rootDir.back() != '/') m_rootDir += '/';
    m_cwd = m_rootDir;
}

void HdosHandler::closeAll() {
    for (int i = 0; i < MAX_DESCRIPTORS; i++)
        closeDescriptor(i);
    m_currentDesc = -1;
}

int HdosHandler::allocateDescriptor() {
    for (int i = 0; i < MAX_DESCRIPTORS; i++) {
        if (m_descriptors[i].status == DESC_CLOSED)
            return i;
    }
    return -1;
}

void HdosHandler::closeDescriptor(int idx) {
    if (idx < 0 || idx >= MAX_DESCRIPTORS) return;
    auto& d = m_descriptors[idx];
    if (d.status == DESC_FILE && d.fd >= 0) {
        ::close(d.fd);
        d.fd = -1;
    } else if (d.status == DESC_DIR && d.dirp) {
        closedir(static_cast<DIR*>(d.dirp));
        d.dirp = nullptr;
    }
    d.status = DESC_CLOSED;
    d.basePath.clear();
}

void HdosHandler::setResult(MOS45GS02* cpu, uint8_t a, bool carry) {
    auto& h = cpu->hyperState();
    h.regA = a;
    if (carry) h.pflags |= 0x01;
    else       h.pflags &= ~0x01;
}

void HdosHandler::setResultRegs(MOS45GS02* cpu, uint8_t a, uint8_t x, uint8_t y, uint8_t z, bool carry) {
    auto& h = cpu->hyperState();
    h.regA = a;
    h.regX = x;
    // Y is at offset 0x02 but hyperState doesn't have regY separate — use regX/regZ
    // Actually from the hypervisor_regs.cpp, offset 0x02 is Y. But hyperState struct
    // may not have regY. Let's set via the D640 registers directly if we have the bus.
    h.regZ = z;
    if (carry) h.pflags |= 0x01;
    else       h.pflags &= ~0x01;
}

// ---------------------------------------------------------------------------
// Main dispatch
// ---------------------------------------------------------------------------

bool HdosHandler::handleTrap(uint8_t func, MOS45GS02* cpu) {
    // Per xemu: switch on func >> 1 for contiguous case values
    switch (func >> 1) {
        case 0x00 >> 1: // $00: get version — don't virtualize, let HYPPO answer
            return false;

        case 0x02 >> 1: // $02: get default drive
        case 0x04 >> 1: // $04: get current drive
            virtGetDrive(cpu);
            return true;

        case 0x06 >> 1: // $06: select drive
            virtSelectDrive(cpu);
            return true;

        case 0x08 >> 1: { // $08: getdisksize
            // Return a dummy disk size to avoid entering uninitialized HYPPO.
            // MEGA65 D81 images are 819200 bytes = 3200 sectors of 256 bytes.
            auto& h = cpu->hyperState();
            h.regA = 0x00;  // sectors low
            h.regX = 0x0C;  // sectors mid (3200 = $0C80)
            h.regY = 0x80;  // sectors high
            h.regZ = 0x00;
            h.pflags |= 0x01; // carry = success
            return true;
        }

        case 0x0C >> 1: // $0C: chdir
            virtCd(cpu);
            return true;

        case 0x12 >> 1: // $12: opendir
            virtOpenDir(cpu);
            return true;

        case 0x14 >> 1: // $14: readdir
            virtReadDir(cpu);
            return true;

        case 0x16 >> 1: // $16: closedir/closefile
        case 0x20 >> 1: // $20: closefile
            virtCloseFileOrDir(cpu);
            return true;

        case 0x18 >> 1: // $18: openfile
            virtOpenFile(cpu);
            return true;

        case 0x1A >> 1: // $1A: readfile
            virtReadFile(cpu);
            return true;

        case 0x22 >> 1: // $22: closeall
            virtCloseAll(cpu);
            return true;

        case 0x2E >> 1: // $2E: setname — let HYPPO handle, capture in onHyppoLeave
            return false;

        case 0x34 >> 1: // $34: findfile
            virtFindFile(cpu);
            return true;

        case 0x36 >> 1: // $36: loadfile
            virtLoadFile(cpu, 0);
            return true;

        case 0x38 >> 1: // $38: get last error
            setResult(cpu, m_lastError, true);
            return true;

        case 0x3A >> 1: // $3A: set transfer area — let HYPPO handle, capture in onHyppoLeave
            return false;

        case 0x3C >> 1: // $3C: cdrootdir
            virtCdRoot(cpu);
            return true;

        case 0x3E >> 1: // $3E: loadfile to attic
            virtLoadFile(cpu, 0x8000000);
            return true;

        default:
            return false; // Not virtualized — let HYPPO handle
    }
}

void HdosHandler::onHyppoLeave(uint8_t func, MOS45GS02* cpu) {
    auto& h = cpu->hyperState();
    bool carry = (h.pflags & 0x01) != 0;

    if (func == 0x2E && carry) {
        // setname succeeded — capture the filename from HYPPO
        // Filename is at address (Y << 8) in user memory
        uint8_t inY = h.regX; // FIXME: need the input Y, not output
        // For now, read from the setname buffer that HYPPO stores
        // The filename was passed in Y (high byte) as a pointer.
        // We'll read it from the transfer area or directly.
        // Simplified: read from the CPU's data bus at the address HYPPO stored
        if (m_physBus) {
            // HYPPO copies filename to its internal buffer; we read from user space
            // The pointer was in input Y register (high byte), address = Y*256
            // Since we don't have the original input Y saved, we'll read from
            // a known location. HYPPO stores the current filename at $BD00.
            m_setnameFn.clear();
            for (int i = 0; i < 63; i++) {
                uint8_t ch = m_physBus->peek8(0xFFF0100 + i); // Hypervisor filename buffer
                if (ch == 0 || ch < 0x20 || ch >= 0x7F) break;
                m_setnameFn += (char)tolower(ch);
            }
        }
    }
    if (func == 0x3A && carry) {
        // Set transfer area — Y register has high byte of address
        m_transferArea = h.regX << 8; // FIXME: same issue with Y
    }
}

// ---------------------------------------------------------------------------
// Virtualized functions
// ---------------------------------------------------------------------------

void HdosHandler::virtGetDrive(MOS45GS02* cpu) {
    setResult(cpu, 0, true); // drive 0, success
}

void HdosHandler::virtSelectDrive(MOS45GS02* cpu) {
    auto& h = cpu->hyperState();
    if (h.regX != 0) {
        setResult(cpu, HDOSERR_NO_SUCH_DISK, false);
        m_lastError = HDOSERR_NO_SUCH_DISK;
    } else {
        setResult(cpu, 0, true);
    }
}

void HdosHandler::virtFindFile(MOS45GS02* cpu) {
    if (m_setnameFn.empty()) {
        setResult(cpu, HDOSERR_FILE_NOT_FOUND, false);
        m_lastError = HDOSERR_FILE_NOT_FOUND;
        return;
    }

    std::string fullPath = m_cwd + m_setnameFn;

    // Try case-insensitive match
    if (!fs::exists(fullPath)) {
        // Scan directory for case-insensitive match
        bool found = false;
        try {
            for (auto& entry : fs::directory_iterator(m_cwd)) {
                std::string name = entry.path().filename().string();
                std::string nameLower = name;
                std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);
                if (nameLower == m_setnameFn) {
                    fullPath = entry.path().string();
                    found = true;
                    break;
                }
            }
        } catch (...) {}
        if (!found) {
            setResult(cpu, HDOSERR_FILE_NOT_FOUND, false);
            m_lastError = HDOSERR_FILE_NOT_FOUND;
            return;
        }
    }

    m_fileFound = fullPath;
    setResult(cpu, 0, true);
    m_lastError = 0;
}

void HdosHandler::virtOpenFile(MOS45GS02* cpu) {
    if (m_fileFound.empty()) {
        setResult(cpu, HDOSERR_FILE_NOT_FOUND, false);
        m_lastError = HDOSERR_FILE_NOT_FOUND;
        return;
    }

    int idx = allocateDescriptor();
    if (idx < 0) {
        setResult(cpu, HDOSERR_TOO_MANY_OPEN, false);
        m_lastError = HDOSERR_TOO_MANY_OPEN;
        return;
    }

    int fd = ::open(m_fileFound.c_str(), O_RDONLY);
    if (fd < 0) {
        setResult(cpu, HDOSERR_FILE_NOT_FOUND, false);
        m_lastError = HDOSERR_FILE_NOT_FOUND;
        return;
    }

    m_descriptors[idx].status = DESC_FILE;
    m_descriptors[idx].fd = fd;
    m_descriptors[idx].basePath = m_cwd;
    m_currentDesc = idx;

    setResult(cpu, (uint8_t)idx, true);
    m_lastError = 0;
}

void HdosHandler::virtOpenDir(MOS45GS02* cpu) {
    int idx = allocateDescriptor();
    if (idx < 0) {
        setResult(cpu, HDOSERR_TOO_MANY_OPEN, false);
        m_lastError = HDOSERR_TOO_MANY_OPEN;
        return;
    }

    DIR* dirp = opendir(m_cwd.c_str());
    if (!dirp) {
        setResult(cpu, HDOSERR_CANNOT_OPEN_DIR, false);
        m_lastError = HDOSERR_CANNOT_OPEN_DIR;
        return;
    }

    m_descriptors[idx].status = DESC_DIR;
    m_descriptors[idx].dirp = dirp;
    m_descriptors[idx].basePath = m_cwd;

    setResult(cpu, (uint8_t)idx, true);
    m_lastError = 0;
}

void HdosHandler::virtReadFile(MOS45GS02* cpu) {
    if (m_currentDesc < 0 || m_currentDesc >= MAX_DESCRIPTORS ||
        m_descriptors[m_currentDesc].status != DESC_FILE) {
        setResult(cpu, HDOSERR_INVALID_DESC, false);
        m_lastError = HDOSERR_INVALID_DESC;
        return;
    }

    // Read up to 512 bytes into the sector buffer at $FFD6E00
    uint8_t buffer[512];
    int bytesRead = ::read(m_descriptors[m_currentDesc].fd, buffer, 512);
    if (bytesRead < 0) {
        setResult(cpu, HDOSERR_READ_ERROR, false);
        m_lastError = HDOSERR_READ_ERROR;
        return;
    }

    // Write to sector buffer in physical memory
    if (m_physBus) {
        for (int i = 0; i < bytesRead; i++)
            m_physBus->write8(0xFFD6E00 + i, buffer[i]);
    }

    // Return byte count in X (low) and Y (high)
    auto& h = cpu->hyperState();
    h.regA = 0;
    h.regX = bytesRead & 0xFF;
    // h.regY = (bytesRead >> 8) & 0xFF; // No regY in hyperState
    h.pflags |= 0x01; // carry = success
    m_lastError = 0;
}

void HdosHandler::virtReadDir(MOS45GS02* cpu) {
    auto& h = cpu->hyperState();
    int descIdx = h.regX; // descriptor from X register

    if (descIdx < 0 || descIdx >= MAX_DESCRIPTORS ||
        m_descriptors[descIdx].status != DESC_DIR) {
        setResult(cpu, HDOSERR_INVALID_DESC, false);
        m_lastError = HDOSERR_INVALID_DESC;
        return;
    }

    DIR* dirp = static_cast<DIR*>(m_descriptors[descIdx].dirp);
    struct dirent* entry = readdir(dirp);

    // Skip . and ..
    while (entry && (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0))
        entry = readdir(dirp);

    if (!entry) {
        // End of directory
        setResult(cpu, 0, false); // carry clear = no more entries
        return;
    }

    // Write directory entry to transfer area in memory
    if (m_physBus && m_transferArea) {
        // HDOS directory entry format (simplified):
        // Offset 0-63: filename (null-terminated)
        std::string name = entry->d_name;
        for (int i = 0; i < 64; i++) {
            m_physBus->write8(m_transferArea + i, (i < (int)name.size()) ? name[i] : 0);
        }
        // Get file size
        std::string fullPath = m_descriptors[descIdx].basePath + name;
        struct stat st;
        uint32_t fsize = 0;
        if (::stat(fullPath.c_str(), &st) == 0) fsize = st.st_size;
        // Offset 64-67: file size (32-bit LE)
        m_physBus->write8(m_transferArea + 64, fsize & 0xFF);
        m_physBus->write8(m_transferArea + 65, (fsize >> 8) & 0xFF);
        m_physBus->write8(m_transferArea + 66, (fsize >> 16) & 0xFF);
        m_physBus->write8(m_transferArea + 67, (fsize >> 24) & 0xFF);
        // Offset 68: file type (0=file, 0x10=dir)
        m_physBus->write8(m_transferArea + 68, (entry->d_type == DT_DIR) ? 0x10 : 0x00);
    }

    setResult(cpu, 0, true);
    m_lastError = 0;
}

void HdosHandler::virtCloseFileOrDir(MOS45GS02* cpu) {
    auto& h = cpu->hyperState();
    int descIdx = h.regX;

    if (descIdx < 0 || descIdx >= MAX_DESCRIPTORS ||
        m_descriptors[descIdx].status == DESC_CLOSED) {
        setResult(cpu, HDOSERR_INVALID_DESC, false);
        m_lastError = HDOSERR_INVALID_DESC;
        return;
    }

    closeDescriptor(descIdx);
    if (m_currentDesc == descIdx) m_currentDesc = -1;
    setResult(cpu, 0, true);
    m_lastError = 0;
}

void HdosHandler::virtCloseAll(MOS45GS02* cpu) {
    closeAll();
    setResult(cpu, 0, true);
    m_lastError = 0;
}

void HdosHandler::virtLoadFile(MOS45GS02* cpu, uint32_t addrBase) {
    if (m_setnameFn.empty()) {
        setResult(cpu, HDOSERR_FILE_NOT_FOUND, false);
        m_lastError = HDOSERR_FILE_NOT_FOUND;
        return;
    }

    // Find the file (case-insensitive)
    std::string fullPath = m_cwd + m_setnameFn;
    if (!fs::exists(fullPath)) {
        bool found = false;
        try {
            for (auto& entry : fs::directory_iterator(m_cwd)) {
                std::string name = entry.path().filename().string();
                std::string nameLower = name;
                std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);
                if (nameLower == m_setnameFn) {
                    fullPath = entry.path().string();
                    found = true;
                    break;
                }
            }
        } catch (...) {}
        if (!found) {
            setResult(cpu, HDOSERR_FILE_NOT_FOUND, false);
            m_lastError = HDOSERR_FILE_NOT_FOUND;
            return;
        }
    }

    // Load address from X (low), Y (high), Z (bank)
    auto& h = cpu->hyperState();
    uint32_t addr = addrBase + h.regX + ((uint32_t)h.regZ << 16);
    // Note: Y register would be bits 15:8 but hyperState doesn't expose it separately

    int fd = ::open(fullPath.c_str(), O_RDONLY);
    if (fd < 0) {
        setResult(cpu, HDOSERR_FILE_NOT_FOUND, false);
        m_lastError = HDOSERR_FILE_NOT_FOUND;
        return;
    }

    // Read and load into memory
    if (m_physBus) {
        uint8_t buf[1024];
        int total = 0;
        for (;;) {
            int n = ::read(fd, buf, sizeof(buf));
            if (n <= 0) break;
            for (int i = 0; i < n; i++) {
                m_physBus->write8(addrBase + ((addr + total + i) & 0xFFFFFF), buf[i]);
            }
            total += n;
        }
    }

    ::close(fd);
    setResult(cpu, 0, true);
    m_lastError = 0;
}

void HdosHandler::virtCd(MOS45GS02* cpu) {
    if (m_setnameFn.empty()) {
        setResult(cpu, HDOSERR_FILE_NOT_FOUND, false);
        m_lastError = HDOSERR_FILE_NOT_FOUND;
        return;
    }

    std::string target = m_cwd + m_setnameFn;
    if (!target.empty() && target.back() != '/') target += '/';

    if (fs::is_directory(target)) {
        m_cwd = target;
        setResult(cpu, 0, true);
        m_lastError = 0;
    } else {
        setResult(cpu, HDOSERR_FILE_NOT_FOUND, false);
        m_lastError = HDOSERR_FILE_NOT_FOUND;
    }
}

void HdosHandler::virtCdRoot(MOS45GS02* cpu) {
    m_cwd = m_rootDir;
    setResult(cpu, 0, true);
    m_lastError = 0;
}
