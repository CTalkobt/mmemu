// BBC Micro discs: DFS .ssd/.dsd images and the Intel 8271, after b-em
// (i8271.c, disc.c, sdf-acc.c; GPL-2.0-or-later)

#include "bbc_disc.h"
#include "libdevices/main/disk_file.h"
#include <algorithm>
#include <cctype>
#include <fstream>

namespace bbc {

// ---------------------------------------------------------------------------
// DfsImage
// ---------------------------------------------------------------------------
std::unique_ptr<DfsImage> DfsImage::open(const std::string& path, std::string* err) {
    // Plain, .gz or .zip (#233): the format comes from the image's own name
    const std::string ext = diskfile::imageExt(path);
    if (ext != "ssd" && ext != "dsd") {
        if (err) *err = "not a DFS disc image (.ssd, .dsd): " + path;
        return nullptr;
    }
    std::vector<uint8_t> data;
    if (!diskfile::read(path, data, err)) return nullptr;
    auto img = fromBytes(std::move(data), ext == "dsd");
    img->m_path = path;
    // Read-only files (and zips holding more than the disc) open write-protected
    if (!diskfile::writable(path)) img->m_protect = true;
    return img;
}

std::unique_ptr<DfsImage> DfsImage::fromBytes(std::vector<uint8_t> data, bool doubleSided) {
    auto img = std::unique_ptr<DfsImage>(new DfsImage());
    img->m_sides = doubleSided ? 2 : 1;
    const size_t perTrack = (size_t)TRACK_SIZE * img->m_sides;
    // 40 or 80 tracks: a short image is padded to the next of those
    const size_t tracks = (data.size() + perTrack - 1) / perTrack;
    img->m_tracks = tracks <= 40 ? 40 : 80;
    data.resize(perTrack * (size_t)img->m_tracks, 0);
    img->m_data = std::move(data);
    return img;
}

long DfsImage::offset(int track, int side, int sector) const {
    if (track < 0 || track >= m_tracks || side < 0 || side >= m_sides || sector < 0 || sector >= SECTORS)
        return -1;
    return ((long)track * m_sides + side) * TRACK_SIZE + (long)sector * SECTOR_SIZE;
}

void DfsImage::write(long at, uint8_t v) {
    if (m_data[(size_t)at] != v) {
        m_data[(size_t)at] = v;
        m_dirty = true;
    }
}

void DfsImage::flush() {
    if (!m_dirty || m_path.empty()) return;
    diskfile::save(m_path, m_data);
    m_dirty = false;
}

// ---------------------------------------------------------------------------
// I8271
// ---------------------------------------------------------------------------
namespace {
// Status register bits
constexpr uint8_t S_BUSY = 0x80, S_RES_FULL = 0x10;
constexpr uint8_t S_INT = 0x08, S_NON_DMA = 0x04;
// Drive control output
constexpr uint8_t SIDESEL = 0x20, DRIVESEL0 = 0x40, DRIVESEL1 = 0x80, DRIVESEL = 0xC0;
constexpr int BYTE_CYCLES = 16 * 17;    // b-em: a disc poll every 16 cycles, a byte every 17th
constexpr int SPIN_UNIT = 128;          // b-em's motorspin counts 2 MHz / 128
}  // namespace

I8271::I8271() { reset(); }

I8271::~I8271() {
    for (auto& d : m_drive) if (d.image) d.image->flush();
}

void I8271::reset() {
    // i8271_reset
    m_motorSpin = 45000LL * SPIN_UNIT;
    m_paramNum = m_paramReq = 0;
    m_status = 0;
    m_fdcTime = 0;
    m_curTrack[0] = m_curTrack[1] = 0;
    m_command = 0xFF;
    m_state = IDLE;
    nmi();
}

void I8271::nmi() {
    if (onNmi) onNmi(m_status & S_INT);
}

void I8271::spinUp() {
    m_motorSpin = 0;
    m_motorOn = true;
}

void I8271::spinDown() {
    if (m_motorOn) {
        m_motorOn = false;
        for (auto& d : m_drive) if (d.image) d.image->flush();
    }
    m_drvout &= (uint8_t)~DRIVESEL;
}

void I8271::setSpinDown() {
    m_motorSpin = (int64_t)m_unloadRevs * 3125 * SPIN_UNIT;
}

void I8271::fail(uint8_t result) {
    // notfound, crc error, write protect: the result with an interrupt,
    // and a short spin-down
    m_result = result;
    m_status = S_RES_FULL | S_INT;
    nmi();
    m_motorSpin = 15000LL * SPIN_UNIT;
    m_fdcTime = 0;
}

uint8_t I8271::read(uint16_t addr) {
    switch (addr & 7) {
        case 0: return m_status;
        case 1:
            m_status &= (uint8_t)~(S_RES_FULL | S_INT);
            nmi();
            return m_result;
        case 4:
            m_status &= (uint8_t)~(S_INT | S_NON_DMA);
            nmi();
            return m_data;
    }
    return 0;
}

void I8271::seek() {
    // disc_seekrelative / disc_seek0: the head moves by the difference from
    // the 8271's idea of the current track
    Drive& d = m_drive[m_cur];
    const int newTrack = m_params[0];
    const int steps = newTrack ? newTrack - m_curTrack[m_cur] : -d.track;
    if (steps) {
        m_fdcTime = (int64_t)std::abs(steps) * m_stepTime + m_settleTime;
        if (m_fdcTime <= 0) m_fdcTime = 200;
        d.track = std::max(0, d.track + steps);
    } else {
        m_fdcTime = 200;
    }
}

void I8271::prepOp(int sectors, int sectorSize, bool deleted) {
    m_curSector = m_params[1];
    m_sectorsLeft = sectors;
    m_sectorSize = sectorSize;
    m_deleted = deleted;
    spinUp();
    m_phase = 0;
    seek();
}

void I8271::driveStatus() {
    const Drive& d = m_drive[m_cur];
    unsigned r = 0x80;
    if (m_drvout & DRIVESEL1) r |= 0x40;
    if (m_motorOn && m_index) r |= 0x10;
    if (d.image && d.image->writeProtected()) r |= 0x08;
    if (m_drvout & DRIVESEL0) r |= 0x04;
    if (d.track == 0) r |= 0x02;
    m_paramReq = 0;
    m_result = (uint8_t)r;
    m_status = S_RES_FULL;
}

void I8271::write(uint16_t addr, uint8_t val) {
    switch (addr & 7) {
        case 0:                                     // command
            if (m_status & S_BUSY) return;
            m_command = val & 0x3F;
            if (m_command == 0x17) m_command = 0x13;
            // Only commands below Read Drive Status change the drive selects
            if (m_command < 0x2C) m_drvout = (uint8_t)((m_drvout & ~DRIVESEL) | (val & DRIVESEL));
            m_cur = (val & 0x80) ? 1 : 0;
            m_paramNum = 0;
            m_status = S_BUSY;
            switch (m_command) {
                case 0x2C: driveStatus(); break;
                case 0x29: case 0x3D: m_paramReq = 1; break;
                case 0x0A: case 0x0E: case 0x12: case 0x16: case 0x1E: case 0x3A: m_paramReq = 2; break;
                case 0x0B: case 0x0F: case 0x13: case 0x1F: case 0x1B: m_paramReq = 3; break;
                case 0x35: m_paramReq = 4; break;
                case 0x00: case 0x04: case 0x23: m_paramReq = 5; break;
                default:
                    m_paramReq = 0;
                    m_result = 0x18;
                    m_status = S_RES_FULL | S_INT;
                    nmi();
                    m_fdcTime = 0;
            }
            break;
        case 1:                                     // parameter
            if (m_paramNum < 5) m_params[m_paramNum++] = val;
            if (m_paramNum == m_paramReq) command();
            break;
        case 2:                                     // reset
            if (val & 1) reset();
            break;
        case 4:                                     // data
            m_data = val;
            m_written = true;
            m_status &= (uint8_t)~(S_INT | S_NON_DMA);
            nmi();
            break;
    }
}

void I8271::command() {
    auto bad = [&]() {
        m_result = 0x18;
        m_status = S_RES_FULL | S_INT;
        nmi();
        m_fdcTime = 0;
    };
    const int multi = m_params[2] & 31, size = 128 << ((m_params[2] >> 5) & 7);
    switch (m_command) {
        case 0x0A: case 0x12: prepOp(1, 128, false); break;             // one 128-byte sector
        case 0x0B: case 0x13: prepOp(multi, size, false); break;        // variable / several
        case 0x0E: case 0x16: prepOp(1, 128, true); break;              // with deleted data
        case 0x0F: prepOp(multi, size, true); break;
        case 0x1E: prepOp(1, 0, true); break;                           // verify
        case 0x1F: prepOp(multi, 0, true); break;
        case 0x1B: prepOp(multi, 4, false); break;                      // read ID
        case 0x23:                                                      // format
            spinUp();
            m_phase = 0;
            seek();
            break;
        case 0x29:                                                      // seek
            seek();
            spinUp();
            break;
        case 0x35:                                                      // specify
            if (m_params[0] == 0x0D) {
                m_stepTime = m_params[1] * 2000u;
                m_settleTime = m_params[2] * 2000u;
                m_unloadRevs = m_params[3] >> 4;
            }
            m_status = 0;
            break;
        case 0x3A:                                                      // write special register
            m_status = 0;
            switch (m_params[0]) {
                case 0x12: m_curTrack[0] = m_params[1]; break;
                case 0x17: break;                                       // mode
                case 0x1A: m_curTrack[1] = m_params[1]; break;
                case 0x23: m_drvout = m_params[1]; break;               // drive control output
                default: bad();
            }
            break;
        case 0x3D:                                                      // read special register
            m_status = S_RES_FULL;
            m_result = 0;
            switch (m_params[0]) {
                case 0x06: m_result = 0; break;
                case 0x12: m_result = (uint8_t)m_curTrack[0]; break;
                case 0x1A: m_result = (uint8_t)m_curTrack[1]; break;
                case 0x23: m_result = m_drvout; break;
                default: bad();
            }
            break;
        default:
            bad();
    }
}

// The disc side (sdf-acc): a sector found starts its bytes; none: "not found"
// after 500 byte times
void I8271::startRead(int sector) {
    if (m_state != IDLE) return;
    const Drive& d = m_drive[m_cur];
    const long at = d.image ? d.image->offset(m_params[0], side(), sector) : -1;
    if (at < 0) { m_count = 500; m_state = NOTFOUND; return; }
    m_at = at;
    m_count = DfsImage::SECTOR_SIZE;
    m_state = READSECTOR;
}

void I8271::startWrite(int sector) {
    if (m_state != IDLE) return;
    const Drive& d = m_drive[m_cur];
    const long at = d.image ? d.image->offset(m_params[0], side(), sector) : -1;
    if (at < 0) { m_count = 500; m_state = NOTFOUND; return; }
    m_at = at;
    m_count = DfsImage::SECTOR_SIZE;
    m_pollDiv = -20;                                // b-em: sdf_time = -20
    m_state = WRITESECTOR;
}

void I8271::data(uint8_t v) {
    if (m_bytesLeft) {
        --m_bytesLeft;
        m_data = v;
        m_status = S_BUSY | S_INT | S_NON_DMA;
        m_result = 0;
        nmi();
    }
}

int I8271::getData(bool last) {
    if (!m_written) return -1;
    if (!last) {
        m_status = S_BUSY | S_INT | S_NON_DMA;
        m_result = 0;
        nmi();
    }
    m_written = false;
    return m_data;
}

void I8271::finishRead() { m_fdcTime = 200; }

void I8271::callback() {
    m_fdcTime = 0;
    switch (m_command) {
        case 0x0A: case 0x0B: case 0x0E: case 0x0F:            // write
            if (!m_phase) {
                m_curTrack[m_cur] = m_params[0];
                startWrite(m_curSector);
                m_phase = 1;
                m_status = S_BUSY | S_INT | S_NON_DMA;
                m_result = 0;
                nmi();
                return;
            }
            if (--m_sectorsLeft == 0) {
                m_status = S_RES_FULL | S_INT;
                m_result = 0;
                nmi();
                setSpinDown();
                return;
            }
            m_curSector++;
            m_bytesLeft = m_sectorSize;
            startWrite(m_curSector);
            m_status = S_BUSY | S_INT | S_NON_DMA;
            m_result = 0;
            nmi();
            break;
        case 0x12: case 0x13: case 0x16: case 0x1E: case 0x1F:  // read, verify
            if (!m_phase) {
                m_curTrack[m_cur] = m_params[0];
                m_bytesLeft = m_sectorSize;
                startRead(m_curSector);
                m_phase = 1;
                return;
            }
            if (--m_sectorsLeft == 0) {
                m_status = S_RES_FULL | S_INT;
                m_result = 0;
                nmi();
                setSpinDown();
                return;
            }
            m_curSector++;
            m_bytesLeft = m_sectorSize;
            startRead(m_curSector);
            break;
        case 0x1B:                                              // read ID
            if (!m_phase) {
                m_curTrack[m_cur] = m_params[0];
                m_bytesLeft = 4;
                m_phase = 1;
            } else {
                if (--m_sectorsLeft == 0) {
                    m_status = S_RES_FULL | S_INT;
                    m_result = 0;
                    nmi();
                    setSpinDown();
                    return;
                }
                m_bytesLeft = 4;
            }
            if (m_state == IDLE) {
                const Drive& d = m_drive[m_cur];
                if (d.image && side() < d.image->sides()) {
                    m_idTrack = d.track;
                    m_formatByte = 0;
                    m_state = READADDR;
                } else {
                    m_count = 500;
                    m_state = NOTFOUND;
                }
            }
            break;
        case 0x23:                                              // format
            if (!m_phase) {
                m_curTrack[m_cur] = m_params[0];
                const Drive& d = m_drive[m_cur];
                if (m_state == IDLE) {
                    if (d.image && d.image->offset(d.track, side(), 0) >= 0) {
                        m_formatSectors = m_params[2] & 0x1F;
                        m_idSector = 0;
                        m_formatByte = 0;
                        m_state = FORMAT;
                    } else {
                        m_count = 500;
                        m_state = NOTFOUND;
                    }
                }
                m_status = S_BUSY | S_INT | S_NON_DMA;
                m_phase = 1;
            } else {
                m_status = S_RES_FULL | S_INT;
                m_result = 0;
            }
            nmi();
            break;
        case 0x29:                                              // seek
            m_curTrack[m_cur] = m_params[0];
            m_status = S_RES_FULL | S_INT;
            m_result = 0;
            nmi();
            setSpinDown();
            break;
        default:
            break;
    }
}

// A byte time (sdf_poll)
void I8271::poll() {
    if (++m_posn == 1) m_index = true;
    else if (m_posn == 4) m_index = false;
    else if (m_posn >= 1600) m_posn = 0;

    Drive& d = m_drive[m_cur];
    switch (m_state) {
        case IDLE:
            break;
        case NOTFOUND:
            if (--m_count == 0) {
                fail(0x18);
                m_state = IDLE;
            }
            break;
        case READSECTOR:
            data(d.image ? d.image->read(m_at++) : 0xE5);
            if (--m_count == 0) {
                finishRead();
                m_state = IDLE;
            }
            break;
        case WRITESECTOR: {
            if (!d.image || d.image->writeProtected()) {
                fail(0x12);
                m_state = IDLE;
                break;
            }
            const int c = getData(--m_count == 0);
            if (c < 0) {
                m_count++;                                  // data underrun: wait
                break;
            }
            d.image->write(m_at++, (uint8_t)c);
            if (m_count == 0) {
                d.image->flush();
                finishRead();
                m_state = IDLE;
            }
            break;
        }
        case READADDR:
            // track, side, sector, size code (1: 256 bytes), two CRC bytes
            switch (m_formatByte++) {
                case 0: data((uint8_t)m_idTrack); break;
                case 1: data((uint8_t)side()); break;
                case 2: data((uint8_t)m_idSector); break;
                case 3: data(1); break;
                case 4: case 5: data(0); break;
                default:
                    m_state = IDLE;
                    finishRead();
                    if (++m_idSector == DfsImage::SECTORS) m_idSector = 0;
                    break;
            }
            break;
        case FORMAT: {
            // Four ID bytes for each sector from the CPU; the sector filled with &E5
            if (d.image && d.image->writeProtected()) {
                fail(0x12);
                m_state = IDLE;
                break;
            }
            const int c = getData(false);
            if (c < 0) break;
            if (++m_formatByte == 4) {
                m_formatByte = 0;
                const long at = d.image->offset(d.track, side(), m_idSector);
                if (at >= 0)
                    for (int i = 0; i < DfsImage::SECTOR_SIZE; i++) d.image->write(at + i, 0xE5);
                if (++m_idSector >= m_formatSectors) {
                    d.image->flush();
                    m_state = IDLE;
                    finishRead();
                }
            }
            break;
        }
    }
}

void I8271::clock(uint64_t cycles) {
    while (cycles) {
        // b-em counts the motor down always; the controller's timer and the
        // disc only while the motor runs
        uint64_t step = cycles;
        if (m_motorSpin > 0) step = std::min<uint64_t>(step, (uint64_t)m_motorSpin);
        if (m_motorOn) {
            if (m_fdcTime > 0) step = std::min<uint64_t>(step, (uint64_t)m_fdcTime);
            const int toPoll = 16 - m_pollTime;
            step = std::min<uint64_t>(step, (uint64_t)std::max(1, toPoll));
        }
        cycles -= step;
        if (m_motorSpin > 0) {
            m_motorSpin -= (int64_t)step;
            if (m_motorSpin <= 0) { m_motorSpin = 0; spinDown(); }
        }
        if (!m_motorOn) continue;
        if (m_fdcTime > 0) {
            m_fdcTime -= (int64_t)step;
            if (m_fdcTime <= 0) callback();
        }
        m_pollTime += (int)step;
        if (m_pollTime >= 16) {
            m_pollTime -= 16;
            if (++m_pollDiv > 16) {                         // sdf_poll's own divider
                m_pollDiv = 0;
                poll();
            }
        }
    }
}

bool I8271::insert(int drive, std::unique_ptr<DfsImage> image) {
    Drive& d = m_drive[drive & 1];
    if (d.image) d.image->flush();
    d.image = std::move(image);
    return d.image != nullptr;
}

bool I8271::mountDisk(int unit, const std::string& path) {
    if (unit != 0 && unit != 1) return false;
    auto img = DfsImage::open(path);
    if (!img) return false;
    return insert(unit, std::move(img));
}

void I8271::ejectDisk(int unit) {
    if (unit != 0 && unit != 1) return;
    insert(unit, nullptr);
}

bool I8271::getDiskStatus(int unit, int& track, int& sector, bool& led) {
    if (unit != 0 && unit != 1) return false;
    track = m_drive[unit].track;
    sector = m_curSector;
    led = m_motorOn && m_cur == unit;
    return true;
}

std::string I8271::getMountedDiskPath(int unit) {
    if (unit != 0 && unit != 1) return {};
    const auto& img = m_drive[unit].image;
    return img ? img->path() : std::string();
}

}  // namespace bbc
