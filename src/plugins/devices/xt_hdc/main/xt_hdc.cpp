// An IBM PC/XT-style hard disk controller with raw image files (#356): see
// xt_hdc.h for the ports, the command protocol and the sources.

#include "xt_hdc.h"
#include <algorithm>
#include <cstring>

namespace {
constexpr int DEFAULT_HEADS = 4;
}

XtHdc::XtHdc() { reset(); }

XtHdc::~XtHdc() {
    for (auto& d : m_drives)
        if (d.file) d.file->flush();
}

void XtHdc::reset() {
    m_phase = Phase::Idle;
    m_dcbPos = 0;
    m_pos = 0;
    m_buf.clear();
    m_completion = 0;
    std::memset(m_sense, 0, sizeof m_sense);
    m_mask = 0;
    m_irqPending = false;
}

bool XtHdc::mount(int drive, const std::string& path) {
    if (drive < 0 || drive > 1) return false;
    auto f = std::make_unique<std::fstream>(path, std::ios::binary | std::ios::in | std::ios::out);
    bool ro = false;
    if (!*f) {
        f = std::make_unique<std::fstream>(path, std::ios::binary | std::ios::in);
        ro = true;
        if (!*f) return false;
    }
    eject(drive);
    Drive& d = m_drives[drive];
    f->seekg(0, std::ios::end);
    d.size = (uint64_t)f->tellg();
    d.file = std::move(f);
    d.readOnly = ro;
    d.path = path;
    if (!d.geometrySet) {
        d.heads = DEFAULT_HEADS;
        const uint64_t perCyl = (uint64_t)DEFAULT_HEADS * SECTORS_PER_TRACK * SECTOR_SIZE;
        d.cylinders = (int)std::max<uint64_t>(1, (d.size + perCyl - 1) / perCyl);
    }
    return true;
}

void XtHdc::eject(int drive) {
    if (drive < 0 || drive > 1) return;
    Drive& d = m_drives[drive];
    if (d.file) d.file->flush();
    const bool keep = d.geometrySet;
    const int c = d.cylinders, h = d.heads;
    d = Drive();
    if (keep) { d.geometrySet = true; d.cylinders = c; d.heads = h; }
}

std::string XtHdc::imagePath(int drive) const { return drive >= 0 && drive < 2 ? m_drives[drive].path : ""; }

void XtHdc::setGeometry(int drive, int cylinders, int heads) {
    if (drive < 0 || drive > 1 || cylinders < 1 || heads < 1 || heads > 16) return;
    m_drives[drive].cylinders = cylinders;
    m_drives[drive].heads = heads;
    m_drives[drive].geometrySet = true;
}

int XtHdc::cylinders(int drive) const { return drive >= 0 && drive < 2 ? m_drives[drive].cylinders : 0; }
int XtHdc::heads(int drive) const { return drive >= 0 && drive < 2 ? m_drives[drive].heads : 0; }

uint8_t XtHdc::status() const {
    uint8_t s = 0;
    switch (m_phase) {
    case Phase::Idle: break;
    case Phase::Command: s = ST_BUSY | ST_REQ | ST_CD; break;
    case Phase::DataIn: s = ST_BUSY | ST_REQ | ST_IO; break;
    case Phase::DataOut: s = ST_BUSY | ST_REQ; break;
    case Phase::Status: s = ST_BUSY | ST_REQ | ST_CD | ST_IO; break;
    }
    if (m_irqPending) s |= ST_INT;
    return s;
}

uint8_t XtHdc::peek(int port) const {
    switch (port & 3) {
    case 0:
        if (m_phase == Phase::DataIn && m_pos < m_buf.size()) return m_buf[m_pos];
        if (m_phase == Phase::Status) return m_completion;
        return 0xFF;
    case 1: return status();
    default: return 0x00;     // the drive-type jumpers; port 3 reads nothing
    }
}

