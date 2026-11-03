#include "atari_sio.h"
#include "plugins/devices/pokey/main/pokey.h"
#include <cstring>
#include <fstream>

namespace atari {

// ---------------------------------------------------------------------------
// ATR images
// ---------------------------------------------------------------------------

bool AtrImage::load(std::vector<uint8_t> bytes, std::string& err) {
    if (bytes.size() < 16 + 128 || bytes[0] != 0x96 || bytes[1] != 0x02) {
        err = "Not an ATR image (no $0296 header).";
        return false;
    }
    m_sectorSize = bytes[4] | (bytes[5] << 8);
    if (m_sectorSize != 128 && m_sectorSize != 256) {
        err = "Unsupported sector size " + std::to_string(m_sectorSize) + ".";
        return false;
    }
    size_t body = bytes.size() - 16;
    m_sectorCount = m_sectorSize == 128 ? (int)(body / 128)
                                        : (body <= 384 ? (int)(body / 128) : 3 + (int)((body - 384) / 256));
    m_data = std::move(bytes);
    return true;
}

size_t AtrImage::offsetOf(int n) const {
    if (n <= 3) return 16 + (size_t)(n - 1) * 128;
    return 16 + 3 * 128 + (size_t)(n - 4) * m_sectorSize;
}

bool AtrImage::read(int n, uint8_t* out) const {
    if (n < 1 || n > m_sectorCount) return false;
    size_t off = offsetOf(n);
    if (off + sizeOf(n) > m_data.size()) return false;
    std::memcpy(out, &m_data[off], sizeOf(n));
    return true;
}

bool AtrImage::write(int n, const uint8_t* data) {
    if (n < 1 || n > m_sectorCount) return false;
    size_t off = offsetOf(n);
    if (off + sizeOf(n) > m_data.size()) return false;
    std::memcpy(&m_data[off], data, sizeOf(n));
    return true;
}

// ---------------------------------------------------------------------------
// XEX boot disks
// ---------------------------------------------------------------------------

namespace {

// Boot loader at $0700 (3 sectors). Header: flags, 3 boot sectors, load
// $0700, DOSINI = an RTS. The code (from $0706, which the OS calls after
// loading the boot sectors) reads 128-byte sectors from sector 4 on through
// DSKINV ($E453) into $0600 and parses a stream of segments
// (start, end, data; $FFFF ends it): INITAD ($2E2) is pointed at an RTS
// before each segment and called after it; at the end it jumps to RUNAD
// ($2E0), or returns with carry clear. Its variables are at $087A-$087F
// (not zero page, which INIT routines may use); the copy pointer is $D4.
const uint8_t XEX_BOOT_LOADER[] = {
    0x00, 0x03, 0x00, 0x07, 0x79, 0x07, 0xA9, 0x00, 0x8D, 0xE0, 0x02, 0x8D, 0xE1, 0x02, 0xA9,
    0x80, 0x8D, 0x7E, 0x08, 0xA9, 0x04, 0x8D, 0x7C, 0x08, 0xA9, 0x00, 0x8D, 0x7D, 0x08, 0x20,
    0x7A, 0x07, 0x85, 0xD4, 0x20, 0x7A, 0x07, 0x85, 0xD5, 0x20, 0x7A, 0x07, 0x8D, 0x7A, 0x08,
    0x20, 0x7A, 0x07, 0x8D, 0x7B, 0x08, 0xA5, 0xD4, 0x25, 0xD5, 0xC9, 0xFF, 0xD0, 0x0D, 0xAD,
    0xE0, 0x02, 0x0D, 0xE1, 0x02, 0xF0, 0x03, 0x6C, 0xE0, 0x02, 0x18, 0x60, 0xA9, 0x79, 0x8D,
    0xE2, 0x02, 0xA9, 0x07, 0x8D, 0xE3, 0x02, 0x20, 0x7A, 0x07, 0xA0, 0x00, 0x91, 0xD4, 0xA5,
    0xD4, 0xCD, 0x7A, 0x08, 0xD0, 0x07, 0xA5, 0xD5, 0xCD, 0x7B, 0x08, 0xF0, 0x09, 0xE6, 0xD4,
    0xD0, 0xE7, 0xE6, 0xD5, 0x4C, 0x52, 0x07, 0x20, 0x76, 0x07, 0x4C, 0x1D, 0x07, 0x6C, 0xE2,
    0x02, 0x60, 0xAE, 0x7E, 0x08, 0xE0, 0x80, 0xD0, 0x32, 0xA9, 0x31, 0x8D, 0x00, 0x03, 0xA9,
    0x01, 0x8D, 0x01, 0x03, 0xA9, 0x52, 0x8D, 0x02, 0x03, 0xA9, 0x00, 0x8D, 0x04, 0x03, 0xA9,
    0x06, 0x8D, 0x05, 0x03, 0xAD, 0x7C, 0x08, 0x8D, 0x0A, 0x03, 0xAD, 0x7D, 0x08, 0x8D, 0x0B,
    0x03, 0x20, 0x53, 0xE4, 0xEE, 0x7C, 0x08, 0xD0, 0x03, 0xEE, 0x7D, 0x08, 0xA2, 0x00, 0xBD,
    0x00, 0x06, 0xE8, 0x8E, 0x7E, 0x08, 0x60
};

}  // namespace

bool xexToAtr(const std::vector<uint8_t>& xex, std::vector<uint8_t>& atr, std::string& err) {
    if (xex.size() < 6 || xex[0] != 0xFF || xex[1] != 0xFF) {
        err = "Not an Atari executable (no $FFFF header).";
        return false;
    }
    // Segments, without the $FFFF markers; then $FFFF $FFFF to end
    std::vector<uint8_t> stream;
    size_t i = 0;
    while (i + 4 <= xex.size()) {
        uint16_t start = (uint16_t)(xex[i] | (xex[i + 1] << 8));
        if (start == 0xFFFF) { i += 2; continue; }
        uint16_t end = (uint16_t)(xex[i + 2] | (xex[i + 3] << 8));
        if (end < start) { err = "Bad segment in the executable."; return false; }
        size_t len = (size_t)end - start + 1;
        if (i + 4 + len > xex.size()) len = xex.size() - (i + 4);   // truncated file: load what is there
        if (len == 0) break;
        uint16_t realEnd = (uint16_t)(start + len - 1);
        stream.insert(stream.end(), { (uint8_t)start, (uint8_t)(start >> 8),
                                      (uint8_t)realEnd, (uint8_t)(realEnd >> 8) });
        stream.insert(stream.end(), xex.begin() + (long)(i + 4), xex.begin() + (long)(i + 4 + len));
        i += 4 + len;
    }
    if (stream.empty()) { err = "The executable has no segments."; return false; }
    stream.insert(stream.end(), { 0xFF, 0xFF, 0xFF, 0xFF });

    size_t dataSectors = (stream.size() + 127) / 128;
    size_t sectors = 3 + dataSectors;
    if (sectors < 720) sectors = 720;   // a standard single-density disk
    atr.assign(16 + sectors * 128, 0);
    size_t paragraphs = sectors * 128 / 16;
    atr[0] = 0x96; atr[1] = 0x02;
    atr[2] = (uint8_t)paragraphs; atr[3] = (uint8_t)(paragraphs >> 8);
    atr[4] = 128; atr[5] = 0;
    atr[6] = (uint8_t)(paragraphs >> 16);
    std::memcpy(&atr[16], XEX_BOOT_LOADER, sizeof(XEX_BOOT_LOADER));
    std::memcpy(&atr[16 + 3 * 128], stream.data(), stream.size());
    return true;
}

// ---------------------------------------------------------------------------
// The SIO bus and its drives
// ---------------------------------------------------------------------------

AtariSio::AtariSio(POKEY* pokey) : m_pokey(pokey) {
    m_cmdLine.sio = this;
}

void AtariSio::reset() {
    m_frame.clear();
    m_frameReady = false;
    m_writeDrive = -1;
    m_writeData.clear();
    m_out.clear();
    m_wait = 0;
    m_cmdLine.level = true;
}

void AtariSio::CmdLine::set(bool l) {
    bool was = level;
    level = l;
    if (!l && was) sio->m_frame.clear();            // command line asserted: a new frame
    if (l && !was && sio->m_frameReady) {           // released after a frame: answer it
        sio->m_frameReady = false;
        sio->command();
    }
}

uint8_t AtariSio::checksum(const uint8_t* d, int n) {
    unsigned sum = 0;
    for (int i = 0; i < n; ++i) {
        sum += d[i];
        if (sum > 0xFF) sum = (sum & 0xFF) + 1;     // end-around carry
    }
    return (uint8_t)sum;
}

void AtariSio::send(uint8_t byte, int delay) {
    m_out.push_back({delay, byte});
}

void AtariSio::sendFrame(const uint8_t* data, int n, int firstDelay) {
    for (int i = 0; i < n; ++i) send(data[i], i == 0 ? firstDelay : BYTE_CYCLES);
    send(checksum(data, n), BYTE_CYCLES);
}

void AtariSio::serialOut(uint8_t byte) {
    if (!m_cmdLine.level) {                          // command frame
        if (m_frame.size() < 5) m_frame.push_back(byte);
        if (m_frame.size() == 5) m_frameReady = checksum(m_frame.data(), 4) == m_frame[4];
        return;
    }
    if (m_writeDrive >= 0) {                         // a write command's data frame
        m_writeData.push_back(byte);
        if ((int)m_writeData.size() == m_writeLen + 1) {
            bool ok = checksum(m_writeData.data(), m_writeLen) == m_writeData[m_writeLen];
            Drive& d = m_drives[m_writeDrive];
            send(ok ? 'A' : 'N', 1000);
            if (ok) send(writeSector(d, m_writeSector, m_writeData.data()) ? 'C' : 'E', 2000);
            m_writeDrive = -1;
            m_writeData.clear();
        }
    }
}

void AtariSio::command() {
    uint8_t dev = m_frame[0], cmd = m_frame[1];
    int sector = m_frame[2] | (m_frame[3] << 8);
    if (dev < 0x31 || dev > 0x34) return;            // not a disk drive: nobody answers
    int n = dev - 0x31;
    Drive& d = m_drives[n];
    if (!d.present) return;                          // no drive there
    m_activeDrive = n;
    if (cmd == 'R' || cmd == 'W' || cmd == 'P') d.lastSector = sector;

    const int ACK_DELAY = 2000, COMPLETE_DELAY = 3000;
    switch (cmd) {
        case 'R': {
            send('A', ACK_DELAY);
            uint8_t buf[256];
            int len = d.image.sizeOf(sector);
            if (!d.image.read(sector, buf)) {
                std::memset(buf, 0, sizeof(buf));
                send('E', COMPLETE_DELAY);
                sendFrame(buf, len, BYTE_CYCLES);
            } else {
                send('C', COMPLETE_DELAY);
                sendFrame(buf, len, BYTE_CYCLES);
            }
            break;
        }
        case 'S': {
            send('A', ACK_DELAY);
            // Drive status (bit 5: double density), FDC status (inverted: $FF
            // = fine), format timeout, unused
            // Drive status: bit 5 double density, bit 3 write protected
            uint8_t st[4] = { (uint8_t)((d.image.sectorSize() == 256 ? 0x20 : 0x00) | (d.readOnly ? 0x08 : 0x00)),
                              0xFF, 0xE0, 0x00 };
            send('C', COMPLETE_DELAY);
            sendFrame(st, 4, BYTE_CYCLES);
            break;
        }
        case 'W': case 'P':
            send('A', ACK_DELAY);
            m_writeDrive = n;
            m_writeSector = sector;
            m_writeLen = d.image.sizeOf(sector);
            m_writeData.clear();
            break;
        default:
            send('N', ACK_DELAY);                    // not supported (format, PERCOM...)
            break;
    }
}

bool AtariSio::writeSector(Drive& d, int sector, const uint8_t* data) {
    if (d.readOnly) return false;
    if (!d.image.write(sector, data)) return false;
    if (d.path.empty()) return true;
    // Write through to the file
    std::fstream f(d.path, std::ios::in | std::ios::out | std::ios::binary);
    if (f) {
        f.seekp((std::streamoff)d.image.offsetOf(sector));
        f.write(reinterpret_cast<const char*>(data), d.image.sizeOf(sector));
    }
    if (!f) {
        m_lastError = "Could not save sector " + std::to_string(sector) + " to " + d.path + ".";
        return false;
    }
    return true;
}

void AtariSio::tick(uint64_t cycles) {
    if (m_out.empty() && m_writeDrive < 0) m_activeDrive = -1;
    while (cycles > 0 && !m_out.empty()) {
        Pending& p = m_out.front();
        if ((uint64_t)p.delay > cycles) { p.delay -= (int)cycles; return; }
        cycles -= (uint64_t)p.delay;
        uint8_t b = p.byte;
        m_out.pop_front();
        m_pokey->receiveSerial(b);
    }
}

bool AtariSio::mount(int drive, std::vector<uint8_t> atrBytes, const std::string& label, std::string& err,
                     const std::string& path) {
    if (drive < 0 || drive > 3) { err = "Drive must be 1-4."; return false; }
    AtrImage img;
    if (!img.load(std::move(atrBytes), err)) return false;
    Drive& d = m_drives[drive];
    d = Drive();
    d.image = std::move(img);
    d.present = true;
    d.label = label;
    d.path = path;
    if (!path.empty()) {
        std::fstream probe(path, std::ios::in | std::ios::out | std::ios::binary);
        d.readOnly = !probe;
    }
    return true;
}

bool AtariSio::mountDisk(int unit, const std::string& path) {
    if (unit < 1 || unit > 4) return false;
    std::ifstream f(path, std::ios::binary);
    if (!f) { m_lastError = "Cannot open " + path + "."; return false; }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return mount(unit - 1, std::move(bytes), path, m_lastError, path);
}

void AtariSio::ejectDisk(int unit) {
    if (unit >= 1 && unit <= 4) unmount(unit - 1);
}

bool AtariSio::getDiskStatus(int unit, int& track, int& sector, bool& led) {
    if (unit < 1 || unit > 4) return false;
    const Drive& d = m_drives[unit - 1];
    track = 0;
    sector = d.lastSector;
    led = m_activeDrive == unit - 1;
    return true;
}

std::string AtariSio::getMountedDiskPath(int unit) {
    if (unit < 1 || unit > 4 || !m_drives[unit - 1].present) return "";
    return m_drives[unit - 1].label;
}

void AtariSio::unmount(int drive) {
    if (drive >= 0 && drive < 4) m_drives[drive] = Drive();
}

}  // namespace atari
