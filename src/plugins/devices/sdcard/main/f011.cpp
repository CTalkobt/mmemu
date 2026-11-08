#include "f011.h"
#include "sdcard.h"
#include <algorithm>
#include <cstring>

// Status A bits
static constexpr uint8_t SA_BUSY = 0x80, SA_DRQ = 0x40, SA_EQ = 0x20, SA_RNF = 0x10,
                         SA_CRC = 0x08, SA_LOST = 0x04, SA_PROT = 0x02, SA_TK0 = 0x01;
// Status B bits
static constexpr uint8_t SB_RDREQ = 0x80, SB_WTREQ = 0x40, SB_RUN = 0x20,
                         SB_DSKIN = 0x08, SB_IRQ = 0x02, SB_DSKCHG = 0x01;

F011Fdc::F011Fdc(SdCardDevice* sd) : m_sd(sd) {
    reset();
}

uint8_t* F011Fdc::buffer() const { return m_sd->fdcBuffer(); }

void F011Fdc::reset() {
    m_control = m_cmd = m_track = m_sector = m_side = 0;
    m_clock = m_step = 0xFF;
    m_curCmd = -1;
    m_ptrCpu = m_ptrFdc = 0;
    m_swapMask = 0;
    m_drive = 0;
    for (auto& d : m_drv) {
        d.statA = d.statB = 0;
        d.headTrack = 0;
        d.haveDisk = d.canWrite = false;
    }
    refreshMedia();
    for (auto& d : m_drv) d.statB &= ~SB_DSKCHG;   // no disk-changed signal at power-on
}

// Media present / writable from a host file or the SD card's mount registers;
// a change sets the disk-changed signal, as inserting a disk does.
void F011Fdc::refreshMedia() {
    for (int d = 0; d < 2; d++) {
        bool have = false, canWrite = false;
        uint64_t source = 0;
        if (m_drv[d].attached) {
            have = true;
            canWrite = !m_drv[d].fileReadOnly;
            source = m_drv[d].fileSerial;
        } else if (m_sd && m_sd->isMounted()) {
            uint8_t m = m_sd->mountFlags();
            uint8_t imgP = d ? 0x18 : 0x03;      // DxIMG + DxP
            // Bit 2 / 5 reads "not write protected" (sdcardio.vhdl $D68B;
            // HYPPO's dos_d81write_en sets it to allow writes)
            uint8_t we   = d ? 0x20 : 0x04;
            have = (m & imgP) == imgP;
            canWrite = have && (m & we) && m_sd->isWritable();
            if (have) source = 1 + (uint64_t)m_sd->mountStartSector(d);
        }
        // A different disk (not only a change of present / writable) is a disk change
        if (have != m_drv[d].haveDisk || canWrite != m_drv[d].canWrite || source != m_drv[d].source) {
            m_drv[d].source = source;
            setDisk(d, have, canWrite);
        }
    }
}

void F011Fdc::setDisk(int d, bool haveDisk, bool canWrite) {
    Drive& v = m_drv[d];
    v.haveDisk = haveDisk;
    v.canWrite = canWrite;
    v.statB |= SB_DSKCHG;
    if (haveDisk) { v.statA |= SA_TK0;  v.statB |= SB_DSKIN; }
    else          { v.statA &= ~SA_TK0; v.statB &= ~SB_DSKIN; }
    if (canWrite) v.statA &= ~SA_PROT;
    else          v.statA |= SA_PROT;
}

bool F011Fdc::diskOffset(uint32_t& off) const {
    if (m_sector == 0) return false;
    off = (uint32_t)m_track * 10240u + (uint32_t)(m_side & 1) * 5120u + (uint32_t)(m_sector - 1) * 512u;
    return off + 512 <= D81_SIZE;
}