uint8_t XtHdc::read(int port) {
    const uint8_t v = peek(port);
    if ((port & 3) != 0) return v;
    if (m_phase == Phase::DataIn) {
        if (++m_pos >= m_buf.size()) dataByteIn();
    } else if (m_phase == Phase::Status) {
        m_phase = Phase::Idle;
        m_irqPending = false;
    }
    return v;
}

void XtHdc::write(int port, uint8_t v) {
    switch (port & 3) {
    case 1:
        reset();
        return;
    case 2:
        m_phase = Phase::Command;
        m_dcbPos = 0;
        m_irqPending = false;
        return;
    case 3:
        m_mask = v & 0x03;
        return;
    default:
        break;
    }
    if (m_phase == Phase::Command) {
        m_dcb[m_dcbPos++] = v;
        if (m_dcbPos == 6) execute();
    } else if (m_phase == Phase::DataOut) {
        m_buf[m_pos++] = v;
        if (m_pos < m_buf.size()) return;
        switch (m_dcb[0]) {
        case 0x0A:
            if (!storeSector()) return;
            if (advance()) { m_pos = 0; return; }
            complete(ERR_NONE);
            return;
        case 0x0C: {
            Drive& d = m_drives[m_drive];
            const int cyls = m_buf[0] << 8 | m_buf[1];
            const int heads = m_buf[2];
            if (cyls < 1 || heads < 1 || heads > 16) { complete(ERR_INVALID_COMMAND); return; }
            d.cylinders = cyls;
            d.heads = heads;
            d.geometrySet = true;
            complete(ERR_NONE);
            return;
        }
        case 0x0F:
            std::memcpy(m_sectorBuffer, m_buf.data(), SECTOR_SIZE);
            complete(ERR_NONE);
            return;
        default:
            complete(ERR_NONE);
            return;
        }
    }
}

void XtHdc::dataByteIn() {
    if (m_dcb[0] == 0x08 && advance()) {
        if (loadSector()) m_pos = 0;
        return;
    }
    complete(ERR_NONE);
}

int64_t XtHdc::lba() const {
    const Drive& d = m_drives[m_drive];
    return ((int64_t)m_cyl * d.heads + m_head) * SECTORS_PER_TRACK + m_sector;
}

bool XtHdc::advance() {
    if (--m_blocks <= 0) return false;
    if (++m_sector >= SECTORS_PER_TRACK) {
        m_sector = 0;
        if (++m_head >= m_drives[m_drive].heads) {
            m_head = 0;
            ++m_cyl;
        }
    }
    return true;
}

bool XtHdc::loadSector() {
    Drive& d = m_drives[m_drive];
    if (m_cyl >= d.cylinders || m_head >= d.heads || m_sector >= SECTORS_PER_TRACK) {
        complete(ERR_ILLEGAL_ADDRESS);
        return false;
    }
    m_buf.assign(SECTOR_SIZE, 0);
    const uint64_t at = (uint64_t)lba() * SECTOR_SIZE;
    if (at < d.size) {
        d.file->clear();
        d.file->seekg((std::streamoff)at);
        d.file->read((char*)m_buf.data(), SECTOR_SIZE);
        d.file->clear();
    }
    m_phase = Phase::DataIn;
    return true;
}

bool XtHdc::storeSector() {
    Drive& d = m_drives[m_drive];
    if (m_cyl >= d.cylinders || m_head >= d.heads || m_sector >= SECTORS_PER_TRACK) {
        complete(ERR_ILLEGAL_ADDRESS);
        return false;
    }
    if (d.readOnly) {
        complete(ERR_WRITE_FAULT);
        return false;
    }
    const uint64_t at = (uint64_t)lba() * SECTOR_SIZE;
    d.file->clear();
    d.file->seekp((std::streamoff)at);
    d.file->write((const char*)m_buf.data(), SECTOR_SIZE);
    d.file->flush();
    if (!*d.file) {
        complete(ERR_WRITE_FAULT);
        return false;
    }
    d.size = std::max<uint64_t>(d.size, at + SECTOR_SIZE);
    return true;
}

