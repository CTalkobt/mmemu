// The DriveWire 4 server (#356): see drivewire_server.h.

#include "drivewire_server.h"
#include <algorithm>
#include <cstring>

struct DriveWireServer::Drive {
    std::string path;
    Mode mode = Mode::WriteThrough;
    std::unique_ptr<std::fstream> file;
    uint64_t size = 0;                                      // bytes in the file
    std::map<uint32_t, std::array<uint8_t, 256>> overlay;   // copy-on-write sectors
};

DriveWireServer::DriveWireServer() {
    m_clock = [] {
        const std::time_t now = std::time(nullptr);
        std::tm t{};
        localtime_r(&now, &t);
        return t;
    };
}

DriveWireServer::~DriveWireServer() = default;

uint16_t DriveWireServer::checksum(const uint8_t* data, size_t n) {
    uint16_t sum = 0;
    for (size_t i = 0; i < n; ++i) sum = (uint16_t)(sum + data[i]);
    return sum;
}

void DriveWireServer::reset() {
    m_cmd.clear();
    m_need = 0;
    m_quiet = 0;
    m_out.clear();
}

uint8_t DriveWireServer::send() {
    if (m_out.empty()) return 0;
    const uint8_t b = m_out.front();
    m_out.pop_front();
    return b;
}

void DriveWireServer::advance(double seconds) {
    if (m_cmd.empty()) return;
    m_quiet += seconds;
    if (m_quiet >= COMMAND_TIMEOUT) done();     // the rest never came: resynchronise
}

// The bytes a command needs before the server acts (more may follow: dispatch)
size_t DriveWireServer::lengthOf(uint8_t op) const {
    switch (op) {
        case OP_TIME: case OP_SERREAD: case OP_INIT: case OP_TERM: case OP_PRINTFLUSH: case OP_NOP:
        case OP_RESET1: case OP_RESET2: case OP_RESET3:
            return 1;
        case OP_DWINIT: case OP_SERINIT: case OP_SERTERM: case OP_PRINT:
        case OP_NAMEOBJ_MOUNT: case OP_NAMEOBJ_CREATE:   // then the name
            return 2;
        case OP_GETSTAT: case OP_SETSTAT: case OP_SERGETSTAT: case OP_SERSETSTAT: case OP_SERWRITE:
        case OP_SERREADM: case OP_SERWRITEM:              // then the bytes
            return 3;
        case OP_READ: case OP_REREAD: case OP_READEX: case OP_REREADEX:
            return 5;
        case OP_SETTIME:
            return 7;
        case OP_WRITE: case OP_REWRITE:
            return 5 + 256 + 2;
        default:
            if (op >= OP_FASTWRITE && op <= OP_FASTWRITE + 14) return 2;
            return 1;
    }
}

void DriveWireServer::done() {
    m_cmd.clear();
    m_need = 0;
    m_quiet = 0;
}

void DriveWireServer::receive(uint8_t b) {
    m_cmd.push_back(b);
    m_quiet = 0;
    if (m_cmd.size() == 1) m_need = lengthOf(b);
    if (m_cmd.size() >= m_need) dispatch();
}

void DriveWireServer::dispatch() {
    const uint8_t op = m_cmd[0];
    m_stats.lastOp = op;
    switch (op) {
        case OP_TIME: {
            const std::tm t = m_clock();
            push((uint8_t)t.tm_year);
            push((uint8_t)(t.tm_mon + 1));
            push((uint8_t)t.tm_mday);
            push((uint8_t)t.tm_hour);
            push((uint8_t)t.tm_min);
            push((uint8_t)t.tm_sec);
            break;
        }
        case OP_DWINIT:
            push(SERVER_VERSION);
            break;
        case OP_SERREAD:
            // No virtual serial channel has data (nor is one closing)
            ++m_stats.serialPolls;
            push(0);
            push(0);
            break;
        case OP_SERREADM:
            // Asked only after OP_SERREAD announces bytes, which never happens:
            // as the spec, more than are queued (none) gets no answer
            break;
        case OP_SERWRITEM:
            if (m_cmd.size() == 3 && m_cmd[2] > 0) { m_need = 3 + m_cmd[2]; return; }
            break;
        case OP_SERSETSTAT:
            if (m_cmd.size() == 3 && m_cmd[2] == SS_COMST) { m_need = 3 + 26; return; }
            break;
        case OP_NAMEOBJ_MOUNT: case OP_NAMEOBJ_CREATE:
            if (m_cmd.size() == 2 && m_cmd[1] > 0) { m_need = 2 + m_cmd[1]; return; }
            push(0);                                // no named objects: failed
            break;
        case OP_PRINT:
            m_print.push_back(m_cmd[1]);
            break;
        case OP_READ: case OP_REREAD: {
            const int drive = m_cmd[1];
            const uint32_t lsn = lsnOf();
            m_stats.lastDrive = drive;
            m_stats.lastLsn = lsn;
            ++(op == OP_READ ? m_stats.reads : m_stats.rereads);
            uint8_t data[256];
            const uint8_t err = readSector(drive, lsn, data);
            if (err) { push(err); break; }
            const uint16_t sum = checksum(data, 256);
            push(E_OK);
            push((uint8_t)(sum >> 8));
            push((uint8_t)sum);
            for (uint8_t d : data) push(d);
            break;
        }
        case OP_READEX: case OP_REREADEX:
            if (m_cmd.size() == 5) {
                // The sector (zeros if it can't be read), then the guest's checksum
                const int drive = m_cmd[1];
                const uint32_t lsn = lsnOf();
                m_stats.lastDrive = drive;
                m_stats.lastLsn = lsn;
                ++(op == OP_READEX ? m_stats.reads : m_stats.rereads);
                m_readErr = readSector(drive, lsn, m_sector.data());
                if (m_readErr) m_sector.fill(0);
                for (uint8_t d : m_sector) push(d);
                m_need = 7;
                return;
            }
            if (m_readErr) {
                push(m_readErr);
            } else if (((uint16_t)m_cmd[5] << 8 | m_cmd[6]) != checksum(m_sector.data(), 256)) {
                ++m_stats.crcErrors;
                push(E_CRC);
            } else {
                push(E_OK);
            }
            break;
        case OP_WRITE: case OP_REWRITE: {
            const int drive = m_cmd[1];
            const uint32_t lsn = lsnOf();
            m_stats.lastDrive = drive;
            m_stats.lastLsn = lsn;
            ++m_stats.writes;
            const uint8_t* data = &m_cmd[5];
            if (((uint16_t)m_cmd[261] << 8 | m_cmd[262]) != checksum(data, 256)) {
                ++m_stats.crcErrors;
                push(E_CRC);
            } else {
                push(writeSector(drive, lsn, data));
            }
            break;
        }
        case OP_NOP: case OP_INIT: case OP_TERM: case OP_RESET1: case OP_RESET2: case OP_RESET3:
        case OP_GETSTAT: case OP_SETSTAT: case OP_SERGETSTAT: case OP_SERINIT: case OP_SERTERM:
        case OP_SERWRITE: case OP_PRINTFLUSH: case OP_SETTIME:
            break;
        default:
            if (op < OP_FASTWRITE || op > OP_FASTWRITE + 14) ++m_stats.unknownOps;
            break;
    }
    done();
}

