// Tandy Portable Disk Drive (TPDD / TPDD2) protocol, after the Bitchin100
// wiki, pdd.sh (also the TPDD1's on-disk file system) and VirtualT's
// tpddserver.cpp. mmsim's own code.

#include "tpdd.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace {

std::string upper(std::string s) {
    for (char& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}

std::string trimRight(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
    return s;
}

std::string pad24(std::string s) {
    if (s.size() > 24) s.resize(24);
    s.append(24 - s.size(), ' ');
    return s;
}

// The 24-byte name field of a request, as a string (trailing blanks kept)
std::string fieldName(const std::vector<uint8_t>& d) {
    std::string s;
    for (size_t i = 0; i < 24 && i < d.size(); ++i) s.push_back((char)d[i]);
    return s;
}

const int LSC_SIZE[7] = {64, 80, 128, 256, 512, 1024, 1280};

// The TPDD1 file system (pdd.sh): 40 FCBs of 31 bytes in sector 0, then the SMT
constexpr int FCB_COUNT = 40, FCB_SIZE = 31, SMT_OFFSET = FCB_COUNT * FCB_SIZE, SMT_FLAGS = 20;
constexpr uint32_t MAX_FILE = 65534;       // pdd.sh PDD1_MAX_FLEN

}  // namespace

Tpdd::Tpdd(Model m) : m_model(m) {
    m_sectors.resize(m == Model::Tpdd1 ? 80 : 160);
}

Tpdd::~Tpdd() { closeFile(); }

uint8_t Tpdd::checksum(const uint8_t* p, size_t n) {
    unsigned sum = 0;
    for (size_t i = 0; i < n; ++i) sum += p[i];
    return (uint8_t)(~sum & 0xFF);
}

std::vector<uint8_t> Tpdd::request(uint8_t type, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> v = {0x5A, 0x5A, type, (uint8_t)data.size()};
    v.insert(v.end(), data.begin(), data.end());
    v.push_back(checksum(v.data() + 2, v.size() - 2));
    return v;
}

std::string Tpdd::tpddName(const std::string& hostName) {
    const size_t dot = hostName.rfind('.');
    const std::string base = dot == std::string::npos ? hostName : hostName.substr(0, dot);
    const std::string ext = dot == std::string::npos ? "" : hostName.substr(dot + 1);
    if (!base.empty() && base.size() <= 6 && ext.size() <= 2 && base.find('.') == std::string::npos) {
        std::string s = upper(base);
        s.append(6 - s.size(), ' ');
        if (!ext.empty()) s += "." + upper(ext);
        return pad24(s);
    }
    return pad24(hostName);
}

void Tpdd::registerSchemes() {
    auto make = [](Model m) {
        return [m](const std::string& arg) -> std::unique_ptr<SerialEndpoint> {
            auto t = std::make_unique<Tpdd>(m);
            if (arg.empty()) return t;
            std::error_code ec;
            if (fs::is_directory(arg, ec)) {
                if (!t->setRootDirectory(arg)) return nullptr;
            } else if (!t->loadImage(arg)) {
                return nullptr;
            }
            return t;
        };
    };
    registerSerialScheme("tpdd", make(Model::Tpdd1));
    registerSerialScheme("tpdd2", make(Model::Tpdd2));
}

std::string Tpdd::describe() const {
    std::string s = m_model == Model::Tpdd1 ? "tpdd:" : "tpdd2:";
    s += !m_root.empty() ? m_root : m_imagePath;
    if (!m_cwd.empty()) s += " [" + m_cwd + "]";
    if (m_fdc) s += " (FDC mode)";
    return s;
}

bool Tpdd::setRootDirectory(const std::string& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return false;
    m_root = dir;
    m_cwd.clear();
    return true;
}

// --- sector images --------------------------------------------------------

bool Tpdd::loadImage(const std::string& path) {
    const size_t n = m_sectors.size();
    std::vector<Sector> secs(n);
    FILE* f = std::fopen(path.c_str(), "rb");
    bool raw = false;
    if (f) {
        std::fseek(f, 0, SEEK_END);
        const long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (size == (long)(n * SECTOR_SIZE)) raw = true;
        else if (size != (long)(n * (1 + ID_SIZE + SECTOR_SIZE))) {
            std::fclose(f);
            return false;
        }
        for (Sector& s : secs) {
            bool ok = true;
            if (!raw) {
                ok = std::fread(&s.lsc, 1, 1, f) == 1 && std::fread(s.id.data(), 1, ID_SIZE, f) == (size_t)ID_SIZE;
                if (s.lsc > 6) s.lsc = 0;
            }
            ok = ok && std::fread(s.data.data(), 1, SECTOR_SIZE, f) == (size_t)SECTOR_SIZE;
            if (!ok) {
                std::fclose(f);
                return false;
            }
        }
        std::fclose(f);
    } else {
        // A new image: only with the .pdd1 / .pdd2 extension, created on the first write
        const std::string ext = upper(fs::path(path).extension().string());
        if (ext != ".PDD1" && ext != ".PDD2") return false;
    }
    m_sectors = std::move(secs);
    m_imagePath = path;
    m_imageRaw = raw;
    return true;
}

