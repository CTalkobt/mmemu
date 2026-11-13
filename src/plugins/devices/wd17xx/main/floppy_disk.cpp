#include "floppy_disk.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <unistd.h>

// ---------------------------------------------------------------------------
// CRC
// ---------------------------------------------------------------------------

uint16_t floppyCrc(uint16_t crc, uint8_t b) {
    crc ^= (uint16_t)b << 8;
    for (int i = 0; i < 8; ++i)
        crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    return crc;
}

uint16_t floppyCrcAfterA1() {
    uint16_t c = FLOPPY_CRC_INIT;
    for (int i = 0; i < 3; ++i) c = floppyCrc(c, 0xA1);
    return c;
}

// ---------------------------------------------------------------------------
// Tracks
// ---------------------------------------------------------------------------

void FloppyTrack::addIdam(int slot, bool dd) {
    clearIdams(slot, 1);
    if ((int)idams.size() >= MAX_IDAMS) return;
    uint16_t v = (uint16_t)(slot & IDAM_OFFSET) | (dd ? IDAM_DD : 0);
    auto it = std::lower_bound(idams.begin(), idams.end(), v,
        [](uint16_t a, uint16_t b) { return (a & IDAM_OFFSET) < (b & IDAM_OFFSET); });
    idams.insert(it, v);
}

void FloppyTrack::clearIdams(int from, int n) {
    int len = length();
    if (len == 0) return;
    idams.erase(std::remove_if(idams.begin(), idams.end(), [&](uint16_t v) {
        int off = v & IDAM_OFFSET;
        int d = ((off - from) % len + len) % len;
        return d < n;
    }), idams.end());
}

// ---------------------------------------------------------------------------
// Disk
// ---------------------------------------------------------------------------

FloppyDisk::FloppyDisk(int cylinders, int heads, int trackLength)
    : m_cyls(std::clamp(cylinders, 0, MAX_CYLINDERS)), m_heads(std::clamp(heads, 1, MAX_HEADS)),
      m_trackLen(trackLength > 0 ? trackLength : TRACK_LEN_DD) {
    m_tracks.resize((size_t)MAX_CYLINDERS * MAX_HEADS);
    for (int c = 0; c < m_cyls; ++c)
        for (int h = 0; h < m_heads; ++h)
            m_tracks[index(c, h)].data.assign(m_trackLen, 0x4E);
}

FloppyTrack* FloppyDisk::track(int cyl, int head) {
    if (cyl < 0 || cyl >= m_cyls || head < 0 || head >= m_heads) return nullptr;
    FloppyTrack* t = &m_tracks[index(cyl, head)];
    return t->data.empty() ? nullptr : t;
}

const FloppyTrack* FloppyDisk::track(int cyl, int head) const {
    return const_cast<FloppyDisk*>(this)->track(cyl, head);
}

FloppyTrack* FloppyDisk::extend(int cyl, int head) {
    if (cyl < 0 || cyl >= MAX_CYLINDERS || head < 0 || head >= MAX_HEADS) return nullptr;
    int newCyls = std::max(m_cyls, cyl + 1), newHeads = std::max(m_heads, head + 1);
    for (int c = 0; c < newCyls; ++c)
        for (int h = 0; h < newHeads; ++h) {
            FloppyTrack& t = m_tracks[index(c, h)];
            if (t.data.empty()) t.data.assign(m_trackLen, 0x4E);
        }
    m_cyls = newCyls;
    m_heads = newHeads;
    return &m_tracks[index(cyl, head)];
}

