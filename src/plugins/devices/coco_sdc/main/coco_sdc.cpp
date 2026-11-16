// The CoCo SDC's command interface, SD card folder and flash banks (#356),
// after the CoCo SDC User Guide (v4): see coco_sdc.h.

#include "coco_sdc.h"
#include "plugins/devices/wd17xx/main/floppy_disk.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {
constexpr int SPT = 18;                        // DSK sectors a track
constexpr uint32_t DSK_MIN = 324;              // sectors: 18 single-sided tracks
constexpr uint32_t FLOPPY_MAX = 2880;          // sectors: 80 double-sided tracks
constexpr uint32_t HD_FLOPPY_VIEW = 1440;      // a hard disk's sectors the FDC sees
constexpr uint32_t NEW_DSK_SECTORS = 630;      // 'N' with zero parameters
constexpr int SDF_HEADER = 512, SDF_TRACK_HEADER = 256, SDF_TRACK_DATA = 6250, SDF_RECORD = 6656;
constexpr int SDF_ENTRIES = 31;

std::string upper(std::string s) {
    for (char& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}

bool wild(const std::string& s) { return s.find_first_of("*?") != std::string::npos; }

/** "NAME.EXT" as an 8.3 pair; false if it doesn't fit (or has wildcards when not allowed). */
bool split83(const std::string& name, std::string& base, std::string& ext, bool allowWild = false) {
    if (name == "." || name == "..") { base = name; ext.clear(); return true; }
    if (name.empty()) return false;
    const auto dot = name.find_last_of('.');
    base = dot == std::string::npos ? name : name.substr(0, dot);
    ext = dot == std::string::npos ? "" : name.substr(dot + 1);
    if (base.empty() || base.size() > 8 || ext.size() > 3 || base.find('.') != std::string::npos) return false;
    for (char c : base + ext) {
        const unsigned char u = (unsigned char)c;
        if (u < 0x21 || u > 0x7E || std::strchr("\"+,/:;<=>[\\]|", c)) return false;
        if (!allowWild && (c == '*' || c == '?')) return false;
    }
    return true;
}

bool globMatch(const char* p, const char* s) {
    if (!*p) return !*s;
    if (*p == '*') {
        for (const char* t = s;; ++t) {
            if (globMatch(p + 1, t)) return true;
            if (!*t) return false;
        }
    }
    if (!*s) return false;
    if (*p != '?' && std::toupper((unsigned char)*p) != std::toupper((unsigned char)*s)) return false;
    return globMatch(p + 1, s + 1);
}

/** 8.3 matching: "*.*" matches every name; a name without a dot matches "X.*" as "X.". */
bool patternMatch(const std::string& pattern, const std::string& name) {
    if (pattern == "*.*" || pattern == "*") return true;
    std::string n = name;
    if (n.find('.') == std::string::npos && pattern.find('.') != std::string::npos) n += '.';
    return globMatch(pattern.c_str(), n.c_str());
}

bool writable(const std::string& path) { return ::access(path.c_str(), W_OK) == 0; }

bool readAll(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

// ---------------------------------------------------------------------------
// SDF (guide chapter 6): a 512-byte header, then per track a 256-byte header
// (entries used, 7 reserved, 31 x 8-byte sector entries) and 6250 raw bytes
// (single-density bytes doubled), padded to 6656.
// ---------------------------------------------------------------------------
std::unique_ptr<FloppyDisk> sdfLoad(const std::vector<uint8_t>& b) {
    if (b.size() < (size_t)SDF_HEADER || std::memcmp(b.data(), "SDF", 3) != 0) return nullptr;
    const int cyls = b[4], sides = b[5];
    if (cyls < 1 || cyls > 80 || sides < 1 || sides > 2) return nullptr;
    auto disk = std::make_unique<FloppyDisk>(cyls, sides, SDF_TRACK_DATA);
    for (int c = 0; c < cyls; ++c)
        for (int h = 0; h < sides; ++h) {
            const size_t rec = SDF_HEADER + (size_t)(c * sides + h) * SDF_RECORD;
            FloppyTrack* t = disk->extend(c, h);
            if (!t) return nullptr;
            if (rec + SDF_TRACK_HEADER + SDF_TRACK_DATA > b.size()) continue;   // a short file: unformatted
            t->data.assign(b.begin() + (long)(rec + SDF_TRACK_HEADER), b.begin() + (long)(rec + SDF_TRACK_HEADER + SDF_TRACK_DATA));
            t->idams.clear();
            const int n = std::min<int>(b[rec], SDF_ENTRIES);
            for (int i = 0; i < n; ++i) {
                const size_t e = rec + 8 + (size_t)i * 8;
                const uint16_t v = (uint16_t)(b[e] | b[e + 1] << 8);
                const int off = (int)(v & 0x3FFF) - SDF_TRACK_HEADER;
                if (off >= 0 && off < SDF_TRACK_DATA) t->addIdam(off, !(v & 0x4000));
            }
        }
    disk->setWriteProtected(b[6] == 0xFF);
    return disk;
}

std::vector<uint8_t> sdfSave(const FloppyDisk& d) {
    const int cyls = d.cylinders(), sides = d.heads();
    std::vector<uint8_t> out((size_t)SDF_HEADER + (size_t)cyls * sides * SDF_RECORD, 0);
    std::memcpy(out.data(), "SDF1", 4);
    out[4] = (uint8_t)cyls;
    out[5] = (uint8_t)sides;
    out[6] = d.writeProtected() ? 0xFF : 0x00;
    for (int c = 0; c < cyls; ++c)
        for (int h = 0; h < sides; ++h) {
            const size_t rec = SDF_HEADER + (size_t)(c * sides + h) * SDF_RECORD;
            const FloppyTrack* t = d.track(c, h);
            uint8_t* raw = &out[rec + SDF_TRACK_HEADER];
            std::fill(raw, raw + SDF_TRACK_DATA, 0x4E);
            if (!t) continue;
            for (int i = 0; i < SDF_TRACK_DATA && i < t->length(); ++i) raw[i] = t->data[(size_t)i];
            const std::vector<FloppySector> found = d.scanTrack(c, h);
            int n = 0;
            for (size_t i = 0; i < t->idams.size() && n < SDF_ENTRIES && i < found.size(); ++i) {
                const int slot = t->idams[i] & FloppyTrack::IDAM_OFFSET;
                const bool dd = (t->idams[i] & FloppyTrack::IDAM_DD) != 0;
                if (slot >= SDF_TRACK_DATA) continue;
                const FloppySector& s = found[i];
                uint16_t id = (uint16_t)(slot + SDF_TRACK_HEADER);
                if (!dd) id |= 0x4000;
                if (!s.idCrcOk) id |= 0x8000;
                // The data field: the mark within 43 (MFM) / 30 (FM) bytes after the ID
                uint16_t data = 0;
                const int step = dd ? 1 : 2;
                const uint64_t p = (uint64_t)slot + 7 * step;
                for (int j = 0; j < (dd ? 43 : 30); ++j) {
                    const uint64_t q = p + (uint64_t)j * step;
                    const uint8_t m = t->at(q);
                    if (m >= 0xF8 && m <= 0xFB && (!dd || (j > 0 && t->at(q - 1) == 0xA1))) {
                        data = (uint16_t)((q % (uint64_t)t->length()) + SDF_TRACK_HEADER);
                        if (m == 0xF8 || m == 0xF9) data |= 0x4000;
                        if (!s.dataCrcOk) data |= 0x8000;
                        break;
                    }
                }
                uint8_t* e = &out[rec + 8 + (size_t)n * 8];
                e[0] = (uint8_t)id; e[1] = (uint8_t)(id >> 8);
                e[2] = (uint8_t)data; e[3] = (uint8_t)(data >> 8);
                e[4] = s.track; e[5] = s.side; e[6] = s.sector; e[7] = s.sizeCode;
                ++n;
            }
            out[rec] = (uint8_t)n;
        }
    return out;
}

bool writeFile(const std::string& path, const std::vector<uint8_t>& bytes) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write((const char*)bytes.data(), (std::streamsize)bytes.size());
    return (bool)f;
}
}  // namespace