bool Tpdd::saveImage(const std::string& path, bool raw) const {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    for (const Sector& s : m_sectors) {
        if (!raw) {
            std::fwrite(&s.lsc, 1, 1, f);
            std::fwrite(s.id.data(), 1, ID_SIZE, f);
        }
        std::fwrite(s.data.data(), 1, SECTOR_SIZE, f);
    }
    return std::fclose(f) == 0;
}

void Tpdd::persistImage() {
    if (!m_imagePath.empty()) saveImage(m_imagePath, m_imageRaw);
}

int Tpdd::logicalSize(int sector) const {
    return LSC_SIZE[m_sectors[sector].lsc % 7];
}

// --- the serial line ------------------------------------------------------

bool Tpdd::receive(uint8_t& b) {
    if (m_out.empty()) return false;
    b = m_out.front();
    m_out.pop_front();
    return true;
}

std::vector<uint8_t> Tpdd::takeOutput() {
    std::vector<uint8_t> v(m_out.begin(), m_out.end());
    m_out.clear();
    return v;
}

void Tpdd::send(uint8_t b) {
    if (m_fdc) fdcByte(b);
    else opByte(b);
}

void Tpdd::reply(uint8_t type, const std::vector<uint8_t>& d) {
    std::vector<uint8_t> v = {type, (uint8_t)d.size()};
    v.insert(v.end(), d.begin(), d.end());
    v.push_back(checksum(v.data(), v.size()));
    m_out.insert(m_out.end(), v.begin(), v.end());
}

void Tpdd::opByte(uint8_t b) {
    switch (m_rx) {
    case RxState::Idle:
        // After a DME answer TS-DOS sends CRs; the second ends the DME
        // session, anything else ends it at once (VirtualT tpddserver.cpp)
        if (m_dmeCrs) {
            if (b == '\r') {
                if (++m_dmeCrs > 2) m_dmeCrs = 0;
                m_dmePending = false;
                break;
            }
            m_dmeCrs = 0;
            m_dmePending = false;
        }
        if (b == 'Z') m_rx = RxState::Z;
        else if (b == 'M') m_rx = RxState::M;
        break;
    case RxState::Z:
        if (b == 'Z') m_rx = RxState::Type;
        else {
            m_rx = RxState::Idle;
            opByte(b);
        }
        break;
    case RxState::Type:
        m_type = b;
        m_rx = RxState::Len;
        break;
    case RxState::Len:
        m_len = b;
        m_data.clear();
        m_rx = b ? RxState::Data : RxState::Sum;
        break;
    case RxState::Data:
        m_data.push_back(b);
        if (m_data.size() == m_len) m_rx = RxState::Sum;
        break;
    case RxState::Sum: {
        m_rx = RxState::Idle;
        std::vector<uint8_t> all = {m_type, m_len};
        all.insert(all.end(), m_data.begin(), m_data.end());
        if (checksum(all.data(), all.size()) != b) {
            normal(0x41);
            m_dmePending = false;
            break;
        }
        execute(m_type, m_data);
        break;
    }
    case RxState::M:
        // "M1\r": TS-DOS's way back from FDC mode, and its DME probe
        m_rx = b == '1' ? RxState::M1 : RxState::Idle;
        if (b == 'Z') {
            m_rx = RxState::Idle;
            opByte(b);
        }
        break;
    case RxState::M1:
        if (b == '\r') m_dmePending = true;
        m_rx = RxState::Idle;
        break;
    }
}

// --- operation mode -------------------------------------------------------

std::string Tpdd::dirPath() const {
    return m_cwd.empty() ? m_root : (fs::path(m_root) / m_cwd).string();
}

std::vector<Tpdd::Entry> Tpdd::listDir() const {
    if (imageFs()) return imageListing();
    std::vector<Entry> v;
    std::error_code ec;
    if (m_root.empty()) return v;
    for (const auto& de : fs::directory_iterator(dirPath(), ec)) {
        const std::string host = de.path().filename().string();
        if (host.empty() || host[0] == '.') continue;
        Entry e;
        e.host = host;
        e.dir = de.is_directory(ec);
        if (e.dir) {
            std::string b = upper(host.substr(0, 6));
            b.append(6 - b.size(), ' ');
            e.name = pad24(b + ".<>");
        } else {
            e.name = tpddName(host);
            const auto sz = de.file_size(ec);
            e.size = ec ? 0 : (uint32_t)std::min<uintmax_t>(sz, 65535);
        }
        v.push_back(e);
    }
    std::sort(v.begin(), v.end(), [](const Entry& a, const Entry& b) { return a.name < b.name; });
    if (!m_cwd.empty()) {
        Entry p;
        p.host = "..";
        p.name = pad24("PARENT.<>");
        p.dir = true;
        v.insert(v.begin(), p);
    }
    return v;
}