namespace {

/** Lays bytes down slot by slot, FM bytes twice, keeping the CRC. */
struct TrackWriter {
    std::vector<uint8_t> buf;
    std::vector<std::pair<int, bool>> idams;
    uint16_t crc = FLOPPY_CRC_INIT;
    void put(uint8_t b, bool dd, int n = 1) {
        for (int i = 0; i < n; ++i) {
            buf.push_back(b);
            if (!dd) buf.push_back(b);
            crc = floppyCrc(crc, b);
        }
    }
    void putCrc(bool dd, bool corrupt = false) {
        uint16_t c = corrupt ? (uint16_t)~crc : crc;
        put((uint8_t)(c >> 8), dd);
        put((uint8_t)c, dd);
    }
};

int sectorBytes(int sizeCode) { return 128 << (sizeCode & 3); }

void layTrack(TrackWriter& w, const std::vector<FloppySectorSpec>& secs, int gap3, bool shortPreamble) {
    bool dd0 = secs.empty() ? true : secs[0].dd;
    if (dd0) {
        if (shortPreamble) w.put(0x4E, true, 32);
        else {
            w.put(0x4E, true, 80); w.put(0x00, true, 12); w.put(0xC2, true, 3);
            w.put(0xFC, true); w.put(0x4E, true, 50);
        }
    } else {
        if (shortPreamble) w.put(0xFF, false, 16);
        else { w.put(0xFF, false, 40); w.put(0x00, false, 6); w.put(0xFC, false); w.put(0xFF, false, 26); }
    }
    for (const FloppySectorSpec& s : secs) {
        bool dd = s.dd;
        uint8_t gap = dd ? 0x4E : 0xFF;
        // ID field
        w.put(0x00, dd, dd ? 12 : 6);
        w.crc = FLOPPY_CRC_INIT;
        if (dd) w.put(0xA1, true, 3);
        w.idams.push_back({(int)w.buf.size(), dd});
        w.put(0xFE, dd);
        w.put(s.track, dd); w.put(s.side, dd); w.put(s.sector, dd); w.put(s.sizeCode, dd);
        w.putCrc(dd);
        w.put(gap, dd, dd ? 22 : 11);
        // Data field
        w.put(0x00, dd, dd ? 12 : 6);
        w.crc = FLOPPY_CRC_INIT;
        if (dd) w.put(0xA1, true, 3);
        w.put(s.dam, dd);
        int n = sectorBytes(s.sizeCode);
        for (int i = 0; i < n; ++i) w.put(i < (int)s.data.size() ? s.data[i] : 0xE5, dd);
        w.putCrc(dd, s.badCrc);
        w.put(gap, dd, gap3);
    }
}

} // namespace

bool FloppyDisk::formatTrack(int cyl, int head, const std::vector<FloppySectorSpec>& sectors) {
    FloppyTrack* t = extend(cyl, head);
    if (!t) return false;
    constexpr int MAX_LEN = 0x2940 - 128;
    int len = m_trackLen;
    int weight = 0;                         // slots a gap byte costs, summed over sectors
    for (const auto& s : sectors) weight += s.dd ? 1 : 2;
    TrackWriter probe;
    layTrack(probe, sectors, 0, false);
    bool shortPre = false;
    int gap3 = 0;
    if (weight) {
        int dflt = 54;
        int fit = ((int)len - (int)probe.buf.size()) / weight;
        if (fit < 8) {
            TrackWriter probe2;
            layTrack(probe2, sectors, 0, true);
            shortPre = true;
            fit = ((int)len - (int)probe2.buf.size()) / weight;
            if (fit < 3) {                  // lengthen the track to take them
                len = (int)probe2.buf.size() + 3 * weight;
                if (len > MAX_LEN) return false;
                fit = 3;
            }
        }
        gap3 = std::min(dflt, fit);
    }
    TrackWriter w;
    layTrack(w, sectors, gap3, shortPre);
    uint8_t fill = (sectors.empty() || sectors.back().dd) ? 0x4E : 0xFF;
    while ((int)w.buf.size() < len) w.buf.push_back(fill);
    t->data = std::move(w.buf);
    t->idams.clear();
    for (auto& [slot, dd] : w.idams) t->addIdam(slot, dd);
    m_dirty = true;
    return true;
}

namespace {

/** Read the ID and data fields from the IDAM at `slot`. */
FloppySector readFields(const FloppyTrack& t, int slot, bool dd, int* damSlot) {
    FloppySector s;
    s.dd = dd;
    int step = dd ? 1 : 2;
    uint16_t crc = dd ? floppyCrcAfterA1() : FLOPPY_CRC_INIT;
    uint8_t id[7];
    for (int k = 0; k < 7; ++k) {
        id[k] = t.at((uint64_t)slot + (uint64_t)k * step);
        crc = floppyCrc(crc, id[k]);
    }
    s.track = id[1]; s.side = id[2]; s.sector = id[3]; s.sizeCode = id[4];
    s.idCrcOk = crc == 0;
    if (damSlot) *damSlot = -1;
    // The data mark: within 43 (MFM) / 30 (FM) bytes of the ID's end
    uint64_t p = (uint64_t)slot + 7 * step;
    int window = dd ? 43 : 30;
    for (int j = 0; j < window; ++j) {
        uint64_t q = p + (uint64_t)j * step;
        uint8_t b = t.at(q);
        if (b >= 0xF8 && b <= 0xFB && (!dd || (j > 0 && t.at(q - 1) == 0xA1))) {
            s.hasData = true;
            s.dam = b;
            if (damSlot) *damSlot = (int)(q % t.length());
            uint16_t c = dd ? floppyCrcAfterA1() : FLOPPY_CRC_INIT;
            c = floppyCrc(c, b);
            int n = sectorBytes(s.sizeCode);
            s.data.resize(n);
            for (int i = 0; i < n + 2; ++i) {
                uint8_t v = t.at(q + (uint64_t)(i + 1) * step);
                if (i < n) s.data[i] = v;
                c = floppyCrc(c, v);
            }
            s.dataCrcOk = c == 0;
            break;
        }
        if (dd && b == 0xFE && j > 0 && t.at(q - 1) == 0xA1) break;   // the next ID
    }
    return s;
}

} // namespace