CocoSdc::CocoSdc() : m_flash((size_t)BANKS * BANK_SIZE, 0xFF) {}

CocoSdc::~CocoSdc() {
    for (int d = 0; d < 2; ++d) eject(d);
}

void CocoSdc::setCardRoot(const std::string& dir) {
    for (int d = 0; d < 2; ++d) eject(d);
    m_root = dir;
    while (m_root.size() > 1 && m_root.back() == '/') m_root.pop_back();
    m_cwd.clear();
    m_listing = false;
    m_list.clear();
    m_setDir.clear();
    readStartup();
}

void CocoSdc::readStartup() {
    if (m_root.empty()) return;
    std::ifstream f(m_root + "/" + findCase(m_root, "STARTUP.CFG"));
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.size() < 3 || line[1] != '=') continue;
        const std::string arg = line.substr(2);
        switch (std::toupper((unsigned char)line[0])) {
        case '0': mountPath(0, arg, false); break;
        case '1': mountPath(1, arg, false); break;
        case 'D': changeDir(arg); break;
        default: break;
        }
    }
}

std::string CocoSdc::currentDir() const {
    std::string s;
    for (const auto& c : m_cwd) s += (s.empty() ? "" : "/") + c;
    return s;
}

void CocoSdc::loadFlash(const std::vector<uint8_t>& image) {
    std::fill(m_flash.begin(), m_flash.end(), 0xFF);
    const size_t n = std::min(image.size(), m_flash.size());
    std::copy(image.begin(), image.begin() + (long)n, m_flash.begin());
    // A smaller ROM (8 KB) repeats through its bank
    if (n && n < (size_t)BANK_SIZE)
        for (size_t i = n; i < (size_t)BANK_SIZE; ++i) m_flash[i] = image[i % n];
    m_hasFlash = n > 0;
}

