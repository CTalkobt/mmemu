// The Radio Shack hard disk interface (WD1010), after xtrs (Tim Mann;
// trs_hard.c, reed.h; MIT-style licence); mmsim's own code.

#include "trs80_hard.h"
#include <cstdio>
#include <cstring>
#include <fstream>

namespace trs80 {

namespace {
constexpr size_t HEADER = 256, SECTOR = 256;
constexpr uint8_t ST_BUSY = 0x80, ST_READY = 0x40, ST_SEEKDONE = 0x10, ST_DRQ = 0x08, ST_ERR = 0x01;
constexpr uint8_t ER_NOTFOUND = 0x10, ER_ABORT = 0x04;
constexpr uint8_t IDLE = ST_READY | ST_SEEKDONE;

uint8_t checksum(const uint8_t* h) {
    unsigned s = 0;
    for (int i = 0; i < 32; ++i) if (i != 3) s += h[i];
    return (uint8_t)(s ^ 0x4C);
}
}  // namespace

HardDisk::~HardDisk() { flush(); }

void HardDisk::reset() {
    m_control = m_data = m_error = m_secCount = m_sector = 0;
    m_cyl = 0;
    m_sdh = 0;
    m_status = 0;
    m_command = 0;
    m_done = -1;
}

bool HardDisk::anyDrive() const {
    for (const auto& d : m_drive) if (d.present()) return true;
    return false;
}

std::vector<uint8_t> HardDisk::create(int cyls, int heads) {
    if (cyls < 1) cyls = 1;
    if (cyls > 256) cyls = 256;
    if (heads < 1) heads = 1;
    if (heads > 8) heads = 8;
    std::vector<uint8_t> img(HEADER + (size_t)cyls * heads * 32 * SECTOR, 0);
    uint8_t* h = img.data();
    h[0] = 0x56; h[1] = 0xCB; h[2] = 0x10; h[4] = 1; h[5] = 4;
    h[10] = 0x42;                       // made by an emulator (as xtrs's mkdisk)
    h[28] = (uint8_t)(cyls & 0xFF);     // 0 = 256
    h[29] = (uint8_t)((heads * 32) & 0xFF);
    h[30] = (uint8_t)((heads * 32) & 0xFF);
    h[31] = 1;
    h[3] = checksum(h);
    return img;
}

bool HardDisk::load(int d, std::vector<uint8_t> img, const std::string& path, std::string* err) {
    if (d < 0 || d > 3) return false;
    if (img.size() < HEADER || img[0] != 0x56 || img[1] != 0xCB || img[2] >= 0x1F) {
        if (err) *err = "not a Reed hard disk image (.hdv)";
        return false;
    }
    Drive nd;
    nd.cyls = img[28] ? img[28] : 256;
    const int perCyl = img[29] ? img[29] : 256;
    nd.heads = perCyl / 32;
    if (perCyl % 32 || nd.heads < 1 || nd.heads > 8) {
        if (err) *err = "unusable geometry (sectors a cylinder not a multiple of 32, or over 8 heads)";
        return false;
    }
    nd.writeProtected = img[7] & 0x80;
    // A short image is read as zeros past its end (and grows when written)
    nd.image = std::move(img);
    nd.path = path;
    flush();
    m_drive[d] = std::move(nd);
    return true;
}

bool HardDisk::mountDisk(int unit, const std::string& path) {
    const int d = unit - 1;
    if (d < 0 || d > 3) return false;
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (!load(d, std::move(b), path)) return false;
    // A read-only file is write protected
    std::ofstream w(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!w) m_drive[d].writeProtected = true;
    return true;
}

void HardDisk::ejectDisk(int unit) {
    const int d = unit - 1;
    if (d < 0 || d > 3 || !m_drive[d].present()) return;
    flush();
    m_drive[d] = Drive();
}

bool HardDisk::getDiskStatus(int unit, int& track, int& sector, bool& led) {
    const int d = unit - 1;
    if (!m_fitted || d < 0 || d > 3) return false;
    track = m_cyl;
    sector = m_sector;
    led = m_done >= 0 && ((m_sdh >> 3) & 3) == d;
    return true;
}

std::string HardDisk::getMountedDiskPath(int unit) {
    const int d = unit - 1;
    return d >= 0 && d < 4 ? m_drive[d].path : "";
}

void HardDisk::getDeviceInfo(DeviceInfo& out) const {
    out.name = "Hard disk (WD1010)";
    out.baseAddr = 0xC0;
    out.addrMask = 0x0F;
    char b[64];
    std::snprintf(b, sizeof b, "$%02X", m_status);
    out.state.push_back({"Status", b});
    std::snprintf(b, sizeof b, "drive %d cyl %d head %d sector %d", (m_sdh >> 3) & 3, m_cyl, m_sdh & 7, m_sector);
    out.state.push_back({"Position", b});
    for (int d = 0; d < 4; ++d) {
        if (!m_drive[d].present()) continue;
        std::snprintf(b, sizeof b, " (%d cyl, %d heads)", m_drive[d].cyls, m_drive[d].heads);
        out.state.push_back({"Drive " + std::to_string(d), (m_drive[d].path.empty() ? "(image)" : m_drive[d].path) + b});
    }
}

void HardDisk::flush() {
    for (auto& d : m_drive) {
        if (!d.dirty || d.path.empty()) continue;
        std::ofstream f(d.path, std::ios::binary | std::ios::trunc);
        if (f) f.write(reinterpret_cast<const char*>(d.image.data()), (std::streamsize)d.image.size());
        d.dirty = false;
    }
}

uint8_t HardDisk::peek(uint8_t port) const {
    if (!m_fitted || !anyDrive()) return 0xFF;       // no drives: the controller isn't there
    switch (port & 0x0F) {
    case 0x0: {
        uint8_t v = 0;
        for (int d = 0; d < 4; ++d)
            if (m_drive[d].present() && m_drive[d].writeProtected) v |= (uint8_t)((0x80 >> d) | 0x02);
        return v;
    }
    case 0x1: return m_control;
    case 0x8: return m_data;
    case 0x9: return m_error;
    case 0xA: return m_secCount;
    case 0xB: return m_sector;
    case 0xC: return (uint8_t)m_cyl;
    case 0xD: return (uint8_t)(m_cyl >> 8);
    case 0xE: return m_sdh;
    case 0xF: return m_status;
    default: return 0xFF;
    }
}

uint8_t HardDisk::in(uint8_t port) {
    if ((port & 0x0F) == 0x8 && m_fitted && anyDrive()) return dataIn();
    return peek(port);
}

void HardDisk::out(uint8_t port, uint8_t v) {
    if (!m_fitted) return;
    switch (port & 0x0F) {
    case 0x1:
        if (v & 0x10) reset();
        m_control = v;
        break;
    case 0x8: dataOut(v); break;
    case 0x9: break;                                // write precompensation
    case 0xA: m_secCount = v; break;
    case 0xB: m_sector = v; break;
    case 0xC: m_cyl = (uint16_t)((m_cyl & 0xFF00) | v); break;
    case 0xD: m_cyl = (uint16_t)((m_cyl & 0x00FF) | (v << 8)); break;
    case 0xE:
        m_sdh = v;
        // Ready even with no drive there (as xtrs: the 4P's boot ROM then
        // doesn't wait long for a missing drive 0)
        m_status = IDLE;
        break;
    case 0xF: command(v); break;
    default: break;
    }
}

bool HardDisk::findSector() {
    const Drive& d = m_drive[(m_sdh >> 3) & 3];
    const int head = m_sdh & 7;
    // Sectors 0-31 or 1-32 (both origins are used); the cylinder isn't
    // limited (as xtrs: some drivers probe past the header's count)
    if (!d.present() || head >= d.heads || m_sector > d.secs) {
        m_status = IDLE | ST_ERR;
        m_error = ER_NOTFOUND;
        m_done = -1;
        return false;
    }
    m_offset = HEADER + SECTOR * ((size_t)m_cyl * d.heads * d.secs + (size_t)head * d.secs + (m_sector % d.secs));
    return true;
}

void HardDisk::command(uint8_t v) {
    m_command = v;
    m_done = -1;
    m_error = 0;
    switch (v & 0xF0) {
    case 0x10:                                      // restore
        m_cyl = 0;
        m_status = IDLE;
        break;
    case 0x20:                                      // read
    case 0x30:                                      // write
        if (!findSector()) break;
        if ((v & 0xF0) == 0x30 && m_drive[(m_sdh >> 3) & 3].writeProtected) {
            m_status = IDLE | ST_ERR | 0x20;        // write fault
            m_error = ER_ABORT;
            break;
        }
        m_done = 0;
        m_status = IDLE | ST_DRQ;
        break;
    case 0x40:                                      // verify (scan ID)
    case 0x70:                                      // seek
        if (findSector()) m_status = IDLE;
        break;
    case 0x50:                                      // format a track: the sectors are already there
    case 0x60:                                      // init
        m_status = IDLE;
        break;
    default:
        m_status = IDLE | ST_ERR;
        m_error = ER_ABORT;
        break;
    }
}

void HardDisk::nextSector() {
    // A multiple-sector command goes on while the count lasts
    if ((m_command & 0x04) && m_secCount > 1) {
        --m_secCount;
        ++m_sector;
        if (findSector()) { m_done = 0; m_status = IDLE | ST_DRQ; return; }
        return;
    }
    if (m_command & 0x04) m_secCount = 0;
    m_done = -1;
    m_status = IDLE;
}

uint8_t HardDisk::dataIn() {
    if ((m_command & 0xF0) == 0x20 && m_done >= 0) {
        const Drive& d = m_drive[(m_sdh >> 3) & 3];
        const size_t at = m_offset + (size_t)m_done;
        m_data = at < d.image.size() ? d.image[at] : 0;
        if (++m_done >= (int)SECTOR) nextSector();
    }
    return m_data;
}

void HardDisk::dataOut(uint8_t v) {
    m_data = v;
    if ((m_command & 0xF0) != 0x30 || m_done < 0) return;
    Drive& d = m_drive[(m_sdh >> 3) & 3];
    const size_t at = m_offset + (size_t)m_done;
    if (at >= d.image.size()) d.image.resize(m_offset + SECTOR, 0);
    d.image[at] = v;
    // The directory cylinder (byte 2 of the first sector) is copied to the
    // header's byte 31, which the Reed emulator reads (as xtrs)
    if (m_cyl == 0 && (m_sdh & 7) == 0 && m_sector == 0 && m_done == 2) {
        d.image[31] = v;
        d.image[3] = checksum(d.image.data());
    }
    d.dirty = true;
    if (++m_done >= (int)SECTOR) nextSector();
}

}  // namespace trs80