bool F011Fdc::readDisk(int d, uint32_t off, uint8_t* buf) {
    Drive& v = m_drv[d];
    if (v.attached) {
        if (off + 512 > v.image.size()) return false;
        std::copy(v.image.begin() + off, v.image.begin() + off + 512, buf);
        return true;
    }
    uint64_t start = (uint64_t)m_sd->mountStartSector(d) * 512;
    return m_sd->readImage(start + off, buf, 512);
}

bool F011Fdc::writeDisk(int d, uint32_t off, const uint8_t* buf) {
    Drive& v = m_drv[d];
    if (v.attached) {
        if (off + 512 > v.image.size()) return false;
        std::copy(buf, buf + 512, v.image.begin() + off);
        if (diskfile::isArchive(v.path)) { v.writeBack.touched(); return true; }
        return diskfile::patch(v.path, v.image, off, 512);
    }
    uint64_t start = (uint64_t)m_sd->mountStartSector(d) * 512;
    return m_sd->writeImage(start + off, buf, 512);
}

void F011Fdc::readSector() {
    Drive& v = m_drv[m_drive & 1];
    uint8_t tmp[512];
    uint32_t off = 0;
    bool ok = m_drive < 2 && v.haveDisk && diskOffset(off) && readDisk(m_drive, off, tmp);
    if (ok) {
        uint8_t* buf = buffer();
        for (int n = 0; n < 512; n++) {
            buf[m_ptrFdc] = tmp[n];
            m_ptrFdc = (m_ptrFdc + 1) & 511;
        }
        v.statA |= SA_DRQ;
        v.statB |= SB_RDREQ | SB_RUN;
        v.statA &= ~SA_EQ;
    } else {
        v.statA |= SA_RNF;
        v.statB &= 0x0F;           // RDREQ/WTREQ/RUN/GATE off
    }
}

void F011Fdc::writeSector() {
    Drive& v = m_drv[m_drive & 1];
    uint8_t tmp[512];
    uint8_t* buf = buffer();
    for (int n = 0; n < 512; n++) {
        tmp[n] = buf[m_ptrFdc];
        m_ptrFdc = (m_ptrFdc + 1) & 511;
    }
    uint32_t off = 0;
    bool ok = m_drive < 2 && v.haveDisk && v.canWrite && diskOffset(off) && writeDisk(m_drive, off, tmp);
    if (ok) {
        v.statA |= SA_DRQ;
        v.statB |= SB_WTREQ | SB_RUN;
        v.statA &= ~SA_EQ;
    } else {
        v.statA |= SA_RNF;
        v.statB &= 0x0F;
    }
}

void F011Fdc::executeCommand() {
    Drive& v = m_drv[m_drive & 1];
    v.statA &= ~SA_BUSY;
    v.statB |= SB_IRQ;
    if (m_curCmd < 0 || m_curCmd > 0xFF) { m_curCmd = -1; return; }   // control write only
    switch (m_cmd & 0xF8) {
        case 0x40:                       // read sector
            v.statB |= SB_RDREQ;
            v.statA |= SA_DRQ;
            v.statA &= ~SA_EQ;
            readSector();
            break;
        case 0x80:                       // write sector
            if (!(v.statA & SA_PROT)) {
                v.statA |= SA_DRQ;
                v.statA &= ~SA_EQ;
                writeSector();
            }
            break;
        case 0x10:                       // step out (or, with bit 2, wait without stepping)
            if (!(m_cmd & 4)) {
                if (v.headTrack) v.headTrack--;
                if (!v.headTrack) v.statA |= SA_TK0;
            }
            break;
        case 0x18:                       // step in
            if (v.headTrack < 128) v.headTrack++;
            v.statA &= ~SA_TK0;
            break;
        case 0x20:                       // spin up
            m_control |= 0x20;
            v.statA |= SA_RNF;
            break;
        case 0x00:                       // cancel; bit 0: reset the buffer pointers
            if (m_cmd & 1) {
                m_ptrCpu = m_ptrFdc = 0;
                v.statA &= ~SA_DRQ;
                v.statB &= ~SB_RDREQ;
            }
            break;
        default:
            break;
    }
    m_curCmd = -1;
}

