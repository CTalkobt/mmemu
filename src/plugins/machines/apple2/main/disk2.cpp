// The Disk II interface card and 5.25" disk images, after AppleWin
// (GPL-2.0-or-later): Disk.cpp (Disk2InterfaceCard, the .dsk/.nib path)
// and DiskImageHelper.cpp (CImageBase Code62/Decode62, NibblizeTrack,
// DenibblizeTrack, SkewTrack; CDoImage, CPoImage, CNib1Image).

#include "disk2.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include "libdevices/main/disk_file.h"
#include <unistd.h>

namespace apple2 {

namespace {

// Valid disk bytes for the 6-bit values (6-and-2 encoding)
const uint8_t DISK_BYTE[0x40] = {
    0x96, 0x97, 0x9A, 0x9B, 0x9D, 0x9E, 0x9F, 0xA6, 0xA7, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF, 0xB2, 0xB3,
    0xB4, 0xB5, 0xB6, 0xB7, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF, 0xCB, 0xCD, 0xCE, 0xCF, 0xD3,
    0xD6, 0xD7, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE, 0xDF, 0xE5, 0xE6, 0xE7, 0xE9, 0xEA, 0xEB, 0xEC,
    0xED, 0xEE, 0xEF, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF};

// The image's sector for each physical sector, ProDOS and DOS order
const uint8_t SECTOR_NUMBER[2][16] = {
    {0x00, 0x08, 0x01, 0x09, 0x02, 0x0A, 0x03, 0x0B, 0x04, 0x0C, 0x05, 0x0D, 0x06, 0x0E, 0x07, 0x0F},
    {0x00, 0x07, 0x0E, 0x06, 0x0D, 0x05, 0x0C, 0x04, 0x0B, 0x03, 0x0A, 0x02, 0x09, 0x01, 0x08, 0x0F}};

constexpr uint8_t VOLUME = 254;
constexpr int TRACKS_STANDARD = 35;
constexpr int TRACKS_MAX = 40;

std::string lowerExt(const std::string& path) {
    const auto dot = path.find_last_of('.');
    std::string e = dot == std::string::npos ? "" : path.substr(dot);
    for (auto& c : e) c = (char)std::tolower((unsigned char)c);
    return e;
}

// CImageBase::IsValidImageSize: 140 KB + n x 4 KB (up to 40 tracks), or
// the odd sizes seen in the wild
bool validDskSize(size_t n) {
    const size_t t = Disk2Image::TRACK_BYTES;
    if (n > (size_t)TRACKS_MAX * t) return false;
    if (n >= (size_t)(TRACKS_STANDARD + 1) * t) return (n - TRACKS_STANDARD * t) % t == 0;
    return (n >= 143105 && n <= 143364) || n == 143403 || n == 143488;
}

uint16_t word(const std::vector<uint8_t>& d, size_t at) { return (uint16_t)(d[at] | (d[at + 1] << 8)); }

// CDoImage::Detect: a DOS 3.3 catalog chain on track 17 in DOS order, or a
// ProDOS volume directory's block links
bool looksDosOrder(const std::vector<uint8_t>& d) {
    bool ok = true;
    for (int loop = 1; loop < 16 && ok; ++loop)
        if (d[0x11002 + (loop << 8)] != loop - 1) ok = false;
    if (ok) return true;
    ok = true;
    for (int loop = 2; loop <= 5 && ok; ++loop)
        if (word(d, (loop << 9) + 0x100) != (loop == 5 ? 0 : 6 - loop) ||
            word(d, (loop << 9) + 0x102) != (loop == 2 ? 0 : 8 - loop)) ok = false;
    return ok;
}
// CPoImage::Detect
bool looksProDosOrder(const std::vector<uint8_t>& d) {
    bool ok = true;
    for (int loop = 5; loop < 14 && ok; ++loop)
        if (d[0x11002 + (loop << 8)] != 14 - loop) ok = false;
    if (ok) return true;
    ok = true;
    for (int loop = 2; loop <= 5 && ok; ++loop)
        if (word(d, loop << 9) != (loop == 2 ? 0 : loop - 1) ||
            word(d, (loop << 9) + 2) != (loop == 5 ? 0 : loop + 1)) ok = false;
    return ok;
}

}  // namespace

// ---------------------------------------------------------------------------
// Disk2Image
// ---------------------------------------------------------------------------

std::unique_ptr<Disk2Image> Disk2Image::fromBytes(std::vector<uint8_t> data, const std::string& ext,
                                                  std::string* err) {
    auto img = std::unique_ptr<Disk2Image>(new Disk2Image());
    const std::string e = lowerExt("x" + ext);
    const bool wozMagic = data.size() >= 12 && std::memcmp(data.data(), "WOZ", 3) == 0;
    if (e == ".woz" || wozMagic) {
        img->m_data = std::move(data);
        if (!img->parseWoz(err)) return nullptr;
        return img;
    }
    if (e == ".nib") {
        if (data.empty() || data.size() % NIB_TRACK || data.size() / NIB_TRACK > TRACKS_MAX) {
            if (err) *err = "not a .nib image (35 tracks of 6656 nibbles)";
            return nullptr;
        }
        img->m_order = Order::Nib;
        img->m_tracks = (int)(data.size() / NIB_TRACK);
    } else {
        if (!validDskSize(data.size())) {
            if (err) *err = "not a 140 KB 5.25\" disk image";
            return nullptr;
        }
        if (e == ".po") img->m_order = Order::ProDOS;
        else if (e == ".do") img->m_order = Order::DOS;
        else img->m_order = looksDosOrder(data) ? Order::DOS : looksProDosOrder(data) ? Order::ProDOS : Order::DOS;
        img->m_tracks = (int)(data.size() / TRACK_BYTES);
    }
    img->m_data = std::move(data);
    return img;
}

std::unique_ptr<Disk2Image> Disk2Image::open(const std::string& path, std::string* err) {
    // Plain, .gz or .zip (#233): the format comes from the image's own name
    std::vector<uint8_t> data;
    if (!diskfile::read(path, data, err)) return nullptr;
    auto img = fromBytes(std::move(data), lowerExt(diskfile::imageName(path)), err);
    if (!img) return nullptr;
    img->m_path = path;
    if (!diskfile::writable(path)) img->m_protect = true;
    return img;
}


// Code62: 256 bytes -> 342 6-bit values (86 of the low two bits, 256 of
// the high six) -> XOR-chained with a checksum (343) -> disk bytes
void Disk2Image::nibblizeSector(const uint8_t* sector, uint8_t* out) {
    uint8_t six[343];
    int n = 0;
    uint8_t offset = 0xAC;
    auto add = [](uint8_t value, uint8_t a) {
        return (uint8_t)((value << 2) | ((a & 0x01) << 1) | ((a & 0x02) >> 1));
    };
    while (offset != 0x02) {
        uint8_t value = 0;
        value = add(value, sector[offset]); offset = (uint8_t)(offset - 0x56);
        value = add(value, sector[offset]); offset = (uint8_t)(offset - 0x56);
        value = add(value, sector[offset]); offset = (uint8_t)(offset - 0x53);
        six[n++] = (uint8_t)(value << 2);
    }
    six[n - 2] &= 0x3F;
    six[n - 1] &= 0x3F;
    for (int i = 0; i < 0x100; ++i) six[n++] = sector[i];
    uint8_t chained[343];
    uint8_t saved = 0;
    for (int i = 0; i < 342; ++i) { chained[i] = saved ^ six[i]; saved = six[i]; }
    chained[342] = saved;
    for (int i = 0; i < 343; ++i) out[i] = DISK_BYTE[chained[i] >> 2];
}

// Decode62
void Disk2Image::denibblizeSector(const uint8_t* in, uint8_t* sector) {
    static uint8_t sixBit[0x80];
    static bool made = false;
    if (!made) {
        std::memset(sixBit, 0, sizeof sixBit);
        for (int i = 0; i < 0x40; ++i) sixBit[DISK_BYTE[i] - 0x80] = (uint8_t)(i << 2);
        made = true;
    }
    uint8_t six[343], buf[342];
    for (int i = 0; i < 343; ++i) six[i] = sixBit[in[i] & 0x7F];
    uint8_t saved = 0;
    for (int i = 0; i < 342; ++i) { buf[i] = saved ^ six[i]; saved = buf[i]; }
    const uint8_t* low = buf;
    const uint8_t* high = buf + 0x56;
    uint8_t offset = 0xAC;
    while (offset != 0x02) {
        if (offset >= 0xAC)
            sector[offset] = (uint8_t)((high[offset] & 0xFC) | ((*low & 0x80) >> 7) | ((*low & 0x40) >> 5));
        offset = (uint8_t)(offset - 0x56);
        sector[offset] = (uint8_t)((high[offset] & 0xFC) | ((*low & 0x20) >> 5) | ((*low & 0x10) >> 3));
        offset = (uint8_t)(offset - 0x56);
        sector[offset] = (uint8_t)((high[offset] & 0xFC) | ((*low & 0x08) >> 3) | ((*low & 0x04) >> 1));
        offset = (uint8_t)(offset - 0x53);
        ++low;
    }
}

// NibblizeTrack: gap 1 (48 sync bytes), then 16 sectors of an address
// field (D5 AA 96, volume, track, sector, checksum in 4-and-4, DE AA EB),
// gap 2 (6), the data field (D5 AA AD, 343 bytes, DE AA EB), gap 3 (27)
int Disk2Image::nibblize(int track, uint8_t* out) const {
    const uint8_t* data = m_data.data() + (size_t)track * TRACK_BYTES;
    const int ord = m_order == Order::ProDOS ? 0 : 1;
    uint8_t* p = out;
    auto a44 = [](uint8_t v) { return (uint8_t)(((v >> 1) & 0x55) | 0xAA); };
    auto b44 = [](uint8_t v) { return (uint8_t)((v & 0x55) | 0xAA); };
    for (int i = 0; i < 48; ++i) *p++ = 0xFF;
    for (int s = 0; s < 16; ++s) {
        const uint8_t t = (uint8_t)track, sec = (uint8_t)s, chk = (uint8_t)(VOLUME ^ t ^ sec);
        *p++ = 0xD5; *p++ = 0xAA; *p++ = 0x96;
        *p++ = a44(VOLUME); *p++ = b44(VOLUME);
        *p++ = a44(t); *p++ = b44(t);
        *p++ = a44(sec); *p++ = b44(sec);
        *p++ = a44(chk); *p++ = b44(chk);
        *p++ = 0xDE; *p++ = 0xAA; *p++ = 0xEB;
        for (int i = 0; i < 6; ++i) *p++ = 0xFF;
        *p++ = 0xD5; *p++ = 0xAA; *p++ = 0xAD;
        nibblizeSector(data + (SECTOR_NUMBER[ord][s] << 8), p);
        p += 343;
        *p++ = 0xDE; *p++ = 0xAA; *p++ = 0xEB;
        for (int i = 0; i < 27; ++i) *p++ = 0xFF;
    }
    return (int)(p - out);
}

// DenibblizeTrack: find each address field's sector, then decode the data
// field after it into the image's sector (sectors not found read as zeroes)
void Disk2Image::denibblize(const uint8_t* track, int nibbles, uint8_t* out) const {
    std::memset(out, 0, TRACK_BYTES);
    const int ord = m_order == Order::ProDOS ? 0 : 1;
    uint8_t work[384];
    int offset = 0;
    int partsLeft = 16 * 2 + 1;
    int sector = -1;
    while (partsLeft--) {
        uint8_t v[3] = {0, 0, 0};
        int num = 0;
        int loop = nibbles;
        while (loop-- && num < 3) {
            if (num) v[num++] = track[offset++];
            else if (track[offset++] == 0xD5) num = 1;
            if (offset >= nibbles) offset = 0;
        }
        if (num == 3 && v[1] == 0xAA) {
            int temp = offset;
            for (int i = 0; i < 384; ++i) {
                work[i] = track[temp++];
                if (temp >= nibbles) temp = 0;
            }
            if (v[2] == 0x96) {
                sector = ((work[4] & 0x55) << 1) | (work[5] & 0x55);
            } else if (v[2] == 0xAD) {
                if (sector >= 0 && sector < 16) denibblizeSector(work, out + (SECTOR_NUMBER[ord][sector] << 8));
                sector = 0;
            }
        }
    }
}

int Disk2Image::readTrack(int track, uint8_t* out, bool skew) {
    if (track < 0 || track >= m_tracks) return 0;
    if (m_order == Order::Nib) {
        std::memcpy(out, m_data.data() + (size_t)track * NIB_TRACK, NIB_TRACK);
        return NIB_TRACK;
    }
    const int n = nibblize(track, out);
    if (skew) {     // SkewTrack: each track starts 768 nibbles on from the last
        const int k = (track * 768) % n;
        std::rotate(out, out + k, out + n);
    }
    return n;
}

void Disk2Image::writeTrack(int track, const uint8_t* nibbles, int count) {
    if (track < 0 || track >= m_tracks || m_protect) return;
    size_t at, len;
    if (m_order == Order::Nib) {
        at = (size_t)track * NIB_TRACK;
        len = NIB_TRACK;
        std::memcpy(m_data.data() + at, nibbles, std::min(count, NIB_TRACK));
    } else {
        at = (size_t)track * TRACK_BYTES;
        len = TRACK_BYTES;
        denibblize(nibbles, count, m_data.data() + at);
    }
    if (!m_path.empty()) diskfile::patch(m_path, m_data, at, len);
}

void Disk2Image::saveFile(size_t at, size_t len) {
    if (m_path.empty()) return;
    diskfile::patch(m_path, m_data, at, len);
}

uint32_t Disk2Image::crc32(const uint8_t* p, size_t n) {
    static uint32_t table[256];
    static bool made = false;
    if (!made) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        made = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// CWOZHelper::ProcessChunks: the header ("WOZ1"/"WOZ2", FF 0A 0D 0A, CRC),
// then chunks: exactly one INFO (a 5.25" disk), TMAP and TRKS; WRIT, META
// and FLUX are skipped
bool Disk2Image::parseWoz(std::string* err) {
    auto fail = [&](const char* why) { if (err) *err = std::string("WOZ: ") + why; return false; };
    const auto& d = m_data;
    if (d.size() < 12 || std::memcmp(d.data(), "WOZ", 3) != 0 || (d[3] != '1' && d[3] != '2'))
        return fail("not a WOZ1/WOZ2 file");
    if (d[4] != 0xFF || d[5] != 0x0A || d[6] != 0x0D || d[7] != 0x0A) return fail("bad header");
    m_woz1 = d[3] == '1';
    bool info = false, tmap = false, trks = false;
    int infoVersion = 1, largestTrack = 0;
    size_t p = 12;
    while (p + 8 <= d.size()) {
        const uint32_t size = word(d, p + 4) | ((uint32_t)word(d, p + 6) << 16);
        const size_t body = p + 8;
        if (body + size > d.size()) return fail("truncated chunk");
        if (std::memcmp(&d[p], "INFO", 4) == 0) {
            if (info) return fail("two INFO chunks");
            info = true;
            infoVersion = d[body];
            m_diskType = d[body + 1];
            if (m_diskType != 1 && m_diskType != 2) return fail("not a 5.25\" or 3.5\" disk");
            if (d[body + 2] == 1) m_protect = true;
            if (infoVersion >= 2 && size >= 46) {
                m_boot13 = d[body + 38] == 2;
                if (d[body + 39]) m_bitTiming = d[body + 39];
                largestTrack = word(d, body + 44);
            }
        } else if (std::memcmp(&d[p], "TMAP", 4) == 0) {
            if (tmap || size < QUARTER_TRACKS) return fail("bad TMAP chunk");
            tmap = true;
            m_tmap = body;
        } else if (std::memcmp(&d[p], "TRKS", 4) == 0) {
            if (trks) return fail("two TRKS chunks");
            trks = true;
            m_trks = body;
            m_trksHdr = p;
            m_trksLast = body + size == d.size();
        }
        p = body + size;
    }
    if (!(info && tmap && trks)) return fail("missing INFO, TMAP or TRKS");
    m_order = Order::Woz;
    m_tracks = 40;
    const int largest = infoVersion >= 2 ? largestTrack * 512 : NIB_TRACK;
    m_maxTrackBytes = std::max({largest, NIB_TRACK, MAX_NIBBLES, WOZ_EMPTY_TRACK});
    return true;
}

// CWOZImageHelper: an unrecorded track is noise, ~30% one bits
int Disk2Image::wozReadTrack(int qt, uint8_t* out, uint32_t& bitCount) const {
    static std::vector<uint8_t> empty;
    if (empty.empty()) {
        uint32_t r = 1;
        empty.resize(WOZ_EMPTY_TRACK);
        for (auto& b : empty) {
            uint8_t n = 0;
            for (int j = 0; j < 8; ++j) {
                r = r * 1103515245u + 12345u;
                if (((r >> 16) & 0x7FFF) % 10 < 3) n |= (uint8_t)(1 << j);
            }
            b = n;
        }
    }
    auto noise = [&]() {
        std::memcpy(out, empty.data(), WOZ_EMPTY_TRACK);
        bitCount = WOZ_EMPTY_TRACK * 8;
        return WOZ_EMPTY_TRACK;
    };
    if (qt < 0 || qt >= QUARTER_TRACKS) return noise();
    const uint8_t idx = m_data[m_tmap + qt];
    if (idx == 0xFF) return noise();
    if (m_woz1) {
        const size_t base = m_trks + (size_t)idx * NIB_TRACK;
        if (base + NIB_TRACK > m_data.size()) return noise();
        const int bytes = word(m_data, base + 6646);
        bitCount = word(m_data, base + 6648);
        if (bytes > 6646 || bytes == 0) return noise();
        std::memcpy(out, &m_data[base], (size_t)bytes);
        return bytes;
    }
    const size_t e = m_trks + (size_t)idx * 8;
    const uint32_t start = word(m_data, e) * 512u;
    const uint32_t bits = word(m_data, e + 4) | ((uint32_t)word(m_data, e + 6) << 16);
    const int bytes = (int)((bits + 7) / 8);
    if (bytes == 0 || bytes > m_maxTrackBytes || start + (size_t)bytes > m_data.size()) return noise();
    bitCount = bits;
    std::memcpy(out, &m_data[start], (size_t)bytes);
    return bytes;
}

// CWOZ1Image/CWOZ2Image::Write: the bits back into the track; an unmapped
// quarter track gets a new track (also seen from the quarter tracks on
// either side) appended to TRKS; then the header CRC
void Disk2Image::wozWriteTrack(int qt, const uint8_t* bits, int bytes) {
    if (m_protect || qt < 0 || qt >= QUARTER_TRACKS || bytes <= 0) return;
    auto put16 = [&](size_t at, uint32_t v) { m_data[at] = (uint8_t)v; m_data[at + 1] = (uint8_t)(v >> 8); };
    auto put32 = [&](size_t at, uint32_t v) { put16(at, v & 0xFFFF); put16(at + 2, v >> 16); };
    uint8_t idx = m_data[m_tmap + qt];
    if (idx == 0xFF) {
        if (!m_trksLast) return;        // TRKS can only grow at the end of the file
        int highest = -1;
        for (int i = 0; i < QUARTER_TRACKS; ++i)
            if (m_data[m_tmap + i] != 0xFF) highest = std::max(highest, (int)m_data[m_tmap + i]);
        idx = (uint8_t)(highest + 1);
        if (!m_woz1 && idx >= QUARTER_TRACKS) return;
        m_data[m_tmap + qt] = idx;
        if (qt > 0) m_data[m_tmap + qt - 1] = idx;
        if (qt + 1 < QUARTER_TRACKS) m_data[m_tmap + qt + 1] = idx;
        uint32_t ext;
        if (m_woz1) {
            if (bytes > 6646) return;
            ext = NIB_TRACK;
            const size_t base = m_data.size();
            m_data.resize(base + ext, 0);
            put16(base + 6646, (uint32_t)bytes);
            put16(base + 6648, (uint32_t)bytes * 8);
        } else {
            ext = ((uint32_t)bytes + 511) & ~511u;
            m_data.resize(m_data.size() + ext, 0);
            const size_t e = m_trks + (size_t)idx * 8;
            uint32_t start = 3;
            for (int i = 0; i < idx; ++i) start += word(m_data, m_trks + (size_t)i * 8 + 2);
            put16(e, start);
            put16(e + 2, ext / 512);
            put32(e + 4, (uint32_t)bytes * 8);
        }
        const uint32_t size = word(m_data, m_trksHdr + 4) | ((uint32_t)word(m_data, m_trksHdr + 6) << 16);
        put32(m_trksHdr + 4, size + ext);
    }
    if (m_woz1) {
        const size_t base = m_trks + (size_t)idx * NIB_TRACK;
        if (bytes > 6646 || base + NIB_TRACK > m_data.size()) return;
        std::memcpy(&m_data[base], bits, (size_t)bytes);
    } else {
        const size_t e = m_trks + (size_t)idx * 8;
        const uint32_t start = word(m_data, e) * 512u;
        const uint32_t count = word(m_data, e + 4) | ((uint32_t)word(m_data, e + 6) << 16);
        if ((int)((count + 7) / 8) != bytes || start + (size_t)bytes > m_data.size()) return;
        std::memcpy(&m_data[start], bits, (size_t)bytes);
    }
    put32(8, crc32(m_data.data() + 12, m_data.size() - 12));
    if (!m_path.empty()) diskfile::save(m_path, m_data);
}

// ---------------------------------------------------------------------------
// Disk2Card
// ---------------------------------------------------------------------------

namespace {
// The address prologue of track 0 sector 0 as read through the latch
// (AppleWin m_T00S00Pattern, for the GH#930 jitter)
const uint8_t T00S00[] = {0xD5, 0xAA, 0x96, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xDE};
}  // namespace

Disk2Card::Disk2Card(std::vector<uint8_t> rom, std::vector<uint8_t> rom13)
    : m_rom(std::move(rom)), m_rom13(std::move(rom13)) {
    m_rom.resize(256, 0);
    if (m_rom13.size() < 256) m_rom13 = m_rom;
    for (auto& d : m_drive) d.disk.track.assign(Disk2Image::MAX_NIBBLES, 0);
}

Disk2Card::~Disk2Card() {
    flush(0);
    flush(1);
}

bool Disk2Card::woz() const {
    const Disk2Image* img = m_drive[m_curr].disk.image.get();
    return img && img->isWoz();
}

bool Disk2Card::chance(int num, int den) {
    m_rand = m_rand * 1103515245u + 12345u;
    return (int)((m_rand >> 16) % (uint32_t)den) < num;
}

// InitFirmware: the 13-sector ROM for a DOS 3.2 WOZ in drive 1
void Disk2Card::chooseRom() {
    const Disk2Image* img = m_drive[0].disk.image.get();
    m_use13 = img && img->bootSector13();
}

void Disk2Card::resetSequencer() {
    m_shiftReg = 0;
    m_latchDelay = 0;
    m_writeStarted = false;
    m_t00Idx = 0;
    m_t00Found = false;
}

void Disk2Card::reset(bool powerCycle) {
    // RESET forces the switches off (UTAIIe table 9.1); the heads stay put
    m_curr = 0;
    m_motorOn = false;
    m_magnets = 0;
    m_writeMode = m_loadMode = false;
    m_deferred = false;
    resetSequencer();
    if (powerCycle) {
        for (int i = 0; i < 2; ++i) {
            flush(i);
            m_drive[i].spinning = 0;
            m_drive[i].writeLight = 0;
            m_drive[i].motorOnCycle = 0;
        }
        m_lastCycle = m_lastReadLatch = m_now = 0;
    }
    chooseRom();
}

void Disk2Card::syncStepper(uint64_t now) {
    if (now > m_now) m_now = now;
    if (m_deferred && m_now >= m_deferredAt + 10) stepperDeferred();
}

// Disk2InterfaceCard::Update: the motor runs on for a second; a stopped
// drive's dirty track goes back to the image
void Disk2Card::update(uint64_t now, uint64_t cycles) {
    syncStepper(now);
    for (int i = 0; i < 2; ++i) {
        Drive& d = m_drive[i];
        if (d.spinning && !m_motorOn) d.spinning -= (int)std::min<uint64_t>((uint64_t)d.spinning, cycles);
        if (m_writeMode && m_curr == i && d.spinning) d.writeLight = SPINNING_CYCLES;
        else if (d.writeLight) d.writeLight -= (int)std::min<uint64_t>((uint64_t)d.writeLight, cycles);
        if (!m_motorOn && !d.spinning) flush(i);
    }
}

// IORead / IOWrite
int Disk2Card::io(uint16_t addr, const uint8_t* wr, uint64_t now) {
    syncStepper(now);
    now = m_now;
    bool isWoz = woz();
    if (isWoz && seqFunction() == SHIFT_WRITE) shiftWriteWoz(now);   // finish the last write
    setSequencer(addr, now);
    switch (addr & 0xF) {
        case 0x0: case 0x1: case 0x2: case 0x3:
        case 0x4: case 0x5: case 0x6: case 0x7: controlStepper(addr, now); break;
        case 0x8: case 0x9: controlMotor(addr, now); break;
        case 0xA: case 0xB: enable(addr, now); isWoz = woz(); break;
        case 0xC: if (!isWoz) readWrite(now); break;
        case 0xD: loadWriteProtect(now); break;
        case 0xE: break;                        // read mode
        case 0xF:                               // write mode: the light
            m_drive[m_curr].writeLight = SPINNING_CYCLES;
            break;
    }
    if (wr) {
        // Any address loads the latch in the sequencer's LD function
        // (write mode and load mode, 74LS323)
        if (seqFunction() == LOAD_WRITE) {
            m_latch = *wr;
            if (isWoz) latchWoz(true, now);
        }
        return 0;
    }
    // Only even addresses return the latch (UTAIIe table 9.1)
    if (addr & 1) return -1;
    if (isWoz && seqFunction() != SHIFT_WRITE) latchWoz(false, now);
    return m_latch;
}

// SetSequencerFunction: $C08C/$C08D load mode off/on, $C08E/$C08F write mode
void Disk2Card::setSequencer(uint16_t addr, uint64_t now) {
    if ((addr & 0xF) < 0xC) return;
    const int old = seqFunction();
    switch ((addr & 3) ^ 2) {
        case 0: m_writeMode = false; break;
        case 1: m_writeMode = true; break;
        case 2: m_loadMode = false; break;
        case 3: m_loadMode = true; break;
    }
    if (!m_writeMode) m_writeStarted = false;
    // Leaving check-write-protect: the bit cells pass (write-protect checks,
    // bit-slip protections)
    if (old == CHECK_WP && seqFunction() != CHECK_WP && woz())
        advanceBits(m_drive[m_curr].disk, bitCellDelta(now));
}

void Disk2Card::checkSpinning(bool stateChanged, uint64_t now) {
    Drive& d = m_drive[m_curr];
    const bool modeChanged = m_motorOn && !d.spinning;
    if (m_motorOn) d.spinning = SPINNING_CYCLES;
    if (modeChanged) m_lastCycle = now;
    if (m_motorOn && stateChanged) d.motorOnCycle = now;
}

void Disk2Card::controlMotor(uint16_t addr, uint64_t now) {
    const bool on = addr & 1;
    const bool changed = on != m_motorOn;
    // "DRIVES OFF forces the control flip-flops to clear" (UTAIIe 9-12)
    if (!on) m_magnets = 0;
    if (changed) m_motorOn = on;
    checkSpinning(changed, now);
}

void Disk2Card::controlStepper(uint16_t addr, uint64_t now) {
    Drive& d = m_drive[m_curr];
    if (!m_motorOn && !d.spinning) return;
    const int bit = 1 << ((addr >> 1) & 3);
    if (addr & 1) m_magnets |= bit;
    else m_magnets &= ~bit;
    if (m_deferred) {
        m_deferred = false;
        int delta = (m_deferredAddr & 7) - (addr & 7);
        if (delta < 0) delta = -delta;
        // Two adjacent magnets off in quick succession don't move the cog
        if ((delta == 2 || delta == 6) && !(addr & 1)) return;
        stepperDeferred();          // the earlier change first
    }
    m_deferredAddr = addr;
    m_deferredAt = now;
    m_deferred = true;
}

// ControlStepperDeferred: the head moves toward an adjacent magnet that is
// on, unless the magnets on both sides are on; a half track at a time. With
// two adjacent magnets on, a WOZ disk's head stops between (a quarter track)
void Disk2Card::stepperDeferred() {
    m_deferred = false;
    Drive& d = m_drive[m_curr];
    int dir = 0;
    if (m_magnets & (1 << ((d.phase + 1) & 3))) dir += 1;
    if (m_magnets & (1 << ((d.phase + 3) & 3))) dir -= 1;
    int quarterDir = 0;
    if (woz() && (m_magnets == 0xC || m_magnets == 0x6 || m_magnets == 0x3 || m_magnets == 0x9)) {
        quarterDir = dir;
        dir = 0;
    }
    const int np = std::clamp(d.phase + dir, 0, 79);
    const int nq = std::max(0, np * 2 + quarterDir);
    if (nq != d.quarter) flush(m_curr);     // the written track, before the head moves
    d.phase = np;
    if (nq != d.quarter) {
        d.quarter = nq;
        d.disk.trackData = false;
    }
}

void Disk2Card::enable(uint16_t addr, uint64_t now) {
    const int nd = addr & 1;
    const bool changed = nd != m_curr;
    m_curr = nd;
    m_drive[!m_curr].spinning = 0;
    m_drive[!m_curr].writeLight = 0;
    checkSpinning(changed, now);
}

// ReadTrack: a WOZ track keeps the head's place in proportion to the
// track lengths (WOZ-1.01), rounded up for the cross-track sync checks
void Disk2Card::readTrack(int drive, uint64_t now) {
    Drive& d = m_drive[drive];
    Floppy& f = d.disk;
    if (!f.image) { f.trackData = false; return; }
    if ((int)f.track.size() < f.image->maxTrackBytes()) f.track.assign((size_t)f.image->maxTrackBytes(), 0);
    if (!f.image->isWoz()) {
        const int track = d.phase >> 1;
        if (track >= f.image->tracks()) { f.trackData = false; return; }
        f.nibbles = f.image->readTrack(track, f.track.data());
        f.byte = 0;
        f.trackData = f.nibbles != 0;
        return;
    }
    // The position on the old track first (only the current drive's clock runs)
    if (drive == m_curr) advanceBits(f, bitCellDelta(now));
    if (f.bitCount == 0) { f.bitOffset = 0; f.bitCount = 8; }
    const uint32_t pos = f.bitOffset, len = f.bitCount;
    uint32_t bits = 0;
    f.nibbles = f.image->wozReadTrack(d.quarter, f.track.data(), bits);
    if (f.nibbles == 0 || bits == 0) { f.nibbles = 1; bits = 8; }
    f.bitCount = bits;
    f.bitOffset = (uint32_t)(((uint64_t)pos * bits) / len) + 7;
    if (f.bitOffset >= f.bitCount) f.bitOffset = 0;
    bitOffsets(f);
    f.extraCycles = 0;
    d.headWindow = 0;
    findTrackSeam(f);
    f.trackData = true;
    f.initialBitOffset = f.bitOffset;
    f.revs = 0;
}

void Disk2Card::flush(int drive) {
    Drive& d = m_drive[drive];
    Floppy& f = d.disk;
    if (f.image && f.dirty) {
        if (f.image->isWoz()) f.image->wozWriteTrack(d.quarter, f.track.data(), f.nibbles);
        else f.image->writeTrack(d.phase >> 1, f.track.data(), f.nibbles);
    }
    f.dirty = false;
}

// UpdateLatchForEmptyDrive: noise once the sequencer settles (GH#748, GH#864)
void Disk2Card::emptyLatch(const Drive& d, uint64_t now) {
    if (now - d.motorOnCycle < (uint64_t)MOTOR_ON_UNTIL_LSS_STABLE_CYCLES) m_latch = 0x80;
    else { m_rand = m_rand * 1103515245u + 12345u; m_latch = (uint8_t)(m_rand >> 16); }
}

// ReadWrite ($C08C, .dsk/.nib): the disk has turned a nibble every 32
// cycles since the last access; a read takes the nibble under the head
// (part of it if within 6 cycles of the last read), a write puts the latch
// there
void Disk2Card::readWrite(uint64_t now) {
    Drive& d = m_drive[m_curr];
    Floppy& f = d.disk;
    if (!f.trackData && f.image) readTrack(m_curr, now);
    if (!f.trackData) { emptyLatch(d, now); return; }
    if (d.spinning) {
        const uint64_t diff = now - m_lastCycle;
        m_lastCycle = now;
        if (diff > 40) {
            f.byte += (int)((diff >> 5) % (uint64_t)f.nibbles);
            if (f.byte >= f.nibbles) f.byte -= f.nibbles;
        }
    }
    if (!m_writeMode) {
        // "DRIVES OFF forces the data register to hold its present state"
        if (!d.spinning) return;
        const uint64_t rd = now - m_lastReadLatch;
        if (rd <= 6) {
            m_latch = (uint8_t)(f.track[f.byte] >> (8 - rd / 4));
            return;
        }
        m_latch = f.track[f.byte];
        m_lastReadLatch = now;
    } else if (!f.image->writeProtected()) {
        if (!d.spinning) return;
        f.track[f.byte] = m_latch;
        f.dirty = true;
    }
    if (++f.byte >= f.nibbles) f.byte = 0;
}

// $C08D: the write-protect switch shifted into the latch (saturated: $FF
// protected, $00 not); on a WOZ disk the sequencer goes to state 0
void Disk2Card::loadWriteProtect(uint64_t now) {
    Drive& d = m_drive[m_curr];
    if (!d.spinning) return;
    const Disk2Image* img = d.disk.image.get();
    m_latch = (img && img->writeProtected()) ? 0xFF : 0x00;
    if (m_writeStarted) return;
    if (img && img->isWoz()) {
        advanceBits(d.disk, bitCellDelta(now));     // E7 bit-slip protections
        resetSequencer();
    }
}

// --- WOZ: the bit stream -----------------------------------------------------

// GetBitCellDelta: bit cells since the last access at the disk's optimal
// bit timing (125 ns units), the remainder carried over
uint32_t Disk2Card::bitCellDelta(uint64_t now) {
    Floppy& f = m_drive[m_curr].disk;
    const int timing = f.image ? f.image->optimalBitTiming() : 32;
    const double cycles = (double)(now - m_lastCycle) + f.extraCycles;
    const double bitTime = 0.125 * timing;
    const uint32_t cells = (uint32_t)std::floor(cycles / bitTime);
    f.extraCycles = cycles - cells * bitTime;
    m_lastCycle = now;
    return cells;
}

void Disk2Card::bitOffsets(Floppy& f) {
    f.byte = (int)(f.bitOffset / 8);
    f.bitMask = (uint8_t)(1 << (7 - (f.bitOffset & 7)));
}

void Disk2Card::advanceBits(Floppy& f, uint32_t cells) {
    if (f.bitCount == 0) return;
    f.bitOffset += cells;
    if (f.bitOffset >= f.bitCount) f.bitOffset %= f.bitCount;
    bitOffsets(f);
}

void Disk2Card::incBit(Floppy& f) {
    f.bitMask >>= 1;
    if (!f.bitMask) { f.bitMask = 0x80; ++f.byte; }
    if (++f.bitOffset == f.bitCount) { f.bitMask = 0x80; f.bitOffset = 0; f.byte = 0; }
    if (f.bitOffset == f.initialBitOffset) ++f.revs;
}

// PreJitterCheck / AddJitter (GH#930): after the boot ROM reads T$00 S$00's
// address prologue, a long skip sometimes slips a bit cell
void Disk2Card::preJitter(int phase) {
    if (phase != 0 || !(m_latch & 0x80)) return;
    if (m_latch == T00S00[m_t00Idx]) {
        if (++m_t00Idx == (int)sizeof T00S00) m_t00Found = true;
    } else {
        m_t00Idx = 0;
    }
}
void Disk2Card::jitter(int phase, Floppy& f) {
    if (phase == 0 && m_t00Found && chance(1, 10)) incBit(f);
    m_t00Idx = 0;
    m_t00Found = false;
}
// AddTrackSeamJitter (GH#1125): from track 33 on, at the start of a long
// sync run (the track seam) half the time a bit cell slips
void Disk2Card::seamJitter(int quarter, Floppy& f) {
    if (quarter >= 33 * 4 && f.longestSyncRun > 110 && (int)f.bitOffset == f.longestSyncStart && chance(5, 10))
        incBit(f);
}

// DataLatchReadWriteWOZ: run the bit cells since the last access through
// the sequencer (no more than 100: a long gap re-syncs)
void Disk2Card::latchWoz(bool write, uint64_t now) {
    Drive& d = m_drive[m_curr];
    Floppy& f = d.disk;
    if (!f.trackData && f.image) readTrack(m_curr, now);
    if (!f.trackData) { emptyLatch(d, now); return; }
    if (!d.spinning) return;
    const uint32_t SIGNIFICANT = 100;
    uint32_t delta = bitCellDelta(now);
    uint32_t remainder = delta;
    if (delta > SIGNIFICANT) {
        remainder = SIGNIFICANT;
        advanceBits(f, delta - SIGNIFICANT);
        m_latchDelay = 0;
        d.headWindow = 0;
        jitter(d.phase, f);
    }
    if (!write) {
        if (seqFunction() != READ_SEQ) {        // check write protect
            advanceBits(f, remainder);
            return;
        }
        readBitsWoz(remainder);
        preJitter(d.phase);
    } else {
        loadWriteWoz(remainder);
    }
}

// DataLatchReadWOZ: each bit cell through the MC3470's head window (after
// three zero cells it makes up bits: ~30% ones) into the shift register; a
// nibble (bit 7 set) is latched and held 7 us, longer while zeroes follow
void Disk2Card::readBitsWoz(uint32_t cells) {
    Drive& d = m_drive[m_curr];
    Floppy& f = d.disk;
    for (uint32_t i = 0; i < cells; ++i) {
        const uint8_t n = f.track[(size_t)f.byte];
        d.headWindow = (uint8_t)((d.headWindow << 1) | ((n & f.bitMask) ? 1 : 0));
        const uint8_t bit = (d.headWindow & 0x0F) ? ((d.headWindow >> 1) & 1) : (chance(3, 10) ? 1 : 0);
        incBit(f);
        seamJitter(d.quarter, f);
        m_shiftReg = (uint8_t)((m_shiftReg << 1) | bit);
        if (m_latchDelay) {
            m_latchDelay -= 4;
            if (m_latchDelay < 0) m_latchDelay = 0;
            if (!m_shiftReg) m_latchDelay += 4;     // GH#662
        }
        if (!m_latchDelay) {
            m_latch = m_shiftReg;
            if (m_shiftReg & 0x80) {
                m_latchDelay = 7;
                m_shiftReg = 0;
            }
        }
    }
}

// DataLoadWriteWOZ: the latch goes to the shift register to be written
void Disk2Card::loadWriteWoz(uint32_t cells) {
    Floppy& f = m_drive[m_curr].disk;
    if (f.image->writeProtected()) { advanceBits(f, cells); return; }
    if (!m_writeStarted) advanceBits(f, cells);    // the cells before writing
    m_writeStarted = true;
    m_shiftReg = m_latch;
    f.longestSyncStart = -1;                       // the seam moved
}

// DataShiftWriteWOZ: the shift register's bits onto the track, a bit cell
// at a time since the last access
void Disk2Card::shiftWriteWoz(uint64_t now) {
    Drive& d = m_drive[m_curr];
    Floppy& f = d.disk;
    const uint32_t cells = bitCellDelta(now);
    if (!f.image) return;
    if (f.image->writeProtected()) { advanceBits(f, cells); return; }
    if (!d.spinning || !f.trackData) return;
    for (uint32_t i = 0; i < cells; ++i) {
        const bool out = m_shiftReg & 0x80;
        m_shiftReg = (uint8_t)(m_shiftReg << 1);
        uint8_t& n = f.track[(size_t)f.byte];
        n = (uint8_t)((n & ~f.bitMask) | (out ? f.bitMask : 0));
        incBit(f);
    }
    f.dirty = true;
}

// FindTrackSeamWOZ: the longest run of 10-bit sync FFs
void Disk2Card::findTrackSeam(Floppy& f) {
    const uint32_t old = f.bitOffset;
    uint8_t shift = 0;
    int zeroes = 0, start = -1, nibbleStart = -1, syncStart = -1, syncRun = 0;
    int longestStart = -1, longestRun = 0;
    f.bitOffset = 0;
    bitOffsets(f);
    f.longestSyncStart = -1;
    const uint32_t initial = f.initialBitOffset;
    f.initialBitOffset = 0xFFFFFFFFu;               // no revolution counting here
    for (uint64_t guard = 0; guard < (uint64_t)f.bitCount * 2 + 16; ++guard) {
        const uint8_t bit = (f.track[(size_t)f.byte] & f.bitMask) ? 1 : 0;
        incBit(f);
        if ((start < 0 && f.bitOffset == 0) || (start >= 0 && (uint32_t)start == f.bitOffset)) break;
        if (shift & 0x80) {
            if (!bit) { ++zeroes; continue; }
            if (shift == 0xFF && zeroes == 2) {
                if (syncStart < 0) syncStart = nibbleStart;
                ++syncRun;
            }
            if ((shift != 0xFF || zeroes != 2) && syncStart >= 0) {
                if (start < 0) start = nibbleStart;
                if (longestRun < syncRun) { longestStart = syncStart; longestRun = syncRun; }
                syncStart = -1;
                syncRun = 0;
            }
            shift = 0;
            zeroes = 0;
        }
        shift = (uint8_t)((shift << 1) | bit);
        if (shift == 0x01) {
            nibbleStart = (int)f.bitOffset - 1;
            if (nibbleStart < 0) nibbleStart += (int)f.bitCount;
        }
    }
    if (longestRun) {
        f.longestSyncStart = longestStart;
        f.longestSyncRun = longestRun;
    }
    f.initialBitOffset = initial;
    f.bitOffset = old;
    bitOffsets(f);
}

// --- Disks -------------------------------------------------------------------

bool Disk2Card::insert(int drive, std::unique_ptr<Disk2Image> image) {
    drive &= 1;
    ejectDisk(drive + 1);
    Floppy& f = m_drive[drive].disk;
    f = Floppy();
    f.image = std::move(image);
    f.track.assign((size_t)std::max(Disk2Image::MAX_NIBBLES, f.image ? f.image->maxTrackBytes() : 0), 0);
    chooseRom();
    return f.image != nullptr;
}

bool Disk2Card::mountDisk(int unit, const std::string& path) {
    if (unit != 1 && unit != 2) return false;
    auto img = Disk2Image::open(path);
    if (!img || img->diskType() != 1) return false;     // a 3.5" WOZ is not for the Disk II
    return insert(unit - 1, std::move(img));
}

void Disk2Card::ejectDisk(int unit) {
    if (unit != 1 && unit != 2) return;
    flush(unit - 1);
    Floppy& f = m_drive[unit - 1].disk;
    f.image.reset();
    f.trackData = false;
}

bool Disk2Card::getDiskStatus(int unit, int& track, int& sector, bool& led) {
    if (unit != 1 && unit != 2) return false;
    const Drive& d = m_drive[unit - 1];
    track = d.quarter >> 2;
    sector = 0;
    led = d.spinning > 0 && m_curr == unit - 1;
    return true;
}

std::string Disk2Card::getMountedDiskPath(int unit) {
    if (unit != 1 && unit != 2) return "";
    const Floppy& f = m_drive[unit - 1].disk;
    if (!f.image) return "";
    return f.image->path().empty() ? "(memory)" : f.image->path();
}

}  // namespace apple2