uint8_t CocoSdc::romByte(uint16_t addr) const {
    return m_flash[(size_t)m_bank * BANK_SIZE + (addr & (BANK_SIZE - 1))];
}

void CocoSdc::reset() {
    m_cmdMode = false;
    m_bank = m_dipBank;
    m_phase = Phase::Idle;
    m_error = 0;
    m_pos = m_len = 0;
    std::memset(m_param, 0, sizeof m_param);
}

bool CocoSdc::writeLatch(uint8_t v) {
    m_cmdMode = v == COMMAND_MODE;
    return m_cmdMode;
}

uint8_t CocoSdc::composeStatus() const {
    if (m_phase != Phase::Idle) return (uint8_t)(ST_BUSY | ST_READY | (m_error & ST_IN_USE));
    return m_error;
}

// ---------------------------------------------------------------------------
// Registers
// ---------------------------------------------------------------------------
bool CocoSdc::peek(int reg, uint8_t& v) const {
    reg &= 0x0F;
    if (reg == 3) { v = (uint8_t)m_bank; return true; }
    if (!m_cmdMode || reg < 8 || reg > 11) return false;
    const bool data = m_phase == Phase::DataIn || m_phase == Phase::Stream;
    if (reg == 8) v = composeStatus();
    else if (data && (reg == 11 || (reg == 10 && !m_eightBit))) v = m_buf[m_pos];
    else v = m_param[reg - 8];
    return true;
}

bool CocoSdc::read(int reg, uint8_t& v) {
    if (!peek(reg, v)) return false;
    reg &= 0x0F;
    const bool data = m_phase == Phase::DataIn || m_phase == Phase::Stream;
    if (m_cmdMode && data && (reg == 11 || (reg == 10 && !m_eightBit)) && ++m_pos >= m_len) {
        if (m_phase == Phase::Stream) {
            ++m_streamBlock;
            streamBlock();
        } else {
            m_phase = Phase::Idle;
            m_pos = 0;
        }
    }
    return true;
}

bool CocoSdc::write(int reg, uint8_t v) {
    reg &= 0x0F;
    if (reg == 3) { setBank(v); return true; }
    if (reg == 2) return true;                  // flash data: programming not modelled
    if (!m_cmdMode || reg < 8 || reg > 11) return false;
    if (reg == 8) { command(v); return true; }
    if (m_phase == Phase::DataOut && (reg == 11 || (reg == 10 && !m_eightBit))) {
        m_buf[m_pos++] = v;
        if (m_pos >= m_len) finishDataOut();
        return true;
    }
    m_param[reg - 8] = v;
    return true;
}

void CocoSdc::fail(uint8_t bits) {
    m_error = (uint8_t)(ST_FAILED | bits);
    m_phase = Phase::Idle;
}