bool F011Fdc::ioRead(IBus*, uint32_t addr, uint8_t* val) {
    if (addr < 0xD080 || addr > 0xD09F) return false;
    refreshMedia();
    Drive& v = m_drv[m_drive & 1];
    if (v.statA & SA_BUSY) executeCommand();
    switch (addr & 0x1F) {
        case 0x00: *val = m_control; break;
        case 0x01: *val = m_cmd; break;
        case 0x02: *val = v.statA; break;
        case 0x03: *val = v.statB; v.statB &= ~SB_WTREQ; break;
        case 0x04: *val = m_track; break;
        case 0x05: *val = m_sector; break;
        case 0x06: *val = m_side; break;
        case 0x07: {
            v.statA &= ~SA_DRQ;
            v.statB &= ~SB_RDREQ;
            if (v.statA & SA_EQ) v.statA |= SA_LOST;
            *val = buffer()[m_ptrCpu ^ m_swapMask];
            m_ptrCpu = (m_ptrCpu + 1) & 511;
            if (m_ptrCpu == m_ptrFdc) v.statA |= SA_EQ; else v.statA &= ~SA_EQ;
            break;
        }
        case 0x08: *val = m_clock; break;
        case 0x09: *val = m_step; break;
        case 0x0A: *val = 0; break;      // P CODE: no protection code
        default:   *val = 0xFF; break;
    }
    return true;
}

bool F011Fdc::ioPeek(IBus*, uint32_t addr, uint8_t* val) {
    if (addr < 0xD080 || addr > 0xD09F) return false;
    const Drive& v = m_drv[m_drive & 1];
    switch (addr & 0x1F) {
        case 0x00: *val = m_control; break;
        case 0x01: *val = m_cmd; break;
        case 0x02: *val = v.statA; break;
        case 0x03: *val = v.statB; break;
        case 0x04: *val = m_track; break;
        case 0x05: *val = m_sector; break;
        case 0x06: *val = m_side; break;
        case 0x07: *val = buffer()[m_ptrCpu ^ m_swapMask]; break;
        case 0x08: *val = m_clock; break;
        case 0x09: *val = m_step; break;
        case 0x0A: *val = 0; break;
        default:   *val = 0xFF; break;
    }
    return true;
}

bool F011Fdc::ioWrite(IBus*, uint32_t addr, uint8_t val) {
    if (addr < 0xD080 || addr > 0xD09F) return false;
    refreshMedia();
    switch (addr & 0x1F) {
        case 0x00: {
            m_control = val;
            if (m_curCmd == -1) m_curCmd = 0x100;
            int drive = val & 7;
            if (drive != m_drive) {
                m_drive = drive;
                m_drv[m_drive & 1].statB &= ~SB_DSKCHG;   // selecting a drive clears its disk-changed signal
            }
            m_drv[m_drive & 1].statA |= SA_BUSY;
            m_swapMask = (val & 0x10) ? 0x100 : 0;
            break;
        }
        case 0x01: {
            Drive& v = m_drv[m_drive & 1];
            if ((v.statA & SA_BUSY) && (val & 0xF8)) return true;   // busy: only cancel is taken
            m_cmd = val;
            m_curCmd = val;
            v.statA |= SA_BUSY;
            v.statB &= ~SB_IRQ;
            v.statA &= ~(SA_CRC | SA_LOST | SA_RNF);
            v.statB &= ~(SB_RDREQ | SB_WTREQ);
            break;
        }
        case 0x04: m_track = val; break;
        case 0x05: m_sector = val; break;
        case 0x06: m_side = val; break;
        case 0x07: {
            Drive& v = m_drv[m_drive & 1];
            v.statA &= ~SA_DRQ;
            if (v.statA & SA_EQ) v.statA |= SA_LOST;
            buffer()[m_ptrCpu ^ m_swapMask] = val;
            m_ptrCpu = (m_ptrCpu + 1) & 511;
            if (m_ptrCpu == m_ptrFdc) v.statA |= SA_EQ; else v.statA &= ~SA_EQ;
            break;
        }
        case 0x08: m_clock = val; break;
        case 0x09: m_step = val; break;
        default: break;
    }
    if (m_drv[m_drive & 1].statA & SA_BUSY) executeCommand();
    return true;
}