bool Tpdd::findEntry(const std::string& name, Entry& out) const {
    const std::string want = upper(trimRight(name));
    for (const Entry& e : listDir()) {
        // Compare the TPDD forms, and the plain host name for TS-DOS's "NAME.BA"
        if (upper(trimRight(e.name)) == want || upper(e.host) == want) {
            out = e;
            return true;
        }
    }
    return false;
}

std::string Tpdd::hostNameFor(const std::string& name, bool& isDir) {
    std::string s = trimRight(name);
    isDir = s.size() > 3 && s.compare(s.size() - 3, 3, ".<>") == 0;
    if (isDir) s.resize(s.size() - 3);
    // "NAME  .BA" -> "NAME.BA": drop the padding before the dot
    const size_t dot = s.find('.');
    if (dot != std::string::npos) {
        std::string base = trimRight(s.substr(0, dot));
        return base + s.substr(dot);
    }
    return trimRight(s);
}

uint8_t Tpdd::freeSectors() const {
    if (imageFs()) {
        int n = 0;
        for (int p = firstDataSector(); p < (int)m_sectors.size(); ++p) n += sectorUsed(p) ? 0 : 1;
        return (uint8_t)n;
    }
    const int total = m_model == Model::Tpdd1 ? 80 : 160;
    int used = 0;
    for (const Entry& e : listDir())
        if (!e.dir) used += (int)((e.size + SECTOR_SIZE - 1) / SECTOR_SIZE);
    return (uint8_t)std::max(0, total - used);
}

void Tpdd::replyEntry(const Entry* e) {
    std::vector<uint8_t> d;
    if (e) {
        for (char c : e->name) d.push_back((uint8_t)c);
        d.push_back(e->attr);
        d.push_back((uint8_t)(e->size >> 8));
        d.push_back((uint8_t)e->size);
    } else {
        d.assign(24, 0);
        d.push_back(0);
        d.push_back(0);
        d.push_back(0);
    }
    d.push_back(freeSectors());
    reply(0x11, d);
}

void Tpdd::closeFile() {
    if (m_file) std::fclose(m_file);
    m_file = nullptr;
    m_openMode = 0;
    m_imgFcb = -1;
    m_imgFile.clear();
    m_imgPos = 0;
}

void Tpdd::execute(uint8_t type, const std::vector<uint8_t>& d) {
    // "M1\r" holds until the DME session ends: TS-DOS asks for the drive's
    // status between "M1\r" and its $08 (VirtualT's m_tsdosDmeStart)
    const bool dme = m_dmePending;
    m_bank = 0;
    if (m_model == Model::Tpdd2 && (type & 0xC0) == 0x40 && (type & 0x3F) < 0x10) {   // bank 1
        type &= 0x3F;
        m_bank = 1;
    }
    if (m_root.empty() && !imageFs() && type <= 0x0D && type != 0x06 && type != 0x07 && type != 0x08 && type != 0x0C) {
        normal(0x70);           // no directory: nothing to file-access
        return;
    }
    if (!m_diskPresent && type <= 0x0D && type != 0x08 && type != 0x0C) {
        normal(0x70);
        return;
    }
    switch (type) {
    case 0x00: opDirectory(d); break;
    case 0x01: opOpen(d); break;
    case 0x02:
        closeFile();
        normal(0x00);
        break;
    case 0x03: opRead(); break;
    case 0x04: opWrite(d); break;
    case 0x05: opDelete(); break;
    case 0x06:
        // Format: wipes a sector image; a host directory is left alone
        if (m_writeProtect) {
            normal(0x50);
            break;
        }
        closeFile();
        if (imageFs()) {
            formatImageFs();
        } else if (m_root.empty()) {
            for (Sector& s : m_sectors) s = Sector{};
            persistImage();
        }
        normal(0x00);
        break;
    case 0x07: normal(0x00); break;
    case 0x08:
        if (dme) {
            // TS-DOS directory-management extension: name the current directory
            std::string name = m_cwd.empty() ? "ROOT" : upper(fs::path(m_cwd).filename().string());
            name = name.substr(0, 6);
            name.append(6 - name.size(), ' ');
            name += ".<> ";
            std::vector<uint8_t> v = {0x00};
            for (char c : name) v.push_back((uint8_t)c);
            reply(0x12, v);
            m_dmeCrs = 1;
        } else if (m_model == Model::Tpdd1) {
            m_fdc = true;
            m_fdcWait = FdcWait::Line;
            m_line.clear();
        } else {
            normal(0x36);
        }
        break;
    case 0x0C:
        reply(0x15, {(uint8_t)((m_writeProtect ? 0x02 : 0) | (m_diskPresent ? 0 : 0x04))});
        break;
    case 0x0D: opRename(d); break;
    case 0x23:
        if (m_model == Model::Tpdd2)
            reply(0x14, {0x41, 0x10, 0x01, 0x00, 0x50, 0x05, 0x00, 0x02, 0x00, 0x28, 0x00, 0xE1, 0x00, 0x00, 0x00});
        break;
    case 0x30:
        if (m_model == Model::Tpdd2) opCache(d);
        else normal(0x36);
        break;
    case 0x31:
        if (m_model == Model::Tpdd2) opMemWrite(d);
        else normal(0x36);
        break;
    case 0x32:
        if (m_model == Model::Tpdd2) opMemRead(d);
        else normal(0x36);
        break;
    case 0x33:
        if (m_model == Model::Tpdd2) reply(0x3A, {0x80, 0x13, 0x05, 0x00, 0x10, 0xE1});
        break;
    default: normal(0x36); break;
    }
}

