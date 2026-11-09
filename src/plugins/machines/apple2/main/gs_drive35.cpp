// Apple 3.5" drive for the IIgs's IWM, after KEGS by Kent Dickey (GPL-3.0;
// iwm.c): the drive's status registers and commands (iwm_read_status35,
// iwm_do_action35), the track format (iwm_nibblize_track_35, a port of the
// ROM's own code at $FF:6320) and its decoding (iwm_denib_track35).

#include "gs_drive35.h"
#include "libdevices/main/disk_file.h"
#include <algorithm>
#include <cctype>
#include <cstring>

namespace apple2 {

namespace {

// The 6-and-2 write translate table (KEGS to_disk_byte)
const uint8_t TO_DISK[64] = {
    0x96, 0x97, 0x9A, 0x9B, 0x9D, 0x9E, 0x9F, 0xA6, 0xA7, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF, 0xB2, 0xB3,
    0xB4, 0xB5, 0xB6, 0xB7, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF, 0xCB, 0xCD, 0xCE, 0xCF, 0xD3,
    0xD6, 0xD7, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE, 0xDF, 0xE5, 0xE6, 0xE7, 0xE9, 0xEA, 0xEB, 0xEC,
    0xED, 0xEE, 0xEF, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF};

struct FromDisk {
    uint32_t t[256];
    FromDisk() {
        for (auto& v : t) v = 0x100;                    // not a disk byte
        for (int i = 0; i < 64; ++i) t[TO_DISK[i]] = (uint32_t)i;
    }
};
const FromDisk FROM_DISK;

std::string lowerExt(const std::string& path) {
    const auto dot = path.find_last_of('.');
    std::string e = dot == std::string::npos ? "" : path.substr(dot);
    for (auto& c : e) c = (char)std::tolower((unsigned char)c);
    return e;
}

}  // namespace

int Gs35Drive::firstBlock(int track, int side) {
    int b = 0;
    for (int t = 0; t < track; ++t) b += 2 * sectorsOn(t);
    return b + side * sectorsOn(track);
}

// iwm_nibblize_track_35
void Gs35Drive::nibblize(const uint8_t* sectors, int track, int side, std::vector<uint8_t>& nib,
                         std::vector<uint8_t>& bits) {
    nib.clear();
    bits.clear();
    auto out = [&](uint32_t v, int size) { nib.push_back((uint8_t)v); bits.push_back((uint8_t)size); };
    const int num = sectorsOn(track);
    int physToLog[16];
    for (int i = 0; i < num; ++i) physToLog[i] = -1;
    int phys = 0;
    for (int log = 0; log < num; ++log) {                // 2:1 interleave
        while (physToLog[phys] >= 0) if (++phys >= num) phys = 0;
        physToLog[phys] = log;
        phys += 2;
        if (phys >= num) phys -= num;
    }
    uint32_t c00[0x100], d00[0x100], e00[0x100];
    for (phys = 0; phys < num; ++phys) {
        const int log = physToLog[phys];
        const int sync = phys == 0 ? 400 : 54;
        for (int i = 0; i < sync; ++i) out(0xFF, 10);
        out(0xD5, 8); out(0xAA, 8); out(0x96, 8);          // address field
        const uint32_t pTrack = (uint32_t)track & 0x3F, pSide = (uint32_t)(side << 5) + (uint32_t)(track >> 6);
        const uint32_t capacity = 0x22;
        out(TO_DISK[pTrack], 8);
        out(TO_DISK[log], 8);
        out(TO_DISK[pSide], 8);
        out(TO_DISK[capacity], 8);
        out(TO_DISK[(pTrack ^ (uint32_t)log ^ pSide ^ capacity) & 0x3F], 8);
        out(0xDE, 8); out(0xAA, 8);
        for (int i = 0; i < 5; ++i) out(0xFF, 10);
        out(0xD5, 8); out(0xAA, 8); out(0xAD, 8);          // data field
        out(TO_DISK[log], 8);

        const uint8_t* buf = sectors + log * 512;
        uint32_t t5c = 0, t5d = 0, t5e = 0, carry, val;
        int y = 0, x = 0xAF;
        c00[0] = d00[0] = e00[0] = e00[1] = 0;
        for (int k = 4; k > 0; --k) { c00[x] = d00[x] = e00[x] = 0; --x; }   // the 12 tag bytes: zero
        while (x >= 0) {
            t5c <<= 1;
            carry = t5c >> 8;
            t5c = (t5c + carry) & 0xFF;
            val = buf[y];
            t5e = val + t5e + carry;
            carry = t5e >> 8;
            t5e &= 0xFF;
            c00[x] = val ^ t5c;
            ++y;
            val = buf[y];
            t5d = t5d + val + carry;
            carry = t5d >> 8;
            t5d &= 0xFF;
            d00[x] = val ^ t5e;
            ++y;
            --x;
            if (x <= 0) break;
            val = buf[y];
            t5c = t5c + val + carry;
            carry = t5c >> 8;
            t5c &= 0xFF;
            e00[x + 1] = val ^ t5d;
            ++y;
        }
        val = ((t5c >> 2) ^ t5d) & 0x3F;
        val = (val ^ t5d) >> 2;
        val = (val ^ t5e) & 0x3F;
        val = (val ^ t5e) >> 2;
        const uint32_t t5f = val;
        uint32_t t63 = 0, t64 = 0, t65 = 0, hi = 0;
        y = 0xAE;
        while (y >= 0) {
            out(TO_DISK[hi & 0x3F], 8);
            out(TO_DISK[t63 & 0x3F], 8);
            t63 = c00[y];
            hi = t63 >> 6;
            out(TO_DISK[t64 & 0x3F], 8);
            t64 = d00[y];
            hi = (hi << 2) + (t64 >> 6);
            --y;
            if (y < 0) break;
            out(TO_DISK[t65 & 0x3F], 8);
            t65 = e00[y + 1];
            hi = (hi << 2) + (t65 >> 6);
        }
        out(TO_DISK[t5f & 0x3F], 8);
        out(TO_DISK[t5e & 0x3F], 8);
        out(TO_DISK[t5d & 0x3F], 8);
        out(TO_DISK[t5c & 0x3F], 8);
        out(0xDE, 8); out(0xAA, 8); out(0xFF, 8);
    }
}

// iwm_denib_track35: the sectors found on the track, by their address
// fields; returns how many decoded
int Gs35Drive::denibblize(const std::vector<uint8_t>& nib, int track, int side, uint8_t* sectors) {
    const int num = sectorsOn(track);
    const size_t len = nib.size();
    if (!len) return 0;
    size_t pos = 0;
    auto next = [&]() { const uint8_t v = nib[pos % len]; ++pos; return (uint32_t)v; };
    bool done[16] = {};
    int count = 0;
    uint32_t val = 0;
    uint32_t c00[0x100], d00[0x100], e00[0x100];
    while (pos < 2 * len && count < num) {
        if (val != 0xD5) { val = next(); continue; }
        if ((val = next()) != 0xAA) continue;
        if ((val = next()) != 0x96) continue;
        val = 0;
        const uint32_t pTrack = FROM_DISK.t[next()];
        const uint32_t sec = FROM_DISK.t[next()];
        const uint32_t pSide = FROM_DISK.t[next()];
        const uint32_t cap = FROM_DISK.t[next()];
        const uint32_t ck = FROM_DISK.t[next()];
        if (pTrack != ((uint32_t)track & 0x3F) || sec >= (uint32_t)num) continue;
        if (pSide != (uint32_t)(side << 5) + (uint32_t)(track >> 6)) continue;
        if (ck != (pTrack ^ sec ^ pSide ^ cap) || done[sec]) continue;
        uint32_t v = 0;
        for (int i = 0; i < 38 && v != 0xD5; ++i) v = next();
        if (v != 0xD5 || next() != 0xAA || next() != 0xAD) continue;
        if (FROM_DISK.t[next()] != sec) continue;
        uint32_t t5c = 0, t5d = 0, t5e = 0, carry = 0, t66, v2;
        int y = 0xAF;
        bool bad = false;
        while (y > 0) {
            v2 = FROM_DISK.t[next()];
            if (v2 >= 0x100) { bad = true; break; }
            t66 = v2;
            t5c <<= 1;
            carry = t5c >> 8;
            t5c = (t5c + carry) & 0xFF;
            v2 = FROM_DISK.t[next()];
            if (v2 >= 0x100) { bad = true; break; }
            v2 = (v2 + ((t66 << 2) & 0xC0)) ^ t5c;
            c00[y] = v2;
            t5e = v2 + t5e + carry;
            carry = t5e >> 8;
            t5e &= 0xFF;
            v2 = ((FROM_DISK.t[next()] & 0xFF) + ((t66 << 4) & 0xC0)) ^ t5e;
            d00[y] = v2;
            t5d = v2 + t5d + carry;
            carry = t5d >> 8;
            t5d &= 0xFF;
            if (--y <= 0) break;
            v2 = ((FROM_DISK.t[next()] & 0xFF) + ((t66 << 6) & 0xC0)) ^ t5d;
            e00[y + 1] = v2;
            t5c = v2 + t5c + carry;
            carry = t5c >> 8;
            t5c &= 0xFF;
        }
        if (bad) continue;
        v2 = FROM_DISK.t[next()] & 0xFF;
        const uint32_t s66 = (v2 << 6) & 0xC0, s67 = (v2 << 4) & 0xC0;
        v2 = (v2 << 2) & 0xC0;
        if (t5e != (FROM_DISK.t[next()] & 0xFF) + v2) continue;
        if (t5d != (FROM_DISK.t[next()] & 0xFF) + s67) continue;
        if (t5c != (FROM_DISK.t[next()] & 0xFF) + s66) continue;
        if (next() != 0xDE || next() != 0xAA) continue;
        uint8_t* out = sectors + sec * 512;
        int n = 0;
        for (int x = 0xAB; x >= 0 && n < 512; --x) {
            out[n++] = (uint8_t)c00[x];
            if (n < 512) out[n++] = (uint8_t)d00[x];
            if (n < 512) out[n++] = (uint8_t)e00[x];
        }
        done[sec] = true;
        ++count;
    }
    return count;
}

bool Gs35Drive::insert(const std::string& path, std::string* err) {
    std::vector<uint8_t> data;
    if (!diskfile::read(path, data, err)) return false;
    const std::string ext = lowerExt(diskfile::imageName(path));
    if (data.size() >= 8 && std::memcmp(data.data(), "WOZ2", 4) == 0) {                  // 3.5" WOZ
        auto img = Disk2Image::open(path, err);
        if (!img) return false;
        if (img->diskType() != 2) { if (err) *err = "a 5.25\" WOZ"; return false; }
        eject();
        m_woz = std::move(img);
        m_protect = m_woz->writeProtected();
        m_path = path;
        m_switched = false;
        return true;
    }
    size_t off = 0;
    if (data.size() >= 64 && std::memcmp(data.data(), "2IMG", 4) == 0) {                  // 2IMG
        off = data[24] | data[25] << 8 | data[26] << 16 | (size_t)data[27] << 24;
        if (data[12] != 1) { if (err) *err = "2IMG: only ProDOS-order images"; return false; }
    } else if (data.size() == IMAGE_BYTES + 84 || ext == ".dc" || ext == ".dc42" || ext == ".image") {
        off = 84;                                                                       // DiskCopy 4.2
    }
    if (data.size() < off + IMAGE_BYTES || data.size() > off + IMAGE_BYTES + 4096) {
        if (err) *err = "not an 800 KB 3.5\" disk image";
        return false;
    }
    std::vector<uint8_t> blocks(data.begin() + (long)off, data.begin() + (long)(off + IMAGE_BYTES));
    insertBytes(std::move(blocks), !diskfile::writable(path));
    m_path = path;
    m_file = std::move(data);
    m_fileOffset = off;
    return true;
}

bool Gs35Drive::insertBytes(std::vector<uint8_t> blocks, bool writeProtected) {
    if (blocks.size() != IMAGE_BYTES) return false;
    eject();
    m_blocks = std::move(blocks);
    m_protect = writeProtected;
    m_switched = false;
    return true;
}

void Gs35Drive::eject() {
    if (m_writing) endWrite();
    if (hasDisk()) m_switched = true;
    m_blocks.clear();
    m_file.clear();
    m_woz.reset();
    m_path.clear();
    m_fileOffset = 0;
    m_loadedTrack = -1;
    m_stream.clear();
    m_nib.clear();
    m_end.clear();
}

// The track side's bits: the WOZ's, or generated from the sectors (each
// nibble's 8 bits, then the zero bits of a 10-bit sync)
void Gs35Drive::loadTrack() {
    if (m_loadedTrack == m_track && m_loadedSide == m_side) return;
    m_loadedTrack = m_track;
    m_loadedSide = m_side;
    m_lastRead = SIZE_MAX;
    m_trackDirty = false;
    m_stream.clear();
    m_trackBits = 1;
    if (m_woz) {
        m_stream.assign((size_t)std::max(m_woz->maxTrackBytes(), 1 << 16), 0);
        uint32_t bits = 0;
        const int bytes = m_woz->wozReadTrack(m_track * 2 + m_side, m_stream.data(), bits);
        m_stream.resize((size_t)bytes);
        m_trackBits = std::max<uint32_t>(bits, 8);
    } else if (!m_blocks.empty()) {
        std::vector<uint8_t> nib, len;
        nibblize(&m_blocks[(size_t)firstBlock(m_track, m_side) * 512], m_track, m_side, nib, len);
        uint32_t total = 0;
        for (uint8_t l : len) total += l;
        m_stream.assign((total + 7) / 8, 0);
        m_trackBits = total;
        uint32_t b = 0;
        for (size_t i = 0; i < nib.size(); ++i) {
            for (int k = 0; k < 8; ++k) setBit(b + (uint32_t)k, (nib[i] >> (7 - k)) & 1);
            b += len[i];
        }
    }
    if (m_stream.empty()) { m_stream.assign(1, 0); m_trackBits = 8; }
    deriveNibbles();
}

// The IWM latches a nibble when its shift register's top bit is set; one
// revolution to fall into step, the next recorded. After three zero bits
// the read amplifier (MC3470) passes noise: such a bit reads as one about
// 30% of the time (KEGS iwm_read_data), so weak bits differ between
// revolutions
void Gs35Drive::deriveNibbles() {
    m_nib.clear();
    m_end.clear();
    m_hasWeakBits = false;
    uint32_t sr = 0;
    int zeros = 0;
    for (uint32_t pass = 0; pass < 2; ++pass)
        for (uint32_t i = 0; i < m_trackBits; ++i) {
            bool bit = getBit(i);
            if (bit) zeros = 0;
            else if (++zeros > 3) {
                m_hasWeakBits = true;
                m_rand = m_rand * 1103515245u + 12345u;
                bit = ((m_rand >> 16) & 0x7FFF) % 10 < 3;
            }
            sr = ((sr << 1) | (bit ? 1u : 0u)) & 0xFF;
            if (sr & 0x80) {
                if (pass) { m_nib.push_back((uint8_t)sr); m_end.push_back(i + 1); }
                sr = 0;
            }
        }
}

// iwm_read_status35: reg = CA1 CA0 SEL CA2
int Gs35Drive::status(int reg, uint64_t now) {
    switch (reg) {
        case 0x0: return m_stepOut ? 1 : 0;              // step direction
        case 0x1: m_side = 0; return 1;                  // lower head
        case 0x2: return hasDisk() ? 0 : 1;              // disk in place: 0
        case 0x3: m_side = 1; return 1;                  // upper head
        case 0x4: return 1;                              // not stepping
        case 0x5: return 1;
        case 0x6: return m_protect ? 0 : 1;              // write protected: 0
        case 0x8: return m_motor ? 0 : 1;                // motor on: 0
        case 0x9: return 1;                              // two sides
        case 0xA: return m_track != 0 ? 1 : 0;           // at track 0: 0
        case 0xB: return m_motor ? 0 : 1;                // ready: 0
        case 0xC: return m_switched ? 1 : 0;             // disk switched
        case 0xD: return 1;
        case 0xE: {                                      // tachometer
            if (!m_motor) return 1;
            const uint64_t bit = (uint64_t)((double)now / TICKS_PER_BIT);
            return (int)((bit >> 7) & 1);
        }
        case 0xF: return 0;                              // the drive is there
        default: return 1;
    }
}

// iwm_do_action35
void Gs35Drive::command(int reg, uint64_t) {
    switch (reg) {
        case 0x0: m_stepOut = false; break;              // step inward (higher tracks)
        case 0x1: m_stepOut = true; break;
        case 0x3: m_switched = false; break;
        case 0x4:                                        // step
            if (m_stepOut) { if (m_track > 0) --m_track; }
            else if (m_track < TRACKS - 1) ++m_track;
            break;
        case 0x8: m_motor = true; break;
        case 0x9: m_motor = false; break;
        case 0xD: eject(); break;
        default: break;
    }
}

// The last nibble the head has passed, once
uint8_t Gs35Drive::readNibble(uint64_t now) {
    if (!hasDisk() || !m_motor) return 0;
    loadTrack();
    if (m_nib.empty()) return 0;
    // A new revolution: tracks with weak bits read differently
    const uint64_t rev = (uint64_t)((double)now / TICKS_PER_BIT) / m_trackBits;
    if (m_hasWeakBits && rev != m_revolution && !m_writing) {
        const size_t last = m_lastRead;
        deriveNibbles();
        m_lastRead = last < m_nib.size() ? last : SIZE_MAX;
    }
    m_revolution = rev;
    const uint32_t bit = bitAt(now);
    size_t i = (size_t)(std::upper_bound(m_end.begin(), m_end.end(), bit) - m_end.begin());
    i = (i + m_nib.size() - 1) % m_nib.size();
    if (i == m_lastRead) return 0;
    m_lastRead = i;
    return m_nib[i];
}

// Write mode: 8 bits a byte at the head (the firmware paces them with the
// handshake register)
void Gs35Drive::writeNibble(uint8_t v, uint64_t now) {
    m_lastWrite = now;
    m_writing = true;
    if (!hasDisk() || m_protect) return;
    loadTrack();
    if (m_writeBit == UINT32_MAX) m_writeBit = bitAt(now);
    for (int k = 0; k < 8; ++k) {
        setBit(m_writeBit, (v >> (7 - k)) & 1);
        m_writeBit = (m_writeBit + 1) % m_trackBits;
    }
    m_trackDirty = true;
}

// The IWM's handshake register: bit 7 the write buffer is empty (a nibble
// time after the last write), bit 6 clear once the shift register has run
// dry too (underrun)
uint8_t Gs35Drive::handshake(uint64_t now) const {
    if (!m_writing) return 0x80;
    const double elapsed = (double)(now - m_lastWrite) / TICKS_PER_BIT;
    return (uint8_t)((elapsed >= 8 ? 0x80 : 0) | (elapsed < 16 ? 0x40 : 0));
}

// Write mode ended: the track back into the image
void Gs35Drive::endWrite() {
    m_writeBit = UINT32_MAX;
    m_writing = false;
    if (!m_trackDirty || !hasDisk()) return;
    m_trackDirty = false;
    deriveNibbles();
    m_lastRead = SIZE_MAX;
    if (m_woz) {
        m_woz->wozWriteTrack(m_loadedTrack * 2 + m_loadedSide, m_stream.data(), (int)m_stream.size());
        return;
    }
    const int num = sectorsOn(m_loadedTrack);
    const size_t first = (size_t)firstBlock(m_loadedTrack, m_loadedSide) * 512;
    denibblize(m_nib, m_loadedTrack, m_loadedSide, &m_blocks[first]);
    if (!m_path.empty() && m_file.size() >= m_fileOffset + IMAGE_BYTES) {
        std::copy(m_blocks.begin() + (long)first, m_blocks.begin() + (long)first + num * 512,
                  m_file.begin() + (long)(m_fileOffset + first));
        diskfile::patch(m_path, m_file, m_fileOffset + first, (size_t)num * 512);
    }
}

bool Gs35Drives::mountDisk(int unit, const std::string& path) {
    if (unit != 1 && unit != 2) return false;
    return drive[unit - 1].insert(path);
}
void Gs35Drives::ejectDisk(int unit) {
    if (unit == 1 || unit == 2) drive[unit - 1].eject();
}
bool Gs35Drives::getDiskStatus(int unit, int& track, int& sector, bool& led) {
    if (unit != 1 && unit != 2) return false;
    const Gs35Drive& d = drive[unit - 1];
    track = d.track();
    sector = d.side();
    led = d.motorOn();
    return d.hasDisk();
}
std::string Gs35Drives::getMountedDiskPath(int unit) {
    if (unit != 1 && unit != 2) return "";
    return drive[unit - 1].path();
}

}  // namespace apple2