void CocoSdc::beginDataIn(bool eightBit) {
    m_phase = Phase::DataIn;
    m_eightBit = eightBit;
    m_pos = 0;
    m_len = 256;
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------
void CocoSdc::command(uint8_t c) {
    m_param[0] = c;
    m_error = 0;
    m_pos = 0;
    m_phase = Phase::Idle;
    m_eightBit = false;
    const int drive = c & 1;
    switch (c & 0xF0) {
    case 0x80:
        if (c & 0x08) { fail(0); return; }
        if (readLsn(drive, lsn(), c & 0x02)) beginDataIn(c & 0x04);
        return;
    case 0xA0: {
        Image& im = m_images[drive];
        if (im.kind == Kind::None) { fail(ST_NOT_FOUND); return; }
        if (im.readOnly) { fail(ST_WRITE_PROTECT); return; }
        m_phase = Phase::DataOut;
        m_eightBit = c & 0x08;
        m_len = 256;
        return;
    }
    case 0x90:
        if (m_images[drive].kind == Kind::None) { fail(0); return; }
        m_streamDrive = drive;
        m_streamBlock = lsn();
        m_eightBit = c & 0x04;
        streamBlock();
        return;
    case 0xC0:
        extended(c);
        return;
    case 0xD0:
        if (m_param[1] == '#') {
            const uint8_t r = mountSet(m_param[2], m_param[2] == 0);
            if (r) fail(r);
        }
        // else: abort stream (already idle)
        return;
    case 0xE0:
        m_phase = Phase::DataOut;
        m_len = 256;
        return;
    default:
        fail(0);
        return;
    }
}

void CocoSdc::extended(uint8_t c) {
    const int drive = c & 1;
    Image& im = m_images[drive];
    std::memset(m_buf, 0, 256);
    switch (m_param[1]) {
    case 'I':
        if (im.kind == Kind::None) { fail(0); return; }
        record32(im.path, m_buf, im.kind == Kind::Sdf);
        beginDataIn();
        return;
    case 'Q': {
        if (im.kind == Kind::None) { fail(0); return; }
        const uint32_t n = (uint32_t)std::min<uint64_t>(fileSize(drive) / 256, 0xFFFFFF);
        m_param[1] = (uint8_t)(n >> 16);
        m_param[2] = (uint8_t)(n >> 8);
        m_param[3] = (uint8_t)n;
        return;
    }
    case 'C': {
        if (m_cwd.empty()) { fail(ST_NOT_FOUND); return; }
        std::string host = m_root;
        for (const auto& q : m_cwd) host += "/" + q;
        record32(host, m_buf, false);
        beginDataIn();
        return;
    }
    case '>':
        if (!m_listing) { fail(ST_HARDWARE); return; }
        for (int i = 0; i < 16 && m_listPos < m_list.size(); ++i) record16(m_list[m_listPos++], m_buf + i * 16);
        beginDataIn();
        return;
    case '+': {
        const uint8_t r = mountSet(0, true);
        if (r) fail(r & ~ST_NOT_FOUND);         // the guide lists only FAILED here
        return;
    }
    case 'V':
        m_param[2] = (uint8_t)(VERSION >> 8);
        m_param[3] = (uint8_t)VERSION;
        return;
    default:
        fail(0);
        return;
    }
}

std::string CocoSdc::blockString() const {
    std::string s;
    for (int i = 2; i < 256 && m_buf[i]; ++i) s += (char)m_buf[i];
    return s;
}

void CocoSdc::finishDataOut() {
    m_phase = Phase::Idle;
    m_pos = 0;
    const uint8_t c = m_param[0];
    const int drive = c & 1;
    if ((c & 0xF0) == 0xA0) {
        writeLsn(drive, lsn(), c & 0x02);
        return;
    }
    if (m_buf[1] != ':') { fail(ST_INVALID); return; }
    const std::string path = blockString();
    uint8_t r = 0;
    switch (m_buf[0]) {
    case 'M': r = mountPath(drive, path, false); break;
    case 'm': r = mountPath(drive, path, true); break;
    case 'N': r = newImage(drive, path, false); break;
    case 'n': r = newImage(drive, path, true); break;
    case 'D': r = changeDir(path); break;
    case 'L': r = startListing(path); break;
    case 'K': {
        std::string host;
        r = resolve(path, host, false);
        std::error_code ec;
        if (!r && fs::exists(host, ec)) r = ST_HARDWARE;
        if (!r && !fs::create_directory(host, ec)) r = ST_HARDWARE;
        break;
    }
    case 'X': {
        std::string host;
        r = resolve(path, host, true);
        if (r) break;
        for (int d = 0; d < 2; ++d)
            if (m_images[d].path == host) { r = ST_IN_USE; break; }
        if (r) break;
        std::error_code ec;
        if (fs::is_directory(host, ec) && !fs::is_empty(host, ec)) { r = ST_IN_USE; break; }
        if (!fs::remove(host, ec)) r = ST_HARDWARE;
        break;
    }
    default:
        r = ST_INVALID;
        break;
    }
    if (r) fail(r);
}

// ---------------------------------------------------------------------------
// Sectors
// ---------------------------------------------------------------------------
uint64_t CocoSdc::fileSize(int drive) const {
    std::error_code ec;
    const uint64_t n = fs::file_size(m_images[drive].path, ec);
    return ec ? 0 : n;
}

bool CocoSdc::readLsn(int drive, uint32_t n, bool singleSided) {
    Image& im = m_images[drive];
    if (im.kind == Kind::None) { fail(ST_NOT_FOUND); return false; }
    if (im.kind == Kind::HardDisk || im.kind == Kind::Raw) {
        if ((uint64_t)n * 256 + 256 > fileSize(drive)) { fail(ST_NOT_FOUND); return false; }
        im.file->clear();
        im.file->seekg((std::streamoff)n * 256);
        im.file->read((char*)m_buf, 256);
        if (im.file->gcount() != 256) { fail(ST_NOT_FOUND); return false; }
        return true;
    }
    FloppyDisk* d = floppy ? floppy(drive) : nullptr;
    if (!d) { fail(ST_NOT_FOUND); return false; }
    const int heads = d->heads();
    if (singleSided && heads == 2) n = n / SPT * 2 * SPT + n % SPT;
    const int track = (int)(n / SPT), cyl = track / heads, head = track % heads, sec = (int)(n % SPT) + 1;
    if (cyl >= d->cylinders()) { fail(ST_NOT_FOUND); return false; }
    for (const FloppySector& s : d->scanTrack(cyl, head)) {
        if (s.sector != sec || s.track != cyl) continue;
        if (!s.idCrcOk || !s.hasData) break;
        std::memset(m_buf, 0, 256);
        std::memcpy(m_buf, s.data.data(), std::min<size_t>(256, s.data.size()));
        // A deleted data mark shows at once; a data CRC error once the data is read
        m_error = (s.dam == 0xF8 || s.dam == 0xF9) ? ST_IN_USE : 0;
        if (!s.dataCrcOk) m_error |= ST_FAILED | ST_HARDWARE;
        return true;
    }
    fail(ST_NOT_FOUND);
    return false;
}

void CocoSdc::writeLsn(int drive, uint32_t n, bool singleSided) {
    Image& im = m_images[drive];
    if (im.kind == Kind::None) { fail(ST_NOT_FOUND); return; }
    if (im.readOnly) { fail(ST_WRITE_PROTECT); return; }
    const std::vector<uint8_t> data(m_buf, m_buf + 256);
    if (im.kind == Kind::HardDisk || im.kind == Kind::Raw) {
        if (im.kind == Kind::HardDisk && (uint64_t)n * 256 + 256 > fileSize(drive)) { fail(ST_NOT_FOUND); return; }
        im.file->clear();
        im.file->seekp((std::streamoff)n * 256);
        im.file->write((const char*)m_buf, 256);
        im.file->flush();
        if (!*im.file) { fail(ST_HARDWARE); return; }
        // The FDC's view of a hard disk's first sectors follows
        if (im.kind == Kind::HardDisk && n < HD_FLOPPY_VIEW && floppy && floppy(drive))
            floppy(drive)->writeSector((int)(n / SPT), 0, (int)(n % SPT) + 1, data);
        return;
    }
    FloppyDisk* d = floppy ? floppy(drive) : nullptr;
    if (!d) { fail(ST_NOT_FOUND); return; }
    if (d->writeProtected()) { fail(ST_WRITE_PROTECT); return; }
    const int heads = d->heads();
    if (singleSided && heads == 2) n = n / SPT * 2 * SPT + n % SPT;
    const int track = (int)(n / SPT), cyl = track / heads;
    if (cyl >= d->cylinders() || !d->writeSector(cyl, track % heads, (int)(n % SPT) + 1, data)) {
        fail(ST_NOT_FOUND);
        return;
    }
    d->markDirty();
}

void CocoSdc::streamBlock() {
    Image& im = m_images[m_streamDrive];
    std::ifstream f(im.path, std::ios::binary);
    if (f) {
        f.seekg((std::streamoff)(m_streamBlock * 512));
        f.read((char*)m_buf, 512);
    }
    const auto got = f ? 512 : (int)f.gcount();
    if (!f && got <= 0) {                       // past the end: done, BUSY clears
        m_phase = Phase::Idle;
        m_pos = 0;
        return;
    }
    if (got < 512) std::memset(m_buf + got, 0, (size_t)(512 - got));
    m_phase = Phase::Stream;
    m_pos = 0;
    m_len = 512;
}

// ---------------------------------------------------------------------------
// Images
// ---------------------------------------------------------------------------
bool CocoSdc::mount(int drive, const std::string& hostPath, bool raw) {
    if (drive < 0 || drive > 1) return false;
    return mountFile(drive, hostPath, raw) == 0;
}

uint8_t CocoSdc::mountFile(int drive, const std::string& host, bool raw) {
    std::error_code ec;
    if (!fs::is_regular_file(host, ec)) return ST_NOT_FOUND;
    if (m_images[drive ^ 1].path == host) return ST_IN_USE;
    eject(drive);
    Image im;
    im.path = host;
    im.readOnly = !writable(host);
    const uint64_t size = fs::file_size(host, ec);
    std::vector<uint8_t> head(4, 0);
    {
        std::ifstream f(host, std::ios::binary);
        f.read((char*)head.data(), 4);
    }
    auto openFile = [&]() {
        im.file = std::make_unique<std::fstream>(host, std::ios::binary | std::ios::in | (im.readOnly ? std::ios::openmode() : std::ios::out));
        return (bool)*im.file;
    };
    if (raw) {
        im.kind = Kind::Raw;
        if (!openFile()) return ST_HARDWARE;
    } else if (std::memcmp(head.data(), "SDF", 3) == 0) {
        std::vector<uint8_t> bytes;
        if (!readAll(host, bytes)) return ST_HARDWARE;
        auto disk = sdfLoad(bytes);
        if (!disk) return ST_HARDWARE;
        im.readOnly = im.readOnly || disk->writeProtected();
        if (im.readOnly) disk->setWriteProtected(true);
        if (!insertFloppy || !insertFloppy(drive, std::move(disk))) return ST_HARDWARE;
        im.kind = Kind::Sdf;
    } else if (size % 256 == 0 && size / 256 > FLOPPY_MAX) {
        // A hard disk: LBA, and its first 1440 sectors as 80 single-sided tracks for the FDC
        im.kind = Kind::HardDisk;
        if (!openFile()) return ST_HARDWARE;
        std::vector<uint8_t> view(2 + (size_t)HD_FLOPPY_VIEW * 256);
        view[0] = SPT;
        view[1] = 1;
        im.file->read((char*)view.data() + 2, (std::streamsize)HD_FLOPPY_VIEW * 256);
        std::string err;
        auto disk = FloppyDisk::fromBytes(view, FloppyFormat::JVC, &err, "view.dsk");
        if (disk) {
            disk->setWriteProtected(true);   // a copy: the FDC can't write it back (mmsim's limit)
            if (insertFloppy) insertFloppy(drive, std::move(disk));
        }
    } else if (size % 256 == 0) {
        // A plain DSK: the geometry by its size (guide chapter 6), saved back as plain sectors
        if (size / 256 < DSK_MIN) return ST_HARDWARE;
        std::vector<uint8_t> bytes;
        if (!readAll(host, bytes)) return ST_HARDWARE;
        bytes.insert(bytes.begin(), {(uint8_t)SPT, (uint8_t)(size / 256 <= 720 ? 1 : 2)});
        std::string err;
        auto disk = FloppyDisk::fromBytes(bytes, FloppyFormat::JVC, &err, "image.dsk");
        if (!disk) return ST_HARDWARE;
        if (im.readOnly) disk->setWriteProtected(true);
        if (!insertFloppy || !insertFloppy(drive, std::move(disk))) return ST_HARDWARE;
        im.kind = Kind::Floppy;
        im.plainDsk = true;
    } else {
        std::string err;
        auto disk = FloppyDisk::load(host, &err);
        if (!disk) return ST_HARDWARE;
        if (im.readOnly) disk->setWriteProtected(true);
        im.readOnly = im.readOnly || disk->writeProtected();
        if (!insertFloppy || !insertFloppy(drive, std::move(disk))) return ST_HARDWARE;
        im.kind = Kind::Floppy;
    }
    m_images[drive] = std::move(im);
    return 0;
}

void CocoSdc::eject(int drive) {
    if (drive < 0 || drive > 1) return;
    Image& im = m_images[drive];
    if (im.kind == Kind::None) return;
    std::unique_ptr<FloppyDisk> d = (im.kind != Kind::Raw && removeFloppy) ? removeFloppy(drive) : nullptr;
    if (d && d->dirty() && !im.readOnly) {
        if (im.kind == Kind::Sdf) {
            writeFile(im.path, sdfSave(*d));
        } else if (im.plainDsk) {
            std::vector<uint8_t> bytes = d->toBytes(FloppyFormat::JVC);
            bytes.erase(bytes.begin(), bytes.begin() + (long)(bytes.size() % 256));   // no header
            writeFile(im.path, bytes);
        }
    }
    if (im.file) im.file->flush();
    im = Image();
}

std::string CocoSdc::imagePath(int drive) const { return drive >= 0 && drive < 2 ? m_images[drive].path : ""; }

bool CocoSdc::isHardDisk(int drive) const {
    return drive >= 0 && drive < 2 && (m_images[drive].kind == Kind::HardDisk || m_images[drive].kind == Kind::Raw);
}

uint32_t CocoSdc::imageSectors(int drive) const {
    return drive >= 0 && drive < 2 && m_images[drive].kind != Kind::None ? (uint32_t)(fileSize(drive) / 256) : 0;
}

uint8_t CocoSdc::mountPath(int drive, const std::string& path, bool raw) {
    if (path.empty()) { eject(drive); return 0; }
    // The directory part, then the file (wildcards allowed; no extension: name.*)
    const auto slash = path.find_last_of('/');
    std::string dir = m_root, leaf = path;
    if (m_root.empty()) return ST_NOT_FOUND;
    if (slash != std::string::npos) {
        leaf = path.substr(slash + 1);
        const std::string d = path.substr(0, slash);
        if (d.empty()) dir = m_root;
        else if (const uint8_t r = resolve(d, dir, true)) return r;
    } else {
        for (const auto& q : m_cwd) dir += "/" + q;
    }
    std::string host;
    if (leaf.empty()) {
        host = dir;
    } else if (leaf == "..") {
        if (const uint8_t r = resolve(path, host, true)) return r;
    } else {
        std::string b, e;
        if (!split83(leaf, b, e, true)) return ST_INVALID;
        std::string found = wild(leaf) ? findWild(dir, leaf) : findCase(dir, leaf);
        if (found.empty() && leaf.find('.') == std::string::npos) found = findWild(dir, leaf + ".*");
        if (found.empty()) return ST_NOT_FOUND;
        host = dir + "/" + found;
    }
    std::error_code ec;
    if (fs::is_directory(host, ec)) {
        // A disk set: the lowest-numbered disk
        m_setDir = host;
        m_setDrive = drive;
        m_setNumber = 0;
        return mountSet(0, true);
    }
    return mountFile(drive, host, raw);
}

std::vector<std::string> CocoSdc::setMembers() const {
    std::vector<std::string> byDigit(10);
    std::error_code ec;
    if (m_setDir.empty()) return byDigit;
    std::vector<std::string> names;
    for (const auto& e : fs::directory_iterator(m_setDir, ec))
        if (e.is_regular_file(ec)) names.push_back(e.path().filename().string());
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) { return upper(a) < upper(b); });
    for (const auto& n : names) {
        std::string b, x;
        if (!split83(n, b, x) || b.empty()) continue;
        const char last = b.back();
        if (last >= '1' && last <= '9' && byDigit[(size_t)(last - '0')].empty()) byDigit[(size_t)(last - '0')] = m_setDir + "/" + n;
    }
    return byDigit;
}