void Tpdd::opDirectory(const std::vector<uint8_t>& d) {
    if (d.size() != 26) {
        normal(0x36);
        return;
    }
    switch (d[25]) {
    case 0: {
        const std::string name = fieldName(d);
        if (trimRight(name).empty()) {
            m_haveRef = false;
            replyEntry(nullptr);
            return;
        }
        Entry e;
        if (findEntry(name, e)) {
            m_ref = e;
            replyEntry(&e);
        } else {
            m_ref = Entry{};
            m_ref.host = hostNameFor(name, m_ref.dir);
            m_ref.name = pad24(trimRight(name));
            m_ref.exists = false;
            replyEntry(nullptr);
        }
        m_haveRef = true;
        return;
    }
    case 1:
        m_listing = listDir();
        m_listPos = 0;
        break;
    case 2:
        if (m_listPos < (int)m_listing.size()) ++m_listPos;
        break;
    case 3:
        if (m_model != Model::Tpdd2) {
            normal(0x36);
            return;
        }
        if (m_listPos > 0) --m_listPos;
        break;
    case 4:
        if (m_model != Model::Tpdd2) {
            normal(0x36);
            return;
        }
        m_listing.clear();
        m_listPos = -1;
        normal(0x00);
        return;
    default: normal(0x36); return;
    }
    if (m_listPos >= 0 && m_listPos < (int)m_listing.size()) replyEntry(&m_listing[m_listPos]);
    else replyEntry(nullptr);
}

void Tpdd::opOpen(const std::vector<uint8_t>& d) {
    if (d.size() != 1 || d[0] < 1 || d[0] > 3) {
        normal(0x36);
        return;
    }
    if (!m_haveRef) {
        normal(0x30);
        return;
    }
    const int mode = d[0];
    closeFile();
    if (imageFs()) {
        opOpenImage(mode);
        return;
    }
    std::error_code ec;
    if (m_ref.dir) {
        m_lastOpenDir = true;
        if (mode == 3) {
            if (m_ref.host == "..") m_cwd = fs::path(m_cwd).parent_path().string();
            else if (m_ref.exists) m_cwd = m_cwd.empty() ? m_ref.host : (fs::path(m_cwd) / m_ref.host).string();
            else {
                normal(0x10);
                return;
            }
            if (m_cwd == ".") m_cwd.clear();
            normal(0x00);
        } else {
            if (m_writeProtect) {
                normal(0x50);
                return;
            }
            fs::create_directory(fs::path(dirPath()) / upper(m_ref.host), ec);
            normal(ec ? 0x60 : 0x00);
        }
        return;
    }
    m_lastOpenDir = false;
    const std::string path = (fs::path(dirPath()) / m_ref.host).string();
    if (mode != 3 && m_writeProtect) {
        normal(0x50);
        return;
    }
    if (mode == 3 && !m_ref.exists) {
        normal(0x10);
        return;
    }
    m_file = std::fopen(path.c_str(), mode == 1 ? "wb" : mode == 2 ? "ab" : "rb");
    if (!m_file) {
        normal(mode == 3 ? 0x10 : 0x60);
        return;
    }
    m_openMode = mode;
    m_ref.exists = true;
    normal(0x00);
}

void Tpdd::opRead() {
    if (m_imgFcb >= 0 && m_openMode == 3) {
        const size_t n = std::min<size_t>(128, m_imgFile.size() - m_imgPos);
        if (n == 0) {
            normal(0x3F);
            return;
        }
        reply(0x10, std::vector<uint8_t>(m_imgFile.begin() + (long)m_imgPos, m_imgFile.begin() + (long)(m_imgPos + n)));
        m_imgPos += n;
        return;
    }
    if (!m_file || m_openMode != 3) {
        normal(0x37);
        return;
    }
    std::vector<uint8_t> buf(128);
    const size_t n = std::fread(buf.data(), 1, buf.size(), m_file);
    if (n == 0) {
        normal(0x3F);
        return;
    }
    buf.resize(n);
    reply(0x10, buf);
}

void Tpdd::opWrite(const std::vector<uint8_t>& d) {
    if (m_imgFcb >= 0 && m_openMode != 3) {
        if (d.empty() || d.size() > 128) {
            normal(0x36);
            return;
        }
        if (m_imgFile.size() + d.size() > MAX_FILE) {
            normal(0x6E);
            return;
        }
        std::vector<uint8_t> next = m_imgFile;
        next.insert(next.end(), d.begin(), d.end());
        if (!writeImageFile(m_imgFcb, next)) {
            normal(0x61);
            return;
        }
        m_imgFile = std::move(next);
        normal(0x00);
        return;
    }
    if (!m_file || m_openMode == 3) {
        normal(0x37);
        return;
    }
    if (d.empty() || d.size() > 128) {
        normal(0x36);
        return;
    }
    std::fseek(m_file, 0, SEEK_END);
    if (std::ftell(m_file) + (long)d.size() > 65535) {
        normal(0x6E);
        return;
    }
    if (std::fwrite(d.data(), 1, d.size(), m_file) != d.size()) {
        normal(0x61);
        return;
    }
    std::fflush(m_file);
    normal(0x00);
}