void XtHdc::complete(uint8_t sense) {
    m_sense[0] = sense ? (uint8_t)(sense | 0x80) : 0;
    m_sense[1] = (uint8_t)(m_drive << 5 | m_head);
    m_sense[2] = (uint8_t)((m_cyl >> 2 & 0xC0) | m_sector);
    m_sense[3] = (uint8_t)m_cyl;
    m_completion = (uint8_t)((sense ? 0x02 : 0) | m_drive << 5);
    m_phase = Phase::Status;
    m_buf.clear();
    m_pos = 0;
    if (m_mask & 0x02) m_irqPending = true;
}

void XtHdc::execute() {
    m_drive = m_dcb[1] >> 5 & 1;
    m_head = m_dcb[1] & 0x1F;
    m_sector = m_dcb[2] & 0x3F;
    m_cyl = (m_dcb[2] & 0xC0) << 2 | m_dcb[3];
    m_blocks = m_dcb[4] ? m_dcb[4] : 256;
    m_pos = 0;
    Drive& d = m_drives[m_drive];
    const bool present = (bool)d.file;
    const bool addrOk = m_cyl < d.cylinders && m_head < d.heads && m_sector < SECTORS_PER_TRACK;
    switch (m_dcb[0]) {
    case 0x00: case 0x01:
        complete(present ? ERR_NONE : ERR_NOT_READY);
        return;
    case 0x0B: case 0x05:
        complete(!present ? ERR_NOT_READY : addrOk ? ERR_NONE : ERR_ILLEGAL_ADDRESS);
        return;
    case 0x03: {
        // The last command's sense, then this one completes cleanly
        std::vector<uint8_t> sense(m_sense, m_sense + 4);
        m_buf = sense;
        m_phase = Phase::DataIn;
        return;
    }
    case 0x04: case 0x06: case 0x07: {
        if (!present) { complete(ERR_NOT_READY); return; }
        if (d.readOnly) { complete(ERR_WRITE_FAULT); return; }
        // Format drive: every track; format track: this one (zeroed)
        const bool all = m_dcb[0] == 0x04;
        if (!all && !addrOk) { complete(ERR_ILLEGAL_ADDRESS); return; }
        const uint64_t trackBytes = (uint64_t)SECTORS_PER_TRACK * SECTOR_SIZE;
        const uint64_t from = all ? 0 : ((uint64_t)m_cyl * d.heads + m_head) * trackBytes;
        const uint64_t to = all ? (uint64_t)d.cylinders * d.heads * trackBytes : from + trackBytes;
        std::vector<char> zero(trackBytes, 0);
        d.file->clear();
        d.file->seekp((std::streamoff)from);
        for (uint64_t at = from; at < to; at += trackBytes) d.file->write(zero.data(), (std::streamsize)trackBytes);
        d.file->flush();
        d.size = std::max(d.size, to);
        complete(*d.file ? ERR_NONE : ERR_WRITE_FAULT);
        return;
    }
    case 0x08:
        if (!present) { complete(ERR_NOT_READY); return; }
        loadSector();
        return;
    case 0x0A:
        if (!present) { complete(ERR_NOT_READY); return; }
        if (!addrOk) { complete(ERR_ILLEGAL_ADDRESS); return; }
        m_buf.assign(SECTOR_SIZE, 0);
        m_phase = Phase::DataOut;
        return;
    case 0x0C:
        m_buf.assign(8, 0);
        m_phase = Phase::DataOut;
        return;
    case 0x0E:
        m_buf.assign(m_sectorBuffer, m_sectorBuffer + SECTOR_SIZE);
        m_phase = Phase::DataIn;
        return;
    case 0x0F:
        m_buf.assign(SECTOR_SIZE, 0);
        m_phase = Phase::DataOut;
        return;
    case 0xE0: case 0xE3: case 0xE4:
        complete(ERR_NONE);
        return;
    default:
        complete(ERR_INVALID_COMMAND);
        return;
    }
}
