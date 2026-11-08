#include "disk_file.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

namespace diskfile {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

bool endsWith(const std::string& s, const std::string& tail) {
    return s.size() > tail.size() && lower(s.substr(s.size() - tail.size())) == tail;
}

std::string extOf(const std::string& name) {
    const size_t slash = name.find_last_of("/\\");
    const size_t dot = name.rfind('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
    return lower(name.substr(dot + 1));
}

// The disk formats the loaders take; a zip's first entry with one of these is the image
bool knownDiskExt(const std::string& ext) {
    static const char* const exts[] = {"d64", "d71", "d80", "d81", "d82", "g64", "imd", "atr",
                                       "dsk", "do",  "po",  "nib", "woz", "ssd", "dsd", "img"};
    for (const char* e : exts) if (ext == e) return true;
    return false;
}

bool readFile(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return !f.bad();
}

// Replace `path` with `bytes` through a temporary file, keeping its permissions
bool replaceFile(const std::string& path, const std::vector<uint8_t>& bytes, std::string* err) {
    const std::string tmp = path + ".tmp" + std::to_string((long)getpid());
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (f) f.write((const char*)bytes.data(), (std::streamsize)bytes.size());
        if (!f) {
            std::remove(tmp.c_str());
            if (err) *err = "Cannot write " + tmp + ".";
            return false;
        }
    }
    struct stat st;
    if (stat(path.c_str(), &st) == 0) chmod(tmp.c_str(), st.st_mode & 07777);
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        if (err) *err = "Cannot replace " + path + ".";
        return false;
    }
    return true;
}

uint16_t rd16(const std::vector<uint8_t>& b, size_t at) { return (uint16_t)(b[at] | (b[at + 1] << 8)); }
uint32_t rd32(const std::vector<uint8_t>& b, size_t at) { return (uint32_t)rd16(b, at) | ((uint32_t)rd16(b, at + 2) << 16); }
void wr16(std::vector<uint8_t>& b, uint16_t v) { b.push_back((uint8_t)v); b.push_back((uint8_t)(v >> 8)); }
void wr32(std::vector<uint8_t>& b, uint32_t v) { wr16(b, (uint16_t)v); wr16(b, (uint16_t)(v >> 16)); }

// ---------------------------------------------------------------------------
// Zip (PKWARE APPNOTE 4.3: the central directory, stored and deflated entries)
// ---------------------------------------------------------------------------
struct ZipEntry {
    std::string name;
    uint16_t method = 0;
    uint32_t crc = 0, csize = 0, usize = 0, local = 0;
};

bool zipEntries(const std::vector<uint8_t>& z, std::vector<ZipEntry>& out) {
    // The end-of-central-directory record: within the last 64 KB + 22 bytes
    if (z.size() < 22) return false;
    size_t eocd = std::string::npos;
    const size_t stop = z.size() > 65557 ? z.size() - 65557 : 0;
    for (size_t at = z.size() - 22 + 1; at-- > stop;)
        if (rd32(z, at) == 0x06054b50) { eocd = at; break; }
    if (eocd == std::string::npos) return false;
    const unsigned count = rd16(z, eocd + 10);
    size_t at = rd32(z, eocd + 16);
    for (unsigned i = 0; i < count; ++i) {
        if (at + 46 > z.size() || rd32(z, at) != 0x02014b50) return false;
        ZipEntry e;
        e.method = rd16(z, at + 10);
        e.crc = rd32(z, at + 16);
        e.csize = rd32(z, at + 20);
        e.usize = rd32(z, at + 24);
        const size_t nameLen = rd16(z, at + 28), extra = rd16(z, at + 30), comment = rd16(z, at + 32);
        e.local = rd32(z, at + 42);
        if (at + 46 + nameLen > z.size()) return false;
        e.name.assign((const char*)&z[at + 46], nameLen);
        at += 46 + nameLen + extra + comment;
        if (!e.name.empty() && e.name.back() != '/') out.push_back(e);   // not a folder
    }
    return true;
}

// The entry holding the image: the first with a disk extension, else the first file
const ZipEntry* pickEntry(const std::vector<ZipEntry>& entries) {
    for (const ZipEntry& e : entries)
        if (knownDiskExt(extOf(e.name))) return &e;
    return entries.empty() ? nullptr : &entries.front();
}

bool inflateRaw(const uint8_t* in, size_t inLen, std::vector<uint8_t>& out, size_t expect) {
    z_stream s{};
    if (inflateInit2(&s, -MAX_WBITS) != Z_OK) return false;
    out.resize(expect);
    s.next_in = const_cast<Bytef*>(in);
    s.avail_in = (uInt)inLen;
    s.next_out = out.data();
    s.avail_out = (uInt)out.size();
    const int r = inflate(&s, Z_FINISH);
    const bool ok = r == Z_STREAM_END && s.total_out == expect;
    inflateEnd(&s);
    return ok;
}