void Tpdd::opDelete() {
    if (!m_haveRef) {
        normal(0x30);
        return;
    }
    if (m_writeProtect) {
        normal(0x50);
        return;
    }
    if (m_ref.dir && m_ref.host == "..") {
        normal(0x00);
        return;
    }
    closeFile();
    if (imageFs()) {
        if (!m_ref.exists || m_ref.fcb < 0) {
            m_haveRef = false;
            normal(0x10);
            return;
        }
        freeChain(m_ref.fcb);
        std::fill(m_sectors[dirSector()].data.begin() + m_ref.fcb * FCB_SIZE,
                  m_sectors[dirSector()].data.begin() + (m_ref.fcb + 1) * FCB_SIZE, 0);
        persistImage();
        m_haveRef = false;
        normal(0x00);
        return;
    }
    std::error_code ec;
    const fs::path p = fs::path(dirPath()) / m_ref.host;
    const bool removed = m_ref.exists && fs::remove(p, ec);
    m_haveRef = false;
    normal(removed ? 0x00 : 0x10);
}

void Tpdd::opRename(const std::vector<uint8_t>& d) {
    if (d.size() != 25) {
        normal(0x36);
        return;
    }
    if (!m_haveRef || !m_ref.exists) {
        normal(m_haveRef ? 0x10 : 0x30);
        return;
    }
    if (m_writeProtect) {
        normal(0x50);
        return;
    }
    bool dir = false;
    const std::string to = hostNameFor(fieldName(d), dir);
    if (to.empty()) {
        normal(0x30);
        return;
    }
    Entry existing;
    if (findEntry(fieldName(d), existing)) {
        normal(0x11);
        return;
    }
    closeFile();
    if (imageFs()) {
        const std::string to24 = pad24(trimRight(fieldName(d)));
        for (int i = 0; i < 24; ++i) m_sectors[dirSector()].data[(size_t)(m_ref.fcb * FCB_SIZE + i)] = (uint8_t)to24[(size_t)i];
        persistImage();
        m_ref.name = m_ref.host = to24;
        normal(0x00);
        return;
    }
    std::error_code ec;
    fs::rename(fs::path(dirPath()) / m_ref.host, fs::path(dirPath()) / to, ec);
    if (ec) {
        normal(0x10);
        return;
    }
    m_ref.host = to;
    m_ref.name = dir ? m_ref.name : tpddName(to);
    normal(0x00);
}

// --- the TPDD1 file system on a disk image (pdd.sh read_fcb / read_smt) ---

std::vector<Tpdd::Entry> Tpdd::imageListing() const {
    std::vector<Entry> v;
    const auto& d = m_sectors[dirSector()].data;
    for (int i = 0; i < FCB_COUNT; ++i) {
        const uint8_t* f = d.data() + i * FCB_SIZE;
        if (f[0] == 0) continue;
        Entry e;
        e.name.assign((const char*)f, 24);
        e.host = e.name;
        e.attr = f[24];
        e.size = (uint32_t)(f[25] << 8 | f[26]);
        e.fcb = i;
        v.push_back(e);
    }
    return v;
}

// The SMT's flag for sector p: TPDD1 bit $80 >> 2 (p % 4) of byte p / 4 (the
// odd bits reserved); TPDD2 bit $80 >> (p % 8) of byte p / 8, p = track * 2
// + sector (pdd.sh read_smt). The TPDD2 keeps the same SMT in sectors 0 and 1.
bool Tpdd::sectorUsed(int p) const {
    if (m_model == Model::Tpdd2) return m_sectors[0].data[SMT_OFFSET + p / 8] & (0x80 >> (p % 8));
    return m_sectors[0].data[SMT_OFFSET + p / 4] & (0x80 >> (2 * (p % 4)));
}

void Tpdd::setSectorUsed(int p, bool used) {
    const bool two = m_model == Model::Tpdd2;
    uint8_t& b = m_sectors[0].data[SMT_OFFSET + (two ? p / 8 : p / 4)];
    const uint8_t bit = (uint8_t)(two ? 0x80 >> (p % 8) : 0x80 >> (2 * (p % 4)));
    b = used ? (uint8_t)(b | bit) : (uint8_t)(b & ~bit);
    // The count is of the data sectors in use (a fresh format: 0; Tandy's
    // TPDD2 utility disk: 12 with sectors 0-13 flagged)
    int n = 0;
    for (int s = firstDataSector(); s < (int)m_sectors.size(); ++s) n += sectorUsed(s) ? 1 : 0;
    m_sectors[0].data[SMT_OFFSET + SMT_FLAGS] = (uint8_t)n;
    if (two)
        std::copy(m_sectors[0].data.begin() + SMT_OFFSET, m_sectors[0].data.begin() + SMT_OFFSET + SMT_FLAGS + 1,
                  m_sectors[1].data.begin() + SMT_OFFSET);
}

