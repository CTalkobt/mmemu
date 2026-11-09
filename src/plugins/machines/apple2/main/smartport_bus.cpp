// SmartPort devices on the disk port, after Clemens (clem_smartport.c,
// Samir Sinha, MIT) and the IIgs Firmware Reference, chapter 7.

#include "smartport_bus.h"
#include "libdevices/main/disk_file.h"
#include <algorithm>
#include <cstring>

namespace apple2 {

namespace {
constexpr uint8_t CMD_STATUS = 0x00, CMD_READ = 0x01, CMD_WRITE = 0x02, CMD_FORMAT = 0x03,
                  CMD_CONTROL = 0x04, CMD_INIT = 0x05;
constexpr uint8_t TYPE_CMD = 0, TYPE_STATUS = 1, TYPE_DATA = 2;
constexpr uint8_t ERR_BADCMD = 0x01, ERR_BADCTL = 0x21, ERR_IO = 0x27,
                  ERR_PROTECT = 0x2B, ERR_BADBLOCK = 0x2D, ERR_OFFLINE = 0x2F,
                  END_OF_CHAIN = 0x7F;                 // the status byte reads $FF
constexpr uint32_t UNIDISK_BYTES = 819200;
}  // namespace

bool blockImage(const std::vector<uint8_t>& data, size_t& off, size_t& size) {
    off = 0;
    size = data.size();
    if (data.size() >= 64 && std::memcmp(data.data(), "2IMG", 4) == 0) {
        if (data[12] != 1) return false;                     // ProDOS order only
        off = data[24] | data[25] << 8 | data[26] << 16 | (size_t)data[27] << 24;
        size = data[28] | data[29] << 8 | data[30] << 16 | (size_t)data[31] << 24;
        if (!size) size = (size_t)(data[20] | data[21] << 8 | data[22] << 16 | (size_t)data[23] << 24) * 512;
        if (off + size > data.size()) return false;
    }
    return size >= 512 && size % 512 == 0;
}

SmartPortBus::SmartPortBus(double unitsPerUs) : m_unitsPerUs(unitsPerUs), m_byteTime(32.0 * unitsPerUs) {}

void SmartPortBus::reset() {
    for (Unit& u : m_unit) u.id = 0;
    m_ph = 0;
    m_enabled = m_req = false;
    m_st = St::Ready;
    m_active = -1;
}

int SmartPortBus::units() const {
    if (m_fixedUnits > 0) return std::min(m_fixedUnits, UNITS);
    for (int i = UNITS - 1; i >= 0; --i) if (m_unit[i].size) return i + 1;
    return 0;
}

bool SmartPortBus::mountDisk(int unit, const std::string& path) {
    if (unit < 1 || unit > UNITS) return false;
    std::vector<uint8_t> data;
    if (!diskfile::read(path, data)) return false;
    size_t off, size;
    if (!blockImage(data, off, size)) return false;
    Unit& u = m_unit[unit - 1];
    const int id = u.id;
    u = Unit();
    u.id = id;
    u.file = std::move(data);
    u.offset = off;
    u.size = size;
    u.path = path;
    u.protect = !diskfile::writable(path);
    return true;
}

bool SmartPortBus::insertBytes(int unit, std::vector<uint8_t> data) {
    if (unit < 1 || unit > UNITS || data.size() < 512 || data.size() % 512) return false;
    Unit& u = m_unit[unit - 1];
    const int id = u.id;
    u = Unit();
    u.id = id;
    u.size = data.size();
    u.file = std::move(data);
    return true;
}

void SmartPortBus::ejectDisk(int unit) {
    if (unit < 1 || unit > UNITS) return;
    const int id = m_unit[unit - 1].id;
    m_unit[unit - 1] = Unit();
    m_unit[unit - 1].id = id;
}

bool SmartPortBus::getDiskStatus(int unit, int& track, int& sector, bool& led) {
    if (unit < 1 || unit > UNITS) return false;
    Unit& u = m_unit[unit - 1];
    track = std::max(u.lastBlock, 0);
    sector = 0;
    led = u.busy;
    u.busy = false;
    return u.size != 0;
}

std::string SmartPortBus::getMountedDiskPath(int unit) {
    return unit >= 1 && unit <= UNITS ? m_unit[unit - 1].path : "";
}

// The bus: phases 0 and 2 alone reset it, 1 and 3 enable it, 0 is REQ
void SmartPortBus::phases(uint32_t ph, uint64_t now) {
    m_ph = ph & 0xF;
    if (m_ph == 0x5) {
        for (Unit& u : m_unit) u.id = 0;
        m_st = St::Ready;
        m_active = -1;
    }
    const bool en = (m_ph & 0xA) == 0xA;
    if (en && !m_enabled) {
        m_st = St::Ready;
        m_req = false;
    }
    m_enabled = en;
    if (!en) { m_req = false; return; }
    const bool req = m_ph & 1;
    if (req && !m_req) { m_req = true; reqRose(now); }
    else if (!req && m_req) { m_req = false; reqFell(); }
}

void SmartPortBus::reqRose(uint64_t now) {
    switch (m_st) {
        case St::Ready:
            m_active = -1;
            [[fallthrough]];
        case St::PendingData:
            m_in.clear();
            m_sawBegin = false;
            m_writing = false;
            m_st = St::Receive;
            break;
        case St::Response:
            m_outPos = 0;
            m_nextAt = (double)now + m_byteTime;
            break;
        default:
            break;
    }
}

void SmartPortBus::reqFell() {
    switch (m_st) {
        case St::Receive:                                   // a packet not for us, or cut short
            m_st = m_cmd != 0xFF && m_active >= 0 ? St::PendingData : St::Ready;
            break;
        case St::Executing: {
            Packet p;
            decode(p);
            command(p);
            break;
        }
        case St::Complete:
        case St::Response:
            m_st = St::Ready;
            m_cmd = 0xFF;
            break;
        default:
            break;
    }
}

bool SmartPortBus::ack(uint64_t now) {
    switch (m_st) {
        case St::Executing: return (double)now < m_ackLowAt;
        case St::Response:
            if (m_outPos >= m_out.size() && m_req && (double)now >= m_nextAt) { m_st = St::Complete; return false; }
            return true;
        case St::Complete: return false;
        default: return true;
    }
}

uint8_t SmartPortBus::read(uint64_t now) {
    if (m_st != St::Response || !m_req || m_outPos >= m_out.size() || (double)now < m_nextAt) return 0;
    m_nextAt = (double)now + m_byteTime;
    return m_out[m_outPos++];
}

uint8_t SmartPortBus::handshake(uint64_t now) const {
    if (!m_writing) return 0xC0;
    const double elapsed = (double)(now - m_lastWrite);
    return (uint8_t)((elapsed >= m_byteTime ? 0x80 : 0) | (elapsed < 2 * m_byteTime ? 0x40 : 0));
}

// A byte from the host: the packet from $C3 to $C8
void SmartPortBus::write(uint8_t v, uint64_t now) {
    m_lastWrite = now;
    m_writing = true;
    if (m_st != St::Receive || !m_req) return;
    if (!m_sawBegin) {
        if (v == 0xC3) m_sawBegin = true;
        return;
    }
    m_in.push_back(v);
    if (m_in.size() < 7) return;
    const size_t odd = m_in[5] & 0x7F, grp = m_in[6] & 0x7F;
    const size_t need = 7 + (odd ? odd + 1 : 0) + grp * 8 + 3;
    if (m_in.size() < need) return;
    Packet p;
    if (m_in.back() != 0xC8 || !decode(p)) { m_st = St::Ready; return; }
    const bool data = m_cmd != 0xFF && m_active >= 0;
    if (data) {
        if ((p.type & 0x7F) != TYPE_DATA || p.dest != m_unit[m_active].id) { m_st = St::Ready; return; }
    } else {
        const int t = target(p);
        if (t < 0) { m_st = St::Ready; return; }
        m_active = t;
    }
    m_st = St::Executing;
    m_ackLowAt = (double)now + m_byteTime;
}

// The packet in m_in (from the byte after $C3): header, 7-bit groups, checksum
bool SmartPortBus::decode(Packet& p) const {
    const std::vector<uint8_t>& in = m_in;
    if (in.size() < 10) return false;
    p.dest = in[0] & 0x7F;
    p.src = in[1] & 0x7F;
    p.type = in[2] & 0x7F;
    p.ext = (in[3] & 0x40) != 0;
    p.status = in[4] & 0x7F;
    const size_t odd = in[5] & 0x7F, grp = in[6] & 0x7F;
    uint8_t chk = 0;
    for (int i = 0; i < 7; ++i) chk ^= in[(size_t)i];
    size_t pos = 7;
    p.data.clear();
    if (odd) {
        const uint8_t msb = in[pos++];
        for (size_t i = 0; i < odd; ++i) p.data.push_back((uint8_t)((in[pos++] & 0x7F) | ((msb << (1 + i)) & 0x80)));
    }
    for (size_t g = 0; g < grp; ++g) {
        const uint8_t msb = in[pos++];
        for (size_t j = 0; j < 7; ++j) p.data.push_back((uint8_t)((in[pos++] & 0x7F) | ((msb << (1 + j)) & 0x80)));
    }
    for (uint8_t b : p.data) chk ^= b;
    const uint8_t got = (uint8_t)((in[pos] & 0x55) | ((in[pos + 1] & 0x55) << 1));
    return got == chk;
}

int SmartPortBus::target(const Packet& p) const {
    if ((p.type & 0x7F) != TYPE_CMD || p.data.empty()) return -1;
    const int n = units();
    if ((p.data[0] & 0x3F) == CMD_INIT) {
        for (int i = 0; i < n; ++i) if (m_unit[i].id == 0) return i;    // the first without an ID
        return -1;
    }
    for (int i = 0; i < n; ++i) if (m_unit[i].id && m_unit[i].id == p.dest) return i;
    return -1;
}

// The reply: sync (five self-sync $FFs as the IWM sees them), $C3, the
// header, the contents in 7-bit groups, the checksum, $C8
void SmartPortBus::reply(int unit, uint8_t type, uint8_t status, const uint8_t* data, size_t len) {
    m_out.assign({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xC3});
    const size_t odd = len % 7, grp = len / 7;
    const uint8_t hdr[7] = {0x80, (uint8_t)(0x80 | m_unit[unit].id), (uint8_t)(0x80 | type), 0x80,
                            (uint8_t)(0x80 | status), (uint8_t)(0x80 | odd), (uint8_t)(0x80 | grp)};
    uint8_t chk = 0;
    for (uint8_t b : hdr) { m_out.push_back(b); chk ^= b; }
    for (size_t i = 0; i < len; ++i) chk ^= data[i];
    if (odd) {
        uint8_t msb = 0x80;
        for (size_t i = 0; i < odd; ++i) if (data[i] & 0x80) msb |= (uint8_t)(0x40 >> i);
        m_out.push_back(msb);
        for (size_t i = 0; i < odd; ++i) m_out.push_back(data[i] | 0x80);
    }
    for (size_t g = 0; g < grp; ++g) {
        const uint8_t* s = data + odd + g * 7;
        uint8_t msb = 0x80;
        for (int j = 0; j < 7; ++j) if (s[j] & 0x80) msb |= (uint8_t)(0x40 >> j);
        m_out.push_back(msb);
        for (int j = 0; j < 7; ++j) m_out.push_back(s[j] | 0x80);
    }
    m_out.push_back(chk | 0xAA);
    m_out.push_back((uint8_t)((chk >> 1) | 0xAA));
    m_out.push_back(0xC8);
    m_outPos = 0;
    m_st = St::Response;
}

// A packet for unit m_active, REQ having fallen
void SmartPortBus::command(const Packet& p) {
    ++m_commands;
    const int n = m_active;
    Unit& u = m_unit[n];
    const uint32_t blocks = (uint32_t)(u.size / 512);
    if (m_cmd != 0xFF) {                                     // the data after WRITEBLOCK or CONTROL
        uint8_t st = 0;
        if (m_cmd == CMD_WRITE) {
            if (!u.size) st = ERR_OFFLINE;
            else if (u.protect) st = ERR_PROTECT;
            else if (m_writeBlock >= blocks) st = ERR_BADBLOCK;
            else if (p.data.size() < 512) st = ERR_IO;
            else {
                const size_t at = u.offset + (size_t)m_writeBlock * 512;
                std::copy(p.data.begin(), p.data.begin() + 512, u.file.begin() + (long)at);
                if (!u.path.empty()) diskfile::patch(u.path, u.file, at, 512);
                u.lastBlock = (int)m_writeBlock;
                u.busy = true;
            }
        }
        m_cmd = 0xFF;
        reply(n, TYPE_STATUS, st, nullptr, 0);
        return;
    }
    const uint8_t cmd = p.data[0];
    const bool ext = (cmd & 0x40) || p.ext;
    auto param = [&](size_t i) -> uint32_t { return i < p.data.size() ? p.data[i] : 0; };
    const size_t at = ext ? 6 : 4;                           // after the command, count and buffer
    uint32_t block = param(at) | param(at + 1) << 8 | param(at + 2) << 16;
    if (ext) block |= param(at + 3) << 24;
    switch (cmd & 0x3F) {
        case CMD_INIT: {
            u.id = p.dest;
            reply(n, TYPE_STATUS, n == units() - 1 ? END_OF_CHAIN : 0, nullptr, 0);
            return;
        }
        case CMD_STATUS: {
            const uint8_t code = (uint8_t)param(at);
            const bool online = u.size != 0;
            const uint8_t flags = online ? (uint8_t)(0xF8 | (u.protect ? 4 : 0)) : 0xE8;
            uint8_t st[25] = {flags, (uint8_t)blocks, (uint8_t)(blocks >> 8), (uint8_t)(blocks >> 16)};
            if (code == 0) { reply(n, TYPE_STATUS, 0, st, 4); return; }
            if (code == 3) {                                 // the DIB
                // An 800 KB disk is named as a UniDisk 3.5, but typed as a
                // removable block device ($02/$00): with a UniDisk's own type
                // ($01/$00) GS/OS hands it to its UniDisk driver, which talks
                // to the drive's 65C02 firmware (not emulated)
                const bool uni = !online || u.size == UNIDISK_BYTES;
                const char* id = uni ? "UNIDISK 3.5     " : "MMEMU HARD DISK ";
                st[4] = (uint8_t)(uni ? 11 : 15);
                std::memcpy(st + 5, id, 16);
                st[21] = 0x02;
                st[22] = uni ? 0x00 : 0x20;                  // removable / fixed
                st[23] = 0x00; st[24] = 0x01;
                reply(n, TYPE_STATUS, 0, st, 25);
                return;
            }
            reply(n, TYPE_STATUS, ERR_BADCTL, nullptr, 0);
            return;
        }
        case CMD_READ: {
            if (!u.size) { reply(n, TYPE_STATUS, ERR_OFFLINE, nullptr, 0); return; }
            if (block >= blocks) { reply(n, TYPE_STATUS, ERR_BADBLOCK, nullptr, 0); return; }
            u.lastBlock = (int)block;
            u.busy = true;
            reply(n, TYPE_DATA, 0, &u.file[u.offset + (size_t)block * 512], 512);
            return;
        }
        case CMD_WRITE:
        case CMD_CONTROL:                                    // a data packet follows
            m_cmd = cmd & 0x3F;
            m_writeBlock = block;
            m_st = St::PendingData;
            return;
        case CMD_FORMAT: {
            uint8_t st = 0;
            if (!u.size) st = ERR_OFFLINE;
            else if (u.protect) st = ERR_PROTECT;
            else {
                std::fill(u.file.begin() + (long)u.offset, u.file.begin() + (long)(u.offset + u.size), 0);
                if (!u.path.empty()) diskfile::patch(u.path, u.file, u.offset, u.size);
            }
            reply(n, TYPE_STATUS, st, nullptr, 0);
            return;
        }
        default:
            reply(n, TYPE_STATUS, ERR_BADCMD, nullptr, 0);
            return;
    }
}

}  // namespace apple2