bool zipRead(const std::vector<uint8_t>& z, std::vector<uint8_t>& out, std::string* err) {
    std::vector<ZipEntry> entries;
    if (!zipEntries(z, entries)) { if (err) *err = "Not a zip file."; return false; }
    const ZipEntry* e = pickEntry(entries);
    if (!e) { if (err) *err = "The zip is empty."; return false; }
    const size_t lh = e->local;
    if (lh + 30 > z.size() || rd32(z, lh) != 0x04034b50) { if (err) *err = "Damaged zip."; return false; }
    const size_t data = lh + 30 + rd16(z, lh + 26) + rd16(z, lh + 28);
    if (data + e->csize > z.size()) { if (err) *err = "Damaged zip."; return false; }
    if (e->method == 0) {
        out.assign(z.begin() + (long)data, z.begin() + (long)(data + e->csize));
    } else if (e->method == 8) {
        if (!inflateRaw(&z[data], e->csize, out, e->usize)) { if (err) *err = "Damaged zip."; return false; }
    } else {
        if (err) *err = "Unsupported zip compression (method " + std::to_string(e->method) + ").";
        return false;
    }
    if (crc32(0, out.data(), (uInt)out.size()) != e->crc) { if (err) *err = "Zip CRC error."; return false; }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Building blocks
// ---------------------------------------------------------------------------
bool gunzip(const std::vector<uint8_t>& in, std::vector<uint8_t>& out) {
    out.clear();
    z_stream s{};
    if (inflateInit2(&s, 32 + MAX_WBITS) != Z_OK) return false;   // gzip or zlib header
    s.next_in = const_cast<Bytef*>(in.data());
    s.avail_in = (uInt)in.size();
    uint8_t buf[65536];
    int r = Z_OK;
    for (;;) {
        s.next_out = buf;
        s.avail_out = sizeof buf;
        r = inflate(&s, Z_NO_FLUSH);
        out.insert(out.end(), buf, buf + (sizeof buf - s.avail_out));
        if (r == Z_STREAM_END) {
            if (s.avail_in == 0) break;
            inflateReset(&s);                     // another gzip member follows
            continue;
        }
        if (r != Z_OK) break;
    }
    inflateEnd(&s);
    return r == Z_STREAM_END;
}

std::vector<uint8_t> gzip(const std::vector<uint8_t>& in, const std::string& name) {
    z_stream s{};
    deflateInit2(&s, Z_BEST_COMPRESSION, Z_DEFLATED, 16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
    gz_header h{};
    std::string n = name;
    if (!n.empty()) {
        h.name = (Bytef*)&n[0];
        h.os = 3;                                 // Unix
        h.time = (uLong)std::time(nullptr);
        deflateSetHeader(&s, &h);
    }
    std::vector<uint8_t> out(deflateBound(&s, (uLong)in.size()) + 64 + n.size());
    s.next_in = const_cast<Bytef*>(in.data());
    s.avail_in = (uInt)in.size();
    s.next_out = out.data();
    s.avail_out = (uInt)out.size();
    deflate(&s, Z_FINISH);
    out.resize(s.total_out);
    deflateEnd(&s);
    return out;
}

std::vector<uint8_t> zipOne(const std::string& entryName, const std::vector<uint8_t>& data) {
    // Raw deflate
    z_stream s{};
    deflateInit2(&s, Z_BEST_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
    std::vector<uint8_t> packed(deflateBound(&s, (uLong)data.size()) + 16);
    s.next_in = const_cast<Bytef*>(data.data());
    s.avail_in = (uInt)data.size();
    s.next_out = packed.data();
    s.avail_out = (uInt)packed.size();
    deflate(&s, Z_FINISH);
    packed.resize(s.total_out);
    deflateEnd(&s);

    const uint32_t crc = (uint32_t)crc32(0, data.data(), (uInt)data.size());
    // MS-DOS time and date
    const std::time_t now = std::time(nullptr);
    std::tm t{};
    localtime_r(&now, &t);
    const uint16_t dosTime = (uint16_t)((t.tm_hour << 11) | (t.tm_min << 5) | (t.tm_sec / 2));
    const uint16_t dosDate = (uint16_t)(((t.tm_year - 80) << 9) | ((t.tm_mon + 1) << 5) | t.tm_mday);

    std::vector<uint8_t> z;
    auto common = [&]() {
        wr16(z, 20); wr16(z, 0); wr16(z, 8);      // version needed, flags, deflate
        wr16(z, dosTime); wr16(z, dosDate);
        wr32(z, crc); wr32(z, (uint32_t)packed.size()); wr32(z, (uint32_t)data.size());
        wr16(z, (uint16_t)entryName.size());
    };
    wr32(z, 0x04034b50);                          // local file header
    common();
    wr16(z, 0);                                   // extra
    z.insert(z.end(), entryName.begin(), entryName.end());
    z.insert(z.end(), packed.begin(), packed.end());
    const uint32_t cd = (uint32_t)z.size();
    wr32(z, 0x02014b50);                          // central directory header
    wr16(z, 0x0314);                              // made by: Unix, 2.0
    common();
    wr16(z, 0); wr16(z, 0); wr16(z, 0); wr16(z, 0);  // extra, comment, disk, internal attrs
    wr32(z, 0100644u << 16);                      // external attrs: a regular file, rw-r--r--
    wr32(z, 0);                                   // local header offset
    z.insert(z.end(), entryName.begin(), entryName.end());
    const uint32_t cdSize = (uint32_t)z.size() - cd;
    wr32(z, 0x06054b50);                          // end of central directory
    wr16(z, 0); wr16(z, 0); wr16(z, 1); wr16(z, 1);
    wr32(z, cdSize); wr32(z, cd);
    wr16(z, 0);
    return z;
}

// ---------------------------------------------------------------------------
// Image files
// ---------------------------------------------------------------------------
Kind kindOf(const std::string& path) {
    if (endsWith(path, ".gz")) return Kind::Gzip;
    if (endsWith(path, ".zip")) return Kind::Zip;
    return Kind::Plain;
}

std::string imageName(const std::string& path) {
    switch (kindOf(path)) {
        case Kind::Gzip: return path.substr(0, path.size() - 3);
        case Kind::Zip: {
            std::vector<uint8_t> z;
            std::vector<ZipEntry> entries;
            if (!readFile(path, z) || !zipEntries(z, entries)) return {};
            const ZipEntry* e = pickEntry(entries);
            return e ? e->name : std::string();
        }
        default: return path;
    }
}

std::string imageExt(const std::string& path) { return extOf(imageName(path)); }

bool read(const std::string& path, std::vector<uint8_t>& out, std::string* err) {
    std::vector<uint8_t> raw;
    if (!readFile(path, raw)) { if (err) *err = "Cannot open " + path + "."; return false; }
    switch (kindOf(path)) {
        case Kind::Plain: out = std::move(raw); return true;
        case Kind::Gzip:
            if (gunzip(raw, out)) return true;
            if (err) *err = path + " is not a valid gzip file.";
            return false;
        case Kind::Zip: return zipRead(raw, out, err);
    }
    return false;
}

bool writable(const std::string& path) {
    if (access(path.c_str(), W_OK) != 0) return false;
    if (kindOf(path) == Kind::Gzip) {
        const size_t slash = path.find_last_of('/');
        const std::string dir = slash == std::string::npos ? "." : path.substr(0, slash ? slash : 1);
        return access(dir.c_str(), W_OK) == 0;    // replaced through a temporary file
    }
    if (kindOf(path) == Kind::Zip) {
        std::vector<uint8_t> z;
        std::vector<ZipEntry> entries;
        if (!readFile(path, z) || !zipEntries(z, entries) || entries.size() != 1) return false;
        if (entries[0].method != 0 && entries[0].method != 8) return false;
        const size_t slash = path.find_last_of('/');
        const std::string dir = slash == std::string::npos ? "." : path.substr(0, slash ? slash : 1);
        return access(dir.c_str(), W_OK) == 0;
    }
    return true;
}

bool save(const std::string& path, const std::vector<uint8_t>& data, std::string* err) {
    switch (kindOf(path)) {
        case Kind::Plain: {
            std::ofstream f(path, std::ios::binary | std::ios::trunc);
            if (f) f.write((const char*)data.data(), (std::streamsize)data.size());
            if (!f && err) *err = "Cannot write " + path + ".";
            return (bool)f;
        }
        case Kind::Gzip: {
            std::string inner = imageName(path);
            const size_t slash = inner.find_last_of('/');
            if (slash != std::string::npos) inner = inner.substr(slash + 1);
            return replaceFile(path, gzip(data, inner), err);
        }
        case Kind::Zip: {
            if (!writable(path)) {
                if (err) *err = path + " is read-only (a zip with more than one file, or not writable).";
                return false;
            }
            return replaceFile(path, zipOne(imageName(path), data), err);
        }
    }
    return false;
}

bool patch(const std::string& path, const std::vector<uint8_t>& data, size_t off, size_t len,
           std::string* err) {
    if (kindOf(path) != Kind::Plain) return save(path, data, err);
    if (off + len > data.size()) return false;
    std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!f) { if (err) *err = "Cannot write " + path + "."; return false; }
    f.seekp((std::streamoff)off);
    f.write((const char*)data.data() + off, (std::streamsize)len);
    return (bool)f;
}

}  // namespace diskfile