std::vector<int> Tpdd::fileChain(int fcb) const {
    std::vector<int> chain;
    const uint8_t* f = m_sectors[dirSector()].data.data() + fcb * FCB_SIZE;
    const uint32_t size = (uint32_t)(f[25] << 8 | f[26]);
    if (size == 0) return chain;
    int p = f[29];
    const size_t want = (size + SECTOR_SIZE - 1) / SECTOR_SIZE;
    while (p >= firstDataSector() && p < (int)m_sectors.size() && chain.size() < want) {
        chain.push_back(p);
        const int next = m_sectors[p].id[0];
        if (next == 0xFF) break;
        p = next;
    }
    return chain;
}

std::vector<uint8_t> Tpdd::readImageFile(int fcb) const {
    const uint8_t* f = m_sectors[dirSector()].data.data() + fcb * FCB_SIZE;
    size_t left = (size_t)(f[25] << 8 | f[26]);
    std::vector<uint8_t> v;
    for (int p : fileChain(fcb)) {
        const size_t n = std::min<size_t>(left, SECTOR_SIZE);
        v.insert(v.end(), m_sectors[p].data.begin(), m_sectors[p].data.begin() + n);
        left -= n;
    }
    return v;
}

void Tpdd::freeChain(int fcb) {
    for (int p : fileChain(fcb)) {
        setSectorUsed(p, false);
        m_sectors[p].id[0] = 0;
    }
    uint8_t* f = m_sectors[dirSector()].data.data() + fcb * FCB_SIZE;
    f[25] = f[26] = 0;
    f[29] = f[30] = 0;
}

bool Tpdd::writeImageFile(int fcb, const std::vector<uint8_t>& data) {
    const std::vector<int> old = fileChain(fcb);
    const size_t need = (data.size() + SECTOR_SIZE - 1) / SECTOR_SIZE;
    std::vector<int> secs;
    for (int p = firstDataSector(); p < (int)m_sectors.size() && secs.size() < need; ++p)
        if (!sectorUsed(p) || std::find(old.begin(), old.end(), p) != old.end()) secs.push_back(p);
    if (secs.size() < need) return false;
    freeChain(fcb);
    for (size_t k = 0; k < secs.size(); ++k) {
        Sector& s = m_sectors[secs[k]];
        const size_t off = k * SECTOR_SIZE, n = std::min<size_t>(SECTOR_SIZE, data.size() - off);
        std::fill(s.data.begin(), s.data.end(), 0);
        std::copy(data.begin() + (long)off, data.begin() + (long)(off + n), s.data.begin());
        s.id.fill(0);
        s.id[0] = (uint8_t)(k + 1 < secs.size() ? secs[k + 1] : 0xFF);
        s.lsc = usedLsc();
        setSectorUsed(secs[k], true);
    }
    uint8_t* f = m_sectors[dirSector()].data.data() + fcb * FCB_SIZE;
    f[25] = (uint8_t)(data.size() >> 8);
    f[26] = (uint8_t)data.size();
    f[29] = secs.empty() ? 0 : (uint8_t)secs.front();
    f[30] = secs.empty() ? 0 : (uint8_t)secs.back();
    m_sectors[dirSector()].lsc = usedLsc();
    persistImage();
    return true;
}

// TPDD1: every sector LSC 1 but sector 0 (pdd.sh: "OPR-format writes LSC 1
// to all untouched sectors"), the SMT's flag for sector 0. TPDD2 (dl2's
// req_format): every sector's first byte $16, the directory sectors 0 and
// 1 ending their chain ($FF) and flagged in both SMT copies ($C0).
void Tpdd::formatImageFs() {
    const bool two = m_model == Model::Tpdd2;
    for (Sector& s : m_sectors) {
        s = Sector{};
        s.lsc = two ? 0x16 : 1;
    }
    if (two) {
        for (int d = 0; d < 2; ++d) {
            m_sectors[d].id[0] = 0xFF;
            m_sectors[d].data[SMT_OFFSET] = 0xC0;
        }
    } else {
        m_sectors[0].lsc = 0;
        m_sectors[0].data[SMT_OFFSET] = 0x80;
    }
    persistImage();
}