uint8_t CocoSdc::mountSet(int number, bool next) {
    const std::vector<std::string> m = setMembers();
    int pick = -1;
    if (next) {
        for (int i = m_setNumber + 1; i <= 9 && pick < 0; ++i) if (!m[(size_t)i].empty()) pick = i;
        if (pick < 0 && !m[1].empty()) pick = 1;
    } else if (number >= 1 && number <= 9 && !m[(size_t)number].empty()) {
        pick = number;
    }
    if (pick < 0) return ST_NOT_FOUND;
    const uint8_t r = mountFile(m_setDrive, m[(size_t)pick], false);
    if (!r) m_setNumber = pick;
    return r;
}

uint8_t CocoSdc::newImage(int drive, const std::string& path, bool raw) {
    std::string host;
    if (const uint8_t r = resolve(path, host, false)) return r;
    std::error_code ec;
    if (fs::is_directory(host, ec)) return ST_INVALID;
    if (!fs::exists(host, ec)) {
        bool ok;
        if (raw) {
            ok = writeFile(host, {});
        } else if (m_param[1] == 0 && m_param[2] == 0) {
            ok = writeFile(host, std::vector<uint8_t>((size_t)NEW_DSK_SECTORS * 256, 0xFF));
        } else {
            const int cyls = m_param[1], sides = m_param[2];
            if (cyls < 1 || cyls > 80 || sides < 1 || sides > 2) return ST_INVALID;
            ok = writeFile(host, sdfSave(FloppyDisk(cyls, sides, SDF_TRACK_DATA)));
        }
        if (!ok) return ST_HARDWARE;
    }
    return mountFile(drive, host, raw);
}