bool F011Fdc::mountDisk(int unit, const std::string& path) {
    if (unit != 8 && unit != 9) return false;
    Drive& v = m_drv[unit - 8];
    std::vector<uint8_t> image;
    if (!diskfile::read(path, image)) return false;           // plain, .gz or .zip
    if (image.size() < D81_SIZE) return false;                // only 800 KB D81 images
    saveImage(v);                                             // the disk it replaces
    v.image = std::move(image);
    v.attached = true;
    v.fileSerial = m_nextSerial++;
    v.path = path;
    v.fileReadOnly = !diskfile::writable(path);
    refreshMedia();
    return true;
}

void F011Fdc::ejectDisk(int unit) {
    if (unit != 8 && unit != 9) return;
    Drive& v = m_drv[unit - 8];
    saveImage(v);
    v.attached = false;
    v.image.clear();
    v.path.clear();
    refreshMedia();
}

// An archive's pending writes go back to the file
void F011Fdc::saveImage(Drive& v) {
    if (v.attached && v.writeBack.dirty()) diskfile::save(v.path, v.image);
    v.writeBack.clear();
}

F011Fdc::~F011Fdc() {
    for (Drive& v : m_drv) saveImage(v);
}

void F011Fdc::tick(uint64_t) {
    for (Drive& v : m_drv)
        if (v.writeBack.due()) saveImage(v);
}

bool F011Fdc::getDiskStatus(int unit, int& track, int& sector, bool& led) {
    if (unit != 8 && unit != 9) return false;
    track  = m_drv[unit - 8].headTrack;
    sector = m_sector;
    led    = (m_drive == unit - 8) && (m_control & 0x60);   // motor or LED on
    return true;
}

std::string F011Fdc::getMountedDiskPath(int unit) {
    if (unit != 8 && unit != 9) return "";
    const Drive& v = m_drv[unit - 8];
    if (v.attached) return v.path;
    if (v.haveDisk) return "<D81 on SD card @ sector " + std::to_string(m_sd->mountStartSector(unit - 8)) + ">";
    return "";
}

void F011Fdc::getDeviceInfo(DeviceInfo& out) const {
    const Drive& v = m_drv[m_drive & 1];
    out.name = m_name;
    out.baseAddr = 0xD080;
    out.addrMask = 0x1F;
    out.registers.push_back({"CONTROL", 0x00, m_control, "Drive select, side, motor, LED"});
    out.registers.push_back({"COMMAND", 0x01, m_cmd, "Last command"});
    out.registers.push_back({"STATA", 0x02, v.statA, "BUSY DRQ EQ RNF CRC LOST PROT TK0"});
    out.registers.push_back({"STATB", 0x03, v.statB, "RDREQ WTREQ RUN NGATE DSKIN INDEX IRQ DSKCHG"});
    out.registers.push_back({"TRACK", 0x04, m_track, "Track"});
    out.registers.push_back({"SECTOR", 0x05, m_sector, "Sector"});
    out.registers.push_back({"SIDE", 0x06, m_side, "Side"});
    for (int d = 0; d < 2; d++) {
        const Drive& x = m_drv[d];
        std::string src = x.attached ? x.path
                        : x.haveDisk ? "SD card D81" : "no disk";
        out.state.push_back({"Drive " + std::to_string(d), src + (x.haveDisk && !x.canWrite ? " (read-only)" : "")});
    }
    out.state.push_back({"Buffer pointers", "CPU " + std::to_string(m_ptrCpu) + ", FDC " + std::to_string(m_ptrFdc)});
}