void Tpdd::opOpenImage(int mode) {
    if (mode != 3 && m_writeProtect) {
        normal(0x50);
        return;
    }
    if (mode == 3 && !m_ref.exists) {
        normal(0x10);
        return;
    }
    int fcb = m_ref.exists ? m_ref.fcb : -1;
    if (fcb < 0) {
        // A new file: the first free FCB
        const auto& d = m_sectors[dirSector()].data;
        for (int i = 0; i < FCB_COUNT && fcb < 0; ++i)
            if (d[i * FCB_SIZE] == 0) fcb = i;
        if (fcb < 0) {
            normal(0x60);
            return;
        }
        uint8_t* f = m_sectors[dirSector()].data.data() + fcb * FCB_SIZE;
        std::fill(f, f + FCB_SIZE, 0);
        for (int i = 0; i < 24; ++i) f[i] = (uint8_t)m_ref.name[(size_t)i];
        f[24] = 'F';
        m_ref.exists = true;
        m_ref.fcb = fcb;
        persistImage();
    }
    m_imgFcb = fcb;
    m_imgPos = 0;
    m_imgFile = mode == 1 ? std::vector<uint8_t>() : readImageFile(fcb);
    if (mode == 1 && !writeImageFile(fcb, m_imgFile)) {     // truncates an existing file
        m_imgFcb = -1;
        normal(0x61);
        return;
    }
    m_openMode = mode;
    normal(0x00);
}

// TPDD2 sector cache: $30 action, track (2), side, sector
void Tpdd::opCache(const std::vector<uint8_t>& d) {
    if (d.size() != 5) {
        reply(0x38, {0x36});
        return;
    }
    const int track = (d[1] << 8) | d[2];
    const int sector = d[4];
    const int i = track * 2 + sector;
    if (track > 79 || sector > 1 || d[3] != 0 || d[0] > 2) {
        reply(0x38, {0x36});
        return;
    }
    if (!m_diskPresent) {
        reply(0x38, {0x70});
        return;
    }
    Sector& s = m_sectors[i];
    if (d[0] == 0) {
        m_cache = s.data;
        std::copy(s.id.begin(), s.id.end(), m_cacheId.begin());
        m_cacheId[ID_SIZE] = s.lsc;
    } else {
        if (m_writeProtect) {
            reply(0x38, {0x50});
            return;
        }
        s.data = m_cache;
        std::copy(m_cacheId.begin(), m_cacheId.begin() + ID_SIZE, s.id.begin());
        persistImage();
    }
    reply(0x38, {0x00});
}

// $31: area (0 cache, 1 drive CPU memory), address (2), data. The CPU area
// models only the ID buffer the backup programs use: $8004-$800F the ID,
// $8010 the size code.
void Tpdd::opMemWrite(const std::vector<uint8_t>& d) {
    if (d.size() < 4 || d[0] > 1) {
        reply(0x38, {0x36});
        return;
    }
    const int addr = (d[1] << 8) | d[2];
    for (size_t k = 3; k < d.size(); ++k) {
        const int a = addr + (int)(k - 3);
        if (d[0] == 0) {
            if (a < SECTOR_SIZE) m_cache[a] = d[k];
        } else if (a >= 0x8004 && a < 0x8004 + ID_SIZE + 1) {
            m_cacheId[a - 0x8004] = d[k];
        }
    }
    reply(0x38, {0x00});
}

void Tpdd::opMemRead(const std::vector<uint8_t>& d) {
    if (d.size() != 4 || d[0] > 1 || d[3] > 252) {
        reply(0x38, {0x36});
        return;
    }
    const int addr = (d[1] << 8) | d[2];
    std::vector<uint8_t> v = {d[0], d[1], d[2]};
    for (int k = 0; k < d[3]; ++k) {
        const int a = addr + k;
        uint8_t b = 0;
        if (d[0] == 0) b = a < SECTOR_SIZE ? m_cache[a] : 0;
        else if (a >= 0x8004 && a < 0x8004 + ID_SIZE + 1) b = m_cacheId[a - 0x8004];
        v.push_back(b);
    }
    reply(0x39, v);
}

// --- FDC mode -------------------------------------------------------------

void Tpdd::fdcAnswer(uint8_t err, uint8_t dat, uint16_t len) {
    char buf[9];
    std::snprintf(buf, sizeof buf, "%02X%02X%04X", err, dat, len);
    for (int i = 0; i < 8; ++i) m_out.push_back((uint8_t)buf[i]);
}

void Tpdd::fdcByte(uint8_t b) {
    switch (m_fdcWait) {
    case FdcWait::Line:
        if (b == '\r') {
            const std::string line = m_line;
            m_line.clear();
            fdcCommand(line);
        } else if (b != '\n' && m_line.size() < 64) {
            m_line.push_back((char)b);
        }
        break;
    case FdcWait::Cr:
        if (b != '\r') break;
        m_fdcWait = FdcWait::Line;
        if (m_fdcCmd == 'A') {
            const auto& id = m_sectors[m_fdcSector].id;
            m_out.insert(m_out.end(), id.begin(), id.end());
        } else {
            const int size = logicalSize(m_fdcSector);
            const auto& data = m_sectors[m_fdcSector].data;
            const int off = (m_fdcLogical - 1) * size;
            m_out.insert(m_out.end(), data.begin() + off, data.begin() + off + size);
        }
        break;
    case FdcWait::Bytes:
        m_fdcBuf.push_back(b);
        if (m_fdcBuf.size() < m_fdcNeed) break;
        m_fdcWait = FdcWait::Line;
        if (m_fdcCmd == 'S') {
            for (size_t i = 0; i < m_sectors.size(); ++i)
                if (std::equal(m_fdcBuf.begin(), m_fdcBuf.end(), m_sectors[i].id.begin())) {
                    fdcAnswer(0x00, (uint8_t)i, (uint16_t)logicalSize((int)i));
                    return;
                }
            fdcAnswer(0x3C, 0xFF, 0);
        } else if (m_fdcCmd == 'B' || m_fdcCmd == 'C') {
            std::copy(m_fdcBuf.begin(), m_fdcBuf.end(), m_sectors[m_fdcSector].id.begin());
            persistImage();
            fdcAnswer(0x00, (uint8_t)m_fdcSector, 0);
        } else {
            const int size = logicalSize(m_fdcSector);
            std::copy(m_fdcBuf.begin(), m_fdcBuf.end(),
                      m_sectors[m_fdcSector].data.begin() + (m_fdcLogical - 1) * size);
            persistImage();
            fdcAnswer(0x00, (uint8_t)m_fdcSector, (uint16_t)size);
        }
        break;
    }
}