// ---------------------------------------------------------------------------
// The card's folder
// ---------------------------------------------------------------------------
std::string CocoSdc::findCase(const std::string& dir, const std::string& comp) const {
    std::error_code ec;
    if (fs::exists(fs::path(dir) / comp, ec)) return comp;
    const std::string want = upper(comp);
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (upper(e.path().filename().string()) == want) return e.path().filename().string();
    return "";
}

std::string CocoSdc::findWild(const std::string& dir, const std::string& pattern) const {
    std::error_code ec;
    std::vector<std::string> names;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string n = e.path().filename().string();
        std::string b, x;
        if (!n.empty() && n[0] != '.' && split83(n, b, x) && patternMatch(pattern, n)) names.push_back(n);
    }
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) { return upper(a) < upper(b); });
    return names.empty() ? "" : names.front();
}

uint8_t CocoSdc::resolve(const std::string& name, std::string& host, bool mustExist) const {
    if (m_root.empty()) return ST_NOT_FOUND;
    if (name.empty()) return ST_INVALID;
    std::vector<std::string> parts = name[0] == '/' ? std::vector<std::string>() : m_cwd;
    std::vector<std::string> comps;
    std::string comp;
    for (char c : name + "/") {
        if (c == '/') {
            if (!comp.empty()) comps.push_back(comp);
            comp.clear();
        } else {
            comp += c;
        }
    }
    for (size_t i = 0; i < comps.size(); ++i) {
        const std::string& p = comps[i];
        if (p == ".") continue;
        if (p == "..") {
            if (parts.empty()) return ST_NOT_FOUND;
            parts.pop_back();
            continue;
        }
        std::string b, e;
        if (!split83(p, b, e)) return ST_INVALID;
        std::string dir = m_root;
        for (const auto& q : parts) dir += "/" + q;
        const std::string found = findCase(dir, p);
        if (found.empty()) {
            if (i + 1 < comps.size() || mustExist) return ST_NOT_FOUND;
            parts.push_back(upper(p));
        } else {
            parts.push_back(found);
        }
    }
    host = m_root;
    for (const auto& q : parts) host += "/" + q;
    return 0;
}