std::vector<FloppySector> FloppyDisk::scanTrack(int cyl, int head) const {
    std::vector<FloppySector> out;
    const FloppyTrack* t = track(cyl, head);
    if (!t) return out;
    for (uint16_t v : t->idams)
        out.push_back(readFields(*t, v & FloppyTrack::IDAM_OFFSET, (v & FloppyTrack::IDAM_DD) != 0, nullptr));
    return out;
}

bool FloppyDisk::readSector(int cyl, int head, int sector, std::vector<uint8_t>& out, uint8_t* dam) const {
    for (const FloppySector& s : scanTrack(cyl, head))
        if (s.sector == sector && s.track == cyl && s.hasData) {
            out = s.data;
            if (dam) *dam = s.dam;
            return true;
        }
    return false;
}

bool FloppyDisk::writeSector(int cyl, int head, int sector, const std::vector<uint8_t>& data, uint8_t dam) {
    FloppyTrack* t = track(cyl, head);
    if (!t) return false;
    for (uint16_t v : t->idams) {
        int damSlot;
        bool dd = (v & FloppyTrack::IDAM_DD) != 0;
        FloppySector s = readFields(*t, v & FloppyTrack::IDAM_OFFSET, dd, &damSlot);
        if (s.sector != sector || s.track != cyl || damSlot < 0) continue;
        int step = dd ? 1 : 2, len = t->length();
        uint64_t q = (uint64_t)damSlot;
        uint16_t c = dd ? floppyCrcAfterA1() : FLOPPY_CRC_INIT;
        auto put = [&](uint8_t b) {
            for (int i = 0; i < step; ++i) t->data[(q + i) % len] = b;
            q += step;
            c = floppyCrc(c, b);
        };
        put(dam);
        int n = sectorBytes(s.sizeCode);
        for (int i = 0; i < n; ++i) put(i < (int)data.size() ? data[i] : 0);
        uint16_t crc = c;
        put((uint8_t)(crc >> 8));
        put((uint8_t)crc);
        m_dirty = true;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Image formats
// ---------------------------------------------------------------------------

namespace {

std::string lowerExt(const std::string& name) {
    auto dot = name.find_last_of('.');
    auto slash = name.find_last_of('/');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return "";
    std::string e = name.substr(dot + 1);
    for (char& ch : e) ch = (char)std::tolower((unsigned char)ch);
    return e;
}

void setErr(std::string* err, const std::string& msg) { if (err) *err = msg; }

constexpr int JV3_ENTRIES = 2901;
constexpr int JV3_HEADER = JV3_ENTRIES * 3 + 1;

/** A JV3 entry's data size: used sectors code 0-3 = 256, 128, 1024, 512;
 *  free ones 512, 1024, 128, 256. */
int jv3Size(uint8_t track, uint8_t flags) {
    int code = track == 0xFF ? ((flags & 3) ^ 2) : ((flags & 3) ^ 1);
    return 128 << code;
}

bool looksDmk(const std::vector<uint8_t>& b) {
    if (b.size() < 16) return false;
    if (b[0] != 0 && b[0] != 0xFF) return false;
    int cyls = b[1];
    int tlen = b[2] | (b[3] << 8);
    if (cyls == 0 || cyls > FloppyDisk::MAX_CYLINDERS || tlen < 0x0A00 || tlen > 0x4000) return false;
    for (int i = 5; i < 12; ++i) if (b[i]) return false;
    int heads = (b[4] & 0x10) ? 1 : 2;
    size_t expect = 16 + (size_t)cyls * heads * tlen;
    return b.size() >= expect && b.size() - expect < (size_t)tlen;
}

bool looksJv3(const std::vector<uint8_t>& b) {
    if (b.size() < (size_t)JV3_HEADER + 128) return false;
    if (b[JV3_HEADER - 1] != 0 && b[JV3_HEADER - 1] != 0xFF) return false;
    size_t total = 0;
    int used = 0;
    for (int i = 0; i < JV3_ENTRIES; ++i) {
        uint8_t t = b[i * 3], s = b[i * 3 + 1], f = b[i * 3 + 2];
        if (t == 0xFF && s == 0xFF && f == 0xFF) continue;      // unused
        if (t != 0xFF) {
            if (t >= FloppyDisk::MAX_CYLINDERS) return false;
            ++used;
        }
        total += jv3Size(t, f);
        (void)s;
    }
    return used > 0 && total <= b.size() - JV3_HEADER;
}

} // namespace

const char* FloppyDisk::formatName(FloppyFormat fmt) {
    switch (fmt) {
    case FloppyFormat::JVC: return "JVC";
    case FloppyFormat::VDK: return "VDK";
    case FloppyFormat::DMK: return "DMK";
    case FloppyFormat::JV1: return "JV1";
    case FloppyFormat::JV3: return "JV3";
    default: return "auto";
    }
}

FloppyFormat FloppyDisk::detect(const std::vector<uint8_t>& b, const std::string& nameHint) {
    std::string e = lowerExt(nameHint);
    if (e == "dmk") return FloppyFormat::DMK;
    if (e == "vdk") return FloppyFormat::VDK;
    if (e == "jv1") return FloppyFormat::JV1;
    if (e == "jv3") return FloppyFormat::JV3;
    if (e == "jvc" || e == "os9") return FloppyFormat::JVC;
    if (b.size() >= 12 && b[0] == 'd' && b[1] == 'k') return FloppyFormat::VDK;
    if (looksDmk(b)) return FloppyFormat::DMK;
    if (looksJv3(b)) return FloppyFormat::JV3;
    size_t n = b.size();
    if (n && n % 256 == 0 && n % 4608 != 0 && n % 2560 == 0) return FloppyFormat::JV1;
    return FloppyFormat::JVC;
}

/** The per-format loaders and savers. */
struct FloppyCodec {
    // Sector-image tracks: the given sectors in order, one density
    static void layout(FloppyDisk& d, int cyl, int head, int nsec, int first, int sizeCode, bool dd,
                       const uint8_t* src, size_t avail, size_t stride, uint8_t dam = 0xFB) {
        std::vector<FloppySectorSpec> specs;
        int n = sectorBytes(sizeCode);
        for (int s = 0; s < nsec; ++s) {
            FloppySectorSpec sp;
            sp.track = (uint8_t)cyl; sp.side = (uint8_t)head; sp.sector = (uint8_t)(first + s);
            sp.sizeCode = (uint8_t)sizeCode; sp.dd = dd; sp.dam = dam;
            sp.data.assign(n, 0);
            size_t off = (size_t)s * stride;
            for (int i = 0; i < n && off + i < avail; ++i) sp.data[i] = src[off + i];
            specs.push_back(std::move(sp));
        }
        d.formatTrack(cyl, head, specs);
    }

    /** Sector `id` of a track as the format's file holds it (zeros if missing). */
    static void sectorOut(const std::vector<FloppySector>& secs, int id, int size, std::vector<uint8_t>& out) {
        for (const FloppySector& s : secs)
            if (s.sector == id && s.hasData) {
                for (int i = 0; i < size; ++i) out.push_back(i < (int)s.data.size() ? s.data[i] : 0);
                return;
            }
        out.insert(out.end(), size, 0);
    }

    static std::unique_ptr<FloppyDisk> loadJvc(const std::vector<uint8_t>& b, std::string* err) {
        size_t hdr = b.size() % 256;
        int spt = 18, heads = 1, code = 1, first = 1;
        bool attr = false, os9 = false;
        if (hdr > 0) {
            spt = b[0];
            if (hdr >= 2) heads = b[1];
            if (hdr >= 3) code = b[2] & 3;
            if (hdr >= 4) first = b[3];
            if (hdr >= 5) attr = b[4] != 0;
        } else if (b.size() >= 256) {
            // Headerless OS-9: LSN0's DD.TOT, DD.TKS, DD.FMT, DD.SPT
            uint32_t tot = (b[0] << 16) | (b[1] << 8) | b[2];
            int tks = b[3], fmtSides = (b[0x10] & 1) + 1, sptOs9 = (b[0x11] << 8) | b[0x12];
            if ((size_t)tot * 256 >= b.size() && tks == sptOs9 && tks > 0) {
                spt = tks;
                heads = fmtSides;
                os9 = true;
            }
        }
        if (heads < 1 || heads > 2 || spt < 1) { setErr(err, "JVC: bad header"); return nullptr; }
        size_t ssize = sectorBytes(code), bps = ssize + (attr ? 1 : 0);
        size_t data = b.size() - hdr;
        size_t perCyl = bps * spt * heads;
        size_t cyls = data / perCyl;
        if (cyls >= 88 && heads == 1) {     // too many tracks: a double-sided 720K disk
            heads = 2;
            perCyl = bps * spt * heads;
            cyls = data / perCyl;
        }
        if (data % perCyl >= bps) ++cyls;
        if (cyls == 0) cyls = 1;
        if (cyls > (size_t)FloppyDisk::MAX_CYLINDERS) { setErr(err, "JVC: too many tracks"); return nullptr; }
        auto d = std::make_unique<FloppyDisk>((int)cyls, heads);
        d->m_format = FloppyFormat::JVC;
        d->m_sectorsPerTrack = spt; d->m_sizeCode = code; d->m_firstSector = first; d->m_dd = true;
        d->m_jvcAttr = attr;
        if (hdr) d->m_jvcHeader.assign(b.begin(), b.begin() + hdr);
        d->m_jvcHeaderless = os9;
        for (size_t c = 0; c < cyls; ++c)
            for (int h = 0; h < heads; ++h) {
                size_t off = hdr + (c * heads + h) * spt * bps + (attr ? 1 : 0);
                const uint8_t* src = off < b.size() ? b.data() + off : nullptr;
                layout(*d, (int)c, h, spt, first, code, true, src, src ? b.size() - off : 0, bps);
            }
        d->m_dirty = false;
        return d;
    }

    static std::vector<uint8_t> saveJvc(const FloppyDisk& d) {
        std::vector<uint8_t> out;
        int spt = d.m_sectorsPerTrack, code = d.m_sizeCode, first = d.m_firstSector, heads = d.m_heads;
        bool standard = spt == 18 && heads == 1 && code == 1 && first == 1 && !d.m_jvcAttr;
        if (!d.m_jvcHeader.empty() || (!standard && !d.m_jvcHeaderless)) {
            std::vector<uint8_t> h = d.m_jvcHeader;
            size_t need = d.m_jvcAttr ? 5 : (first != 1 ? 4 : (code != 1 ? 3 : (heads != 1 ? 2 : 1)));
            if (h.size() < need) h.resize(need, 0);
            h[0] = (uint8_t)spt;
            if (h.size() >= 2) h[1] = (uint8_t)heads;
            if (h.size() >= 3) h[2] = (uint8_t)code;
            if (h.size() >= 4) h[3] = (uint8_t)first;
            if (h.size() >= 5) h[4] = d.m_jvcAttr ? 1 : 0;
            out = h;
        }
        int ssize = sectorBytes(code);
        for (int c = 0; c < d.m_cyls; ++c)
            for (int h = 0; h < heads; ++h) {
                auto secs = d.scanTrack(c, h);
                for (int s = 0; s < spt; ++s) {
                    if (d.m_jvcAttr) out.push_back(0);
                    sectorOut(secs, first + s, ssize, out);
                }
            }
        return out;
    }

    static std::unique_ptr<FloppyDisk> loadVdk(const std::vector<uint8_t>& b, std::string* err) {
        if (b.size() < 12 || b[0] != 'd' || b[1] != 'k') { setErr(err, "VDK: bad header"); return nullptr; }
        size_t hdr = b[2] | (b[3] << 8);
        if (hdr < 12 || hdr > b.size()) { setErr(err, "VDK: bad header size"); return nullptr; }
        if (b[11] & 7) { setErr(err, "VDK: compressed images are not supported"); return nullptr; }
        int cyls = b[8], heads = b[9];
        if (heads < 1 || heads > 2 || cyls < 1 || cyls > FloppyDisk::MAX_CYLINDERS) {
            setErr(err, "VDK: bad geometry"); return nullptr;
        }
        auto d = std::make_unique<FloppyDisk>(cyls, heads);
        d->m_format = FloppyFormat::VDK;
        d->m_writeProtect = b[10] & 1;
        d->m_vdkHeader.assign(b.begin(), b.begin() + hdr);
        for (int c = 0; c < cyls; ++c)
            for (int h = 0; h < heads; ++h) {
                size_t off = hdr + (size_t)(c * heads + h) * 18 * 256;
                const uint8_t* src = off < b.size() ? b.data() + off : nullptr;
                layout(*d, c, h, 18, 1, 1, true, src, src ? b.size() - off : 0, 256);
            }
        d->m_dirty = false;
        return d;
    }

    static std::vector<uint8_t> saveVdk(const FloppyDisk& d) {
        std::vector<uint8_t> out = d.m_vdkHeader;
        if (out.size() < 12) out = {'d', 'k', 12, 0, 0x10, 0x10, 'M', 0, 0, 0, 0, 0};
        out[8] = (uint8_t)d.m_cyls;
        out[9] = (uint8_t)d.m_heads;
        out[10] = (uint8_t)((out[10] & ~1) | (d.m_writeProtect ? 1 : 0));
        for (int c = 0; c < d.m_cyls; ++c)
            for (int h = 0; h < d.m_heads; ++h) {
                auto secs = d.scanTrack(c, h);
                for (int s = 1; s <= 18; ++s) sectorOut(secs, s, 256, out);
            }
        return out;
    }

    static std::unique_ptr<FloppyDisk> loadDmk(const std::vector<uint8_t>& b, std::string* err) {
        if (b.size() < 16) { setErr(err, "DMK: short header"); return nullptr; }
        int cyls = b[1];
        int tlen = b[2] | (b[3] << 8);
        int heads = (b[4] & 0x10) ? 1 : 2;
        bool sd = (b[4] & 0x40) != 0;
        if (tlen <= 128 || tlen > 0x4000 || cyls < 1 || cyls > FloppyDisk::MAX_CYLINDERS) {
            setErr(err, "DMK: bad geometry"); return nullptr;
        }
        int dataLen = (tlen - 128) * (sd ? 2 : 1);
        auto d = std::make_unique<FloppyDisk>(cyls, heads, dataLen);
        d->m_format = FloppyFormat::DMK;
        d->m_writeProtect = b[0] == 0xFF;
        d->m_dmkSingleDensity = sd;
        for (int c = 0; c < cyls; ++c)
            for (int h = 0; h < heads; ++h) {
                size_t base = 16 + (size_t)(c * heads + h) * tlen;
                FloppyTrack* t = d->track(c, h);
                auto byteAt = [&](size_t i) -> uint8_t { return base + i < b.size() ? b[base + i] : 0; };
                for (int i = 0; i < tlen - 128; ++i) {
                    uint8_t v = byteAt(128 + i);
                    if (sd) { t->data[2 * i] = v; t->data[2 * i + 1] = v; }
                    else t->data[i] = v;
                }
                t->idams.clear();
                for (int k = 0; k < 64; ++k) {
                    uint16_t p = byteAt(2 * k) | (byteAt(2 * k + 1) << 8);
                    int off = p & FloppyTrack::IDAM_OFFSET;
                    if (off == 0) break;
                    off -= 128;
                    if (off < 0 || off >= tlen - 128) continue;
                    t->addIdam(sd ? off * 2 : off, (p & FloppyTrack::IDAM_DD) != 0);
                }
            }
        d->m_dirty = false;
        return d;
    }

    static std::vector<uint8_t> saveDmk(const FloppyDisk& d) {
        int maxLen = 0;
        bool allSd = true;
        for (int c = 0; c < d.m_cyls; ++c)
            for (int h = 0; h < d.m_heads; ++h)
                if (const FloppyTrack* t = d.track(c, h)) {
                    maxLen = std::max(maxLen, t->length());
                    for (uint16_t v : t->idams) if (v & FloppyTrack::IDAM_DD) allSd = false;
                    if (t->length() & 1) allSd = false;
                }
        if (maxLen == 0) maxLen = d.m_trackLen;
        bool halve = d.m_dmkSingleDensity && allSd && !(maxLen & 1);
        int tdata = halve ? maxLen / 2 : maxLen;
        int tlen = tdata + 128;
        std::vector<uint8_t> out(16, 0);
        out[0] = d.m_writeProtect ? 0xFF : 0;
        out[1] = (uint8_t)d.m_cyls;
        out[2] = (uint8_t)tlen;
        out[3] = (uint8_t)(tlen >> 8);
        out[4] = (uint8_t)((d.m_heads == 1 ? 0x10 : 0) | (halve ? 0x40 : 0));
        for (int c = 0; c < d.m_cyls; ++c)
            for (int h = 0; h < d.m_heads; ++h) {
                const FloppyTrack* t = d.track(c, h);
                std::vector<uint8_t> trk(tlen, 0);
                if (t) {
                    int k = 0;
                    for (uint16_t v : t->idams) {
                        int off = (v & FloppyTrack::IDAM_OFFSET);
                        off = (halve ? off / 2 : off) + 128;
                        uint16_t p = (uint16_t)off | (v & FloppyTrack::IDAM_DD);
                        trk[2 * k] = (uint8_t)p;
                        trk[2 * k + 1] = (uint8_t)(p >> 8);
                        if (++k == 64) break;
                    }
                    uint8_t pad = t->data.empty() ? 0 : t->data.back();
                    for (int i = 0; i < tdata; ++i) {
                        int src = halve ? 2 * i : i;
                        trk[128 + i] = src < t->length() ? t->data[src] : pad;
                    }
                }
                out.insert(out.end(), trk.begin(), trk.end());
            }
        return out;
    }

    static std::unique_ptr<FloppyDisk> loadJv1(const std::vector<uint8_t>& b, std::string* err) {
        size_t cyls = (b.size() + 2559) / 2560;
        if (cyls == 0) { setErr(err, "JV1: empty image"); return nullptr; }
        if (cyls > (size_t)FloppyDisk::MAX_CYLINDERS) { setErr(err, "JV1: too many tracks"); return nullptr; }
        auto d = std::make_unique<FloppyDisk>((int)cyls, 1);
        d->m_format = FloppyFormat::JV1;
        d->m_sectorsPerTrack = 10; d->m_sizeCode = 1; d->m_firstSector = 0; d->m_dd = false;
        for (size_t c = 0; c < cyls; ++c) {
            size_t off = c * 2560;
            // The directory (track 17) is written with the $FA data mark
            layout(*d, (int)c, 0, 10, 0, 1, false, b.data() + off, b.size() - off, 256, c == 17 ? 0xFA : 0xFB);
        }
        d->m_dirty = false;
        return d;
    }

    static std::vector<uint8_t> saveJv1(const FloppyDisk& d) {
        std::vector<uint8_t> out;
        for (int c = 0; c < d.m_cyls; ++c) {
            auto secs = d.scanTrack(c, 0);
            for (int s = 0; s < 10; ++s) sectorOut(secs, s, 256, out);
        }
        return out;
    }

    static uint8_t jv3Dam(bool dd, int bits) {
        if (dd) return bits == 1 ? 0xF8 : 0xFB;
        static const uint8_t sd[4] = {0xFB, 0xFA, 0xF9, 0xF8};
        return sd[bits & 3];
    }

    static std::unique_ptr<FloppyDisk> loadJv3(const std::vector<uint8_t>& b, std::string* err) {
        if (b.size() < (size_t)JV3_HEADER) { setErr(err, "JV3: short header"); return nullptr; }
        std::map<std::pair<int, int>, std::vector<FloppySectorSpec>> tracks;
        int maxCyl = 0, heads = 1;
        bool wp = false;
        size_t block = 0;
        for (int blk = 0; blk < 2 && block + JV3_HEADER <= b.size(); ++blk) {
            const uint8_t* h = b.data() + block;
            if (blk == 0) wp = h[JV3_HEADER - 1] == 0;
            size_t off = block + JV3_HEADER;
            bool full = true;
            for (int i = 0; i < JV3_ENTRIES; ++i) {
                uint8_t t = h[i * 3], s = h[i * 3 + 1], f = h[i * 3 + 2];
                if (t == 0xFF && s == 0xFF && f == 0xFF) { full = false; continue; }
                int size = jv3Size(t, f);
                if (t != 0xFF) {
                    FloppySectorSpec sp;
                    sp.dd = (f & 0x80) != 0;
                    sp.track = t; sp.sector = s;
                    sp.side = (f & 0x10) ? 1 : 0;
                    sp.sizeCode = (uint8_t)((f & 3) ^ 1);
                    sp.dam = jv3Dam(sp.dd, (f >> 5) & 3);
                    sp.badCrc = (f & 0x08) != 0;
                    sp.data.assign(size, 0);
                    for (int k = 0; k < size && off + k < b.size(); ++k) sp.data[k] = b[off + k];
                    maxCyl = std::max(maxCyl, (int)t);
                    if (sp.side) heads = 2;
                    tracks[{t, sp.side}].push_back(std::move(sp));
                }
                off += size;
            }
            if (!full) break;               // a second block only follows a full first one
            block = off;
        }
        if (tracks.empty()) { setErr(err, "JV3: no sectors"); return nullptr; }
        if (maxCyl >= FloppyDisk::MAX_CYLINDERS) { setErr(err, "JV3: too many tracks"); return nullptr; }
        auto d = std::make_unique<FloppyDisk>(maxCyl + 1, heads);
        d->m_format = FloppyFormat::JV3;
        d->m_writeProtect = wp;
        for (auto& [key, specs] : tracks)
            if (!d->formatTrack(key.first, key.second, specs)) {
                setErr(err, "JV3: track " + std::to_string(key.first) + " does not fit");
                return nullptr;
            }
        d->m_dirty = false;
        return d;
    }

    static std::vector<uint8_t> saveJv3(const FloppyDisk& d) {
        struct Entry { uint8_t t, s, f; std::vector<uint8_t> data; };
        std::vector<Entry> entries;
        for (int c = 0; c < d.m_cyls; ++c)
            for (int h = 0; h < d.m_heads; ++h)
                for (const FloppySector& s : d.scanTrack(c, h)) {
                    if (!s.hasData) continue;
                    uint8_t f = (uint8_t)((s.sizeCode & 3) ^ 1);
                    if (s.dd) { f |= 0x80; if (s.dam == 0xF8 || s.dam == 0xF9) f |= 0x20; }
                    else f |= (uint8_t)(((0xFB - s.dam) & 3) << 5);
                    if (h) f |= 0x10;
                    if (!s.dataCrcOk) f |= 0x08;
                    entries.push_back({(uint8_t)c, s.sector, f, s.data});
                }
        std::vector<uint8_t> out;
        size_t i = 0;
        do {
            size_t n = std::min(entries.size() - i, (size_t)JV3_ENTRIES);
            std::vector<uint8_t> hdr(JV3_HEADER, 0xFF);
            for (size_t k = 0; k < n; ++k) {
                hdr[k * 3] = entries[i + k].t;
                hdr[k * 3 + 1] = entries[i + k].s;
                hdr[k * 3 + 2] = entries[i + k].f;
            }
            hdr[JV3_HEADER - 1] = d.m_writeProtect ? 0 : 0xFF;
            out.insert(out.end(), hdr.begin(), hdr.end());
            for (size_t k = 0; k < n; ++k)
                out.insert(out.end(), entries[i + k].data.begin(), entries[i + k].data.end());
            i += n;
        } while (i < entries.size() && i % JV3_ENTRIES == 0 && i < 2 * (size_t)JV3_ENTRIES);
        return out;
    }
};

std::unique_ptr<FloppyDisk> FloppyDisk::fromBytes(const std::vector<uint8_t>& bytes, FloppyFormat fmt,
                                                  std::string* err, const std::string& nameHint) {
    if (fmt == FloppyFormat::Auto) fmt = detect(bytes, nameHint);
    switch (fmt) {
    case FloppyFormat::JVC: return FloppyCodec::loadJvc(bytes, err);
    case FloppyFormat::VDK: return FloppyCodec::loadVdk(bytes, err);
    case FloppyFormat::DMK: return FloppyCodec::loadDmk(bytes, err);
    case FloppyFormat::JV1: return FloppyCodec::loadJv1(bytes, err);
    case FloppyFormat::JV3: return FloppyCodec::loadJv3(bytes, err);
    default: setErr(err, "unknown disk image format"); return nullptr;
    }
}

std::unique_ptr<FloppyDisk> FloppyDisk::load(const std::string& path, std::string* err, FloppyFormat fmt) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { setErr(err, "cannot open " + path); return nullptr; }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto d = fromBytes(bytes, fmt, err, path);
    if (!d) return nullptr;
    d->m_path = path;
    d->m_readOnlyFile = ::access(path.c_str(), W_OK) != 0;
    return d;
}

std::vector<uint8_t> FloppyDisk::toBytes(FloppyFormat fmt, std::string* err) const {
    if (fmt == FloppyFormat::Auto) fmt = m_format;
    switch (fmt) {
    case FloppyFormat::JVC: return FloppyCodec::saveJvc(*this);
    case FloppyFormat::VDK: return FloppyCodec::saveVdk(*this);
    case FloppyFormat::DMK: return FloppyCodec::saveDmk(*this);
    case FloppyFormat::JV1: return FloppyCodec::saveJv1(*this);
    case FloppyFormat::JV3: return FloppyCodec::saveJv3(*this);
    default: setErr(err, "unknown disk image format"); return {};
    }
}

bool FloppyDisk::save(std::string* err) {
    if (m_path.empty()) { setErr(err, "the disk has no file"); return false; }
    if (m_readOnlyFile) { setErr(err, m_path + " is read-only: changes kept in memory"); return false; }
    std::vector<uint8_t> bytes = toBytes(m_format, err);
    std::ofstream f(m_path, std::ios::binary | std::ios::trunc);
    if (!f) {
        m_readOnlyFile = true;
        setErr(err, "cannot write " + m_path + ": changes kept in memory");
        return false;
    }
    f.write((const char*)bytes.data(), (std::streamsize)bytes.size());
    if (!f) { setErr(err, "write failed: " + m_path); return false; }
    m_dirty = false;
    return true;
}

bool FloppyDisk::saveAs(const std::string& path, FloppyFormat fmt, std::string* err) {
    if (fmt == FloppyFormat::Auto) {
        std::string e = lowerExt(path);
        fmt = e == "dmk" ? FloppyFormat::DMK : e == "vdk" ? FloppyFormat::VDK
            : e == "jv1" ? FloppyFormat::JV1 : e == "jv3" ? FloppyFormat::JV3
            : e == "jvc" || e == "os9" ? FloppyFormat::JVC : m_format;
    }
    std::string oldPath = m_path;
    FloppyFormat oldFmt = m_format;
    bool oldRo = m_readOnlyFile;
    m_path = path;
    m_format = fmt;
    m_readOnlyFile = false;
    if (!save(err)) {
        m_path = oldPath; m_format = oldFmt; m_readOnlyFile = oldRo;
        return false;
    }
    return true;
}