void Tpdd::fdcCommand(const std::string& line) {
    size_t i = 0;
    while (i < line.size() && line[i] == ' ') ++i;
    if (i >= line.size()) return;
    const char cmd = (char)std::toupper((unsigned char)line[i++]);
    std::vector<int> p;
    bool bad = false;
    {
        std::string rest = line.substr(i);
        size_t k = 0;
        while (k < rest.size() && rest[k] == ' ') ++k;
        while (k < rest.size()) {
            size_t e = rest.find(',', k);
            const std::string tok = rest.substr(k, e == std::string::npos ? std::string::npos : e - k);
            if (tok.empty() || tok.find_first_not_of("0123456789 ") != std::string::npos) bad = true;
            else p.push_back(std::atoi(tok.c_str()));
            if (e == std::string::npos) break;
            k = e + 1;
        }
    }
    m_fdcCmd = cmd;
    const int n = (int)m_sectors.size();
    if (cmd == 'M') {
        if (!p.empty() && p[0] == 1) m_fdc = false;
        return;
    }
    if (bad) {
        fdcAnswer(0x21, 0, 0);
        return;
    }
    if (!m_diskPresent && cmd != 'D') {
        fdcAnswer(0xD1, 0, 0);
        return;
    }
    switch (cmd) {
    case 'D':
        fdcAnswer(0x00, (uint8_t)((m_writeProtect ? 0x20 : 0) | (m_diskPresent ? 0 : 0x80)), 0);
        return;
    case 'F':
    case 'G': {
        const int lsc = p.empty() ? 3 : p[0];
        if (lsc > 6) {
            fdcAnswer(0x33, 0, 0);
            return;
        }
        if (m_writeProtect) {
            fdcAnswer(0xB0, 0, 0);
            return;
        }
        for (Sector& s : m_sectors) {
            s = Sector{};
            s.lsc = (uint8_t)lsc;
        }
        persistImage();
        fdcAnswer(0x00, 0, 0);
        return;
    }
    case 'A':
    case 'B':
    case 'C':
    case 'R':
    case 'W':
    case 'X': {
        const int ps = p.empty() ? 0 : p[0];
        if (ps >= n) {
            fdcAnswer(0x13, (uint8_t)ps, 0);
            return;
        }
        m_fdcSector = ps;
        if (cmd == 'A') {
            fdcAnswer(0x00, (uint8_t)ps, (uint16_t)logicalSize(ps));
            m_fdcWait = FdcWait::Cr;
            return;
        }
        if ((cmd == 'B' || cmd == 'C' || cmd == 'W' || cmd == 'X') && m_writeProtect) {
            fdcAnswer(0xB0, (uint8_t)ps, 0);
            return;
        }
        if (cmd == 'B' || cmd == 'C') {
            fdcAnswer(0x00, (uint8_t)ps, 0);
            m_fdcBuf.clear();
            m_fdcNeed = ID_SIZE;
            m_fdcWait = FdcWait::Bytes;
            return;
        }
        const int ls = p.size() > 1 ? p[1] : 1;
        const int size = logicalSize(ps);
        if (ls < 1) {
            fdcAnswer(0x11, (uint8_t)ps, 0);
            return;
        }
        if (ls > SECTOR_SIZE / size) {
            fdcAnswer(0x12, (uint8_t)ps, 0);
            return;
        }
        m_fdcLogical = ls;
        fdcAnswer(0x00, (uint8_t)ps, (uint16_t)size);
        if (cmd == 'R') {
            m_fdcWait = FdcWait::Cr;
        } else {
            m_fdcBuf.clear();
            m_fdcNeed = (size_t)size;
            m_fdcWait = FdcWait::Bytes;
        }
        return;
    }
    case 'S':
        fdcAnswer(0x00, 0, 0);
        m_fdcBuf.clear();
        m_fdcNeed = ID_SIZE;
        m_fdcWait = FdcWait::Bytes;
        return;
    default: fdcAnswer(0xC0, 0, 0); return;
    }
}