uint8_t CocoSdc::changeDir(const std::string& path) {
    if (m_root.empty()) return ST_NOT_FOUND;
    std::string host;
    if (path == "/") {
        host = m_root;
    } else if (const uint8_t r = resolve(path, host, true)) {
        return r;
    }
    std::error_code ec;
    if (!fs::is_directory(host, ec)) return ST_NOT_FOUND;
    std::vector<std::string> parts;
    const std::string rel = host.size() > m_root.size() ? host.substr(m_root.size() + 1) : "";
    std::string comp;
    for (char c : rel + "/") {
        if (c == '/') { if (!comp.empty()) parts.push_back(comp); comp.clear(); }
        else comp += c;
    }
    m_cwd = parts;
    return 0;
}

uint8_t CocoSdc::startListing(const std::string& spec) {
    m_listing = false;
    m_list.clear();
    m_listPos = 0;
    if (m_root.empty()) return ST_NOT_FOUND;
    std::string pattern = spec, dir = m_root;
    for (const auto& q : m_cwd) dir += "/" + q;
    const auto slash = spec.find_last_of('/');
    if (slash != std::string::npos) {
        pattern = spec.substr(slash + 1);
        const std::string d = spec.substr(0, slash);
        if (d.empty()) dir = m_root;
        else if (const uint8_t r = resolve(d, dir, true)) return r;
    }
    if (pattern.empty()) pattern = "*.*";
    std::string b, x;
    if (!split83(pattern, b, x, true)) return ST_INVALID;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return ST_NOT_FOUND;
    std::vector<std::string> items;
    if (dir != m_root) {                        // a sub-directory's "." and ".." (as FAT has them)
        if (patternMatch(pattern, ".")) items.push_back(dir + "/.");
        if (patternMatch(pattern, "..")) items.push_back(dir + "/..");
    }
    std::vector<std::string> found;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string n = e.path().filename().string();
        if (n.empty() || n[0] == '.' || !split83(n, b, x) || !patternMatch(pattern, n)) continue;
        found.push_back(e.path().string());
    }
    std::sort(found.begin(), found.end(), [](const std::string& a, const std::string& c) {
        return upper(fs::path(a).filename().string()) < upper(fs::path(c).filename().string());
    });
    items.insert(items.end(), found.begin(), found.end());
    m_list = items;
    m_listing = true;
    return 0;
}