bool DriveWireServer::mount(int drive, const std::string& path, Mode mode) {
    if (drive < 0 || drive >= DRIVES || path.empty()) return false;
    auto d = std::make_unique<Drive>();
    d->path = path;
    d->mode = mode;
    if (mode == Mode::WriteThrough)
        d->file = std::make_unique<std::fstream>(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!d->file || !*d->file) {
        // Not writable: read only (writes answer E$WP)
        d->file = std::make_unique<std::fstream>(path, std::ios::binary | std::ios::in);
        if (!*d->file) return false;
        if (mode == Mode::WriteThrough) d->mode = Mode::ReadOnly;
    }
    d->file->seekg(0, std::ios::end);
    d->size = (uint64_t)d->file->tellg();
    m_drives[drive] = std::move(d);
    return true;
}

bool DriveWireServer::eject(int drive) {
    if (!mounted(drive)) return false;
    m_drives[drive].reset();
    return true;
}

std::string DriveWireServer::imagePath(int drive) const { return mounted(drive) ? m_drives[drive]->path : ""; }

DriveWireServer::Mode DriveWireServer::mode(int drive) const {
    return mounted(drive) ? m_drives[drive]->mode : Mode::ReadOnly;
}

uint32_t DriveWireServer::sectors(int drive) const {
    if (!mounted(drive)) return 0;
    const Drive& d = *m_drives[drive];
    uint32_t n = (uint32_t)((d.size + 255) / 256);
    if (!d.overlay.empty()) n = std::max(n, d.overlay.rbegin()->first + 1);
    return n;
}

uint8_t DriveWireServer::readSector(int drive, uint32_t lsn, uint8_t* out) {
    std::memset(out, 0, 256);
    if (!mounted(drive)) return E_NOTRDY;
    Drive& d = *m_drives[drive];
    const auto o = d.overlay.find(lsn);
    if (o != d.overlay.end()) {
        std::memcpy(out, o->second.data(), 256);
        return E_OK;
    }
    const uint64_t at = (uint64_t)lsn * 256;
    if (at >= d.size) return E_OK;                          // past the end: zeros
    d.file->clear();
    d.file->seekg((std::streamoff)at);
    d.file->read((char*)out, 256);
    if (d.file->bad()) return E_READ;
    d.file->clear();
    return E_OK;
}

uint8_t DriveWireServer::writeSector(int drive, uint32_t lsn, const uint8_t* data) {
    if (!mounted(drive)) return E_NOTRDY;
    Drive& d = *m_drives[drive];
    if (d.mode == Mode::ReadOnly) return E_WP;
    if (d.mode == Mode::CopyOnWrite) {
        std::memcpy(d.overlay[lsn].data(), data, 256);
        return E_OK;
    }
    const uint64_t at = (uint64_t)lsn * 256;
    d.file->clear();
    if (at > d.size) {
        // Past the end: fill the gap with zeros first
        d.file->seekp((std::streamoff)d.size);
        const std::vector<uint8_t> zeros((size_t)(at - d.size), 0);
        d.file->write((const char*)zeros.data(), (std::streamsize)zeros.size());
    }
    d.file->seekp((std::streamoff)at);
    d.file->write((const char*)data, 256);
    d.file->flush();
    if (!*d.file) {
        d.file->clear();
        return E_WRITE;
    }
    d.size = std::max<uint64_t>(d.size, at + 256);
    return E_OK;
}

std::string DriveWireServer::describe() const {
    std::string s;
    for (int i = 0; i < DRIVES; ++i) {
        if (!m_drives[i]) continue;
        const std::string& p = m_drives[i]->path;
        const auto slash = p.find_last_of('/');
        if (!s.empty()) s += "; ";
        s += "drive " + std::to_string(i) + ": " + (slash == std::string::npos ? p : p.substr(slash + 1));
        if (m_drives[i]->mode == Mode::CopyOnWrite) s += " (copy on write)";
        else if (m_drives[i]->mode == Mode::ReadOnly) s += " (read only)";
    }
    return s;
}