namespace {
void nameFields(const std::string& hostPath, uint8_t* e) {
    std::memset(e, ' ', 11);
    std::string name = fs::path(hostPath).filename().string();
    std::string b, x;
    if (!split83(name, b, x)) { b = name.substr(0, 8); x.clear(); }
    b = upper(b);
    x = upper(x);
    std::memcpy(e, b.data(), std::min<size_t>(b.size(), 8));
    std::memcpy(e + 8, x.data(), std::min<size_t>(x.size(), 3));
}

void attrsAndSize(const std::string& hostPath, uint8_t& attr, uint32_t& size) {
    std::error_code ec;
    const std::string leaf = fs::path(hostPath).filename().string();
    const bool isDir = leaf == "." || leaf == ".." || fs::is_directory(hostPath, ec);
    attr = (uint8_t)((isDir ? 0x10 : 0) | (writable(hostPath) ? 0 : 0x01));
    const uint64_t n = isDir ? 0 : fs::file_size(hostPath, ec);
    size = ec ? 0 : (uint32_t)std::min<uint64_t>(n, 0xFFFFFFFFu);
}
}  // namespace

void CocoSdc::record32(const std::string& hostPath, uint8_t* e, bool sdf) const {
    std::memset(e, 0, 32);
    nameFields(hostPath, e);
    uint8_t attr;
    uint32_t size;
    attrsAndSize(hostPath, attr, size);
    e[11] = (uint8_t)(attr | (sdf ? 0x04 : 0));
    e[28] = (uint8_t)size;
    e[29] = (uint8_t)(size >> 8);
    e[30] = (uint8_t)(size >> 16);
    e[31] = (uint8_t)(size >> 24);
}

void CocoSdc::record16(const std::string& hostPath, uint8_t* e) const {
    std::memset(e, 0, 16);
    nameFields(hostPath, e);
    uint8_t attr;
    uint32_t size;
    attrsAndSize(hostPath, attr, size);
    e[11] = attr;
    e[12] = (uint8_t)(size >> 24);
    e[13] = (uint8_t)(size >> 16);
    e[14] = (uint8_t)(size >> 8);
    e[15] = (uint8_t)size;
}
