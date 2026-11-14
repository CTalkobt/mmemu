// CBM DOS on a host folder at the Commodore bus command level (L2:
// TALK/LISTEN, L3: Commodore DOS), shared by the X16 HostFS and the PET's
// IEEE-488 drive.
//
// Ported from x16-emulator src/ieee.c (HostFS), the IEEE intercept and
// STATUS write in src/main.c, and src/iso_8859_15.c,
// Copyright (c) 2019, 2022, 2023 Michael Steil, et al. All rights reserved.
// License: 2-clause BSD:
//
//   Redistribution and use in source and binary forms, with or without
//   modification, are permitted provided that the following conditions are
//   met: 1. Redistributions of source code must retain the above copyright
//   notice, this list of conditions and the following disclaimer.
//   2. Redistributions in binary form must reproduce the above copyright
//   notice, this list of conditions and the following disclaimer in the
//   documentation and/or other materials provided with the distribution.
//   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
//   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES ARE DISCLAIMED.
//
// The UTF-8 decoder is the public-domain branchless decoder (utf8.h); the
// encoder is after Ondřej Hruška's utf8_encode (MIT).
//
// Not ported: the 65C816-only EXTAPI16 calls (XMACPTR / XMCIOUT), the
// "-prg" autoload hack, and the cycle compensation for host time.

#include "cbm_host_dos.h"
#include "util/logging.h"
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr int WILDCARD_ALL = 0;
constexpr int WILDCARD_PRG = 1;
constexpr int WILDCARD_DIR = 2;
constexpr size_t CHANNEL_NAME_MAX = 79;         // x16-emulator: uint8_t name[80]
constexpr size_t CMD_MAX = 255;                 // uint8_t cmd[256]

const char* BLOCKS_FREE = "BLOCKS FREE.";

auto logger() { return LogRegistry::instance().getLogger("cbm.hostdos"); }

// Byte k of s, 0 past the end (the C code reads NUL terminators)
inline uint8_t at(const std::string& s, size_t k) { return k < s.size() ? (uint8_t)s[k] : 0; }

// The string up to its first NUL
inline std::string cstr(const std::string& s, size_t from = 0) {
    if (from >= s.size()) return "";
    std::string r = s.substr(from);
    size_t z = r.find('\0');
    return z == std::string::npos ? r : r.substr(0, z);
}

// --- UTF-8 / ISO-8859-15 (utf8.h, utf8_encode.h, iso_8859_15.c) ----------

// Decode the code point at s[off] (s padded with zeros as the branchless
// decoder needs); returns the length consumed (at least 1).
int utf8Decode(const std::string& str, size_t off, uint32_t* c, int* e) {
    static const char lengths[] = {
        1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
        0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 2, 3, 3, 4, 0
    };
    static const int masks[] = {0x00, 0x7f, 0x1f, 0x0f, 0x07};
    static const uint32_t mins[] = {4194304, 0, 128, 2048, 65536};
    static const int shiftc[] = {0, 18, 12, 6, 0};
    static const int shifte[] = {0, 6, 4, 2, 0};

    uint8_t s[4] = { at(str, off), at(str, off + 1), at(str, off + 2), at(str, off + 3) };
    int len = lengths[s[0] >> 3];
    *c  = (uint32_t)(s[0] & masks[len]) << 18;
    *c |= (uint32_t)(s[1] & 0x3f) << 12;
    *c |= (uint32_t)(s[2] & 0x3f) <<  6;
    *c |= (uint32_t)(s[3] & 0x3f) <<  0;
    *c >>= shiftc[len];
    *e  = (*c < mins[len]) << 6;
    *e |= ((*c >> 11) == 0x1b) << 7;
    *e |= (*c > 0x10FFFF) << 8;
    *e |= (s[1] & 0xc0) >> 2;
    *e |= (s[2] & 0xc0) >> 4;
    *e |= (s[3]       ) >> 6;
    *e ^= 0x2a;
    *e >>= shifte[len];
    return len + !len;
}

void utf8Encode(std::string& out, uint32_t utf) {
    if (utf <= 0x7F) {
        out += (char)utf;
    } else if (utf <= 0x07FF) {
        out += (char)(((utf >> 6) & 0x1F) | 0xC0);
        out += (char)(((utf >> 0) & 0x3F) | 0x80);
    } else if (utf <= 0xFFFF) {
        out += (char)(((utf >> 12) & 0x0F) | 0xE0);
        out += (char)(((utf >>  6) & 0x3F) | 0x80);
        out += (char)(((utf >>  0) & 0x3F) | 0x80);
    } else if (utf <= 0x10FFFF) {
        out += (char)(((utf >> 18) & 0x07) | 0xF0);
        out += (char)(((utf >> 12) & 0x3F) | 0x80);
        out += (char)(((utf >>  6) & 0x3F) | 0x80);
        out += (char)(((utf >>  0) & 0x3F) | 0x80);
    } else {
        out += "\xEF\xBF\xBD";
    }
}

uint8_t iso8859_15FromUnicode(uint32_t c) {
    if (c == '\n') return '\r';
    switch (c) {
        case 0x20ac: return 0xa4;   // €
        case 0x160:  return 0xa6;   // Š
        case 0x161:  return 0xa8;   // š
        case 0x17d:  return 0xb4;   // Ž
        case 0x17e:  return 0xb8;   // ž
        case 0x152:  return 0xbc;   // Œ
        case 0x153:  return 0xbd;   // œ
        case 0x178:  return 0xbe;   // Ÿ
    }
    switch (c) {
        case 0xa4: case 0xa6: case 0xa8: case 0xb4:
        case 0xb8: case 0xbc: case 0xbd: case 0xbe:
            return '?';
    }
    if (c >= 256) return '?';
    return (uint8_t)c;
}

uint32_t unicodeFromIso8859_15(uint8_t c) {
    switch (c) {
        case 0xa4: return 0x20ac;
        case 0xa6: return 0x160;
        case 0xa8: return 0x161;
        case 0xb4: return 0x17d;
        case 0xb8: return 0x17e;
        case 0xbc: return 0x152;
        case 0xbd: return 0x153;
        case 0xbe: return 0x178;
        default:   return c;
    }
}

std::string utf8ToIsoString(const std::string& src) {
    std::string dst;
    size_t off = 0;
    while (off < src.size() && src[off]) {
        uint32_t cp;
        int e;
        off += utf8Decode(src, off, &cp, &e);
        if (e) { dst += '?'; continue; }
        switch (cp) {
            case ',': case '"': case '*': case ':':
            case '\\': case '<': case '>': case '|':
                // not valid in FAT32 file names, but possible on the host
                dst += '?';
                break;
            default:
                dst += (char)iso8859_15FromUnicode(cp);
                break;
        }
    }
    return dst;
}

uint32_t caseFoldUnicode(uint32_t cp) {
    if ((cp >= 0x41 && cp <= 0x5a) || (cp >= 0xc0 && cp <= 0xde)) cp += 0x20;
    else if (cp == 0x0160) cp = 0x0161;
    else if (cp == 0x017d) cp = 0x017e;
    else if (cp == 0x0152) cp = 0x0153;
    else if (cp == 0x0178) cp = 0xff;
    return cp;
}

uint8_t caseFoldIso(uint8_t f) {
    if ((f >= 0x41 && f <= 0x5a) || (f >= 0xc0 && f <= 0xde)) f += 0x20;
    else if (f == 0xa6) f = 0xa8;
    else if (f == 0xb4) f = 0xb8;
    else if (f == 0xbc) f = 0xbd;
    else if (f == 0xbe) f = 0xff;
    return f;
}

// The code point at str[*off]; *off moves to the last byte of the character
uint32_t utf8ToCodepoint(const std::string& str, int* off) {
    uint32_t cp;
    int e;
    int n = utf8Decode(str, (size_t)*off, &cp, &e);
    *off += n - 1;
    return e ? '?' : cp;
}

// Case-insensitive comparison of one UTF-8 character of each; 0 on a match
int compareUtf8CharI(const std::string& s1, const std::string& s2, int* off1, int* off2) {
    uint32_t cp1 = utf8ToCodepoint(s1, off1);
    uint32_t cp2 = utf8ToCodepoint(s2, off2);
    return (int)caseFoldUnicode(cp2) - (int)caseFoldUnicode(cp1);
}

std::optional<std::string> realPath(const std::string& p) {
    char* r = ::realpath(p.c_str(), nullptr);
    if (!r) return std::nullopt;
    std::string s(r);
    std::free(r);
    return s;
}

bool isSep(char c) { return c == '/' || c == '\\'; }

// Last '/' or '\\' in s, npos if none
size_t lastSep(const std::string& s) {
    size_t a = s.rfind('/'), b = s.rfind('\\');
    if (a == std::string::npos) return b;
    if (b == std::string::npos) return a;
    return a > b ? a : b;
}

const char* errorString(int e) {
    switch (e) {
        case 0x00: return " OK";
        case 0x01: return " FILES SCRATCHED";
        case 0x02: return "PARTITION SELECTED";
        case 0x20: return "READ ERROR";
        case 0x25: return "WRITE ERROR";
        case 0x26: return "WRITE PROTECT ON";
        case 0x30: case 0x31: case 0x32: return "SYNTAX ERROR";
        case 0x33: return "ILLEGAL FILENAME";
        case 0x34: return "EMPTY FILENAME";
        case 0x39: return "SUBDIRECTORY NOT FOUND";
        case 0x49: return "INVALID FORMAT";
        case 0x62: return " FILE NOT FOUND";
        case 0x63: return "FILE EXISTS";
        case 0x70: return "NO CHANNEL";
        case 0x71: return "DIRECTORY ERROR";
        case 0x72: return "PARTITION FULL";
        case 0x73: return "HOST FS V1.0 X16";
        case 0x74: return "DRIVE NOT READY";
        case 0x75: return "FORMAT ERROR";
        case 0x77: return "SELECTED PARTITION ILLEGAL";
        default:   return "";
    }
}


} // namespace

int parseCbmUnit(const std::string& v) {
    size_t used = 0;
    int u = std::stoi(v, &used, 10);
    if (used != v.size() || u < 8 || u > 31) throw std::invalid_argument("unit must be 8-31");
    return u;
}

CbmHostDos::~CbmHostDos() {
    for (int ch = 0; ch < 16; ch++) cclose(ch);
    closeDirectory();
}

void CbmHostDos::reset() {
    for (int ch = 0; ch < 16; ch++) cclose(ch);
    closeDirectory();
    m_listening = m_talking = m_opening = false;
    m_cmd.clear();
    m_initialized = false;          // re-resolved at the next call
}

bool CbmHostDos::init() {
    auto root = realPath(m_rootSetting.empty() ? "." : m_rootSetting);
    if (!root) {
        logger()->error("HostFS: cannot resolve the host folder \"{}\"", m_rootSetting);
        return false;
    }
    m_fsroot = *root;
    m_startin = m_fsroot;
    if (!m_startinSetting.empty()) {
        auto s = realPath(m_startinSetting);
        // must be inside the root, else the root
        if (s && s->compare(0, m_fsroot.size(), m_fsroot) == 0) m_startin = *s;
        else logger()->warn("HostFS: start directory \"{}\" is not inside {}; using the root",
                            m_startinSetting, m_fsroot);
    }
    m_cwd = m_startin;
    m_initialized = true;
    return true;
}

// ---------------------------------------------------------------------------
// The error channel
// ---------------------------------------------------------------------------

void CbmHostDos::setActivity(bool active) {
    m_active = active;
    onActivity(active);
}

void CbmHostDos::setErrorText(int e, const char* text, int t, int s) {
    std::snprintf((char*)m_error, sizeof(m_error), "%02x,%s,%02d,%02d\r", e, text, t, s);
    m_errorLen = (int)std::strlen((char*)m_error);
    m_errorPos = 0;
    m_errorFlag = !(e < 0x10 || e == 0x73);
    onErrorFlag(m_errorFlag);
}

void CbmHostDos::setError(int e, int t, int s) { setErrorText(e, errorString(e), t, s); }
void CbmHostDos::clearError() { setError(0, 0, 0); }

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

// [[@][<media 0-9>][</relative_path/> | <//absolute_path/>]:]<file_path>[*]
// Parses the part before the ':' and attaches the directory part to the name.
std::optional<std::string> CbmHostDos::parseDosFilename(const std::string& name, bool dirHandling) {
    m_overwrite = false;
    size_t colon = name.find(':');
    size_t p;                                       // index one past the ':'
    if (colon != std::string::npos) p = colon + 1;
    else if (dirHandling) p = name.size() + 1;
    else return name;

    size_t i = 0;
    std::string newname;
    if (!dirHandling) {
        if (at(name, i) == '@') { m_overwrite = true; i++; }   // overwrite flag
        while (at(name, i) >= '0' && at(name, i) <= '9') i++;  // medium: ignored
    }
    if (at(name, i) == '/') {                       // directory part
        i++;
        for (; i + 1 < p; i++) newname += name[i];
        if (newname.empty()) return std::nullopt;   // mangled (single slash?)
        if (newname.back() != '/') return std::nullopt;
    }
    if (p < name.size()) newname += name.substr(p);
    return newname;
}

std::optional<std::string> CbmHostDos::resolvePathUtf8(const std::string& name, bool mustExist, int wildcardType) {
    m_pathExists = false;
    clearError();

    // Absolute names are appended to the root (slash and all), relative ones to the cwd
    bool absolute = !name.empty() && isSep(name[0]);
    std::string tmp = absolute ? m_fsroot + name : m_cwd + "/" + name;
    std::string tmp2 = tmp;
    bool hasWildcardChars = tmp.find_first_of("*?") != std::string::npos;

    // Search the directory for the first match, in directory order
    size_t sep = lastSep(tmp);
    if (sep == std::string::npos) { setError(0x62, 0, 0); return std::nullopt; }
    std::string dir = tmp.substr(0, sep);
    std::string c = tmp.substr(sep + 1);

    DIR* dirp = opendir(dir.c_str());
    if (!dirp) { setError(0x62, 0, 0); return std::nullopt; }

    std::optional<std::string> ret;
    bool found = false;
    struct dirent* dp;
    while ((dp = readdir(dirp))) {
        std::string d = dp->d_name;
        // a wildcard at the first position does not match leading-dot names
        if ((at(c, 0) == '*' || at(c, 0) == '?') && d[0] == '.') continue;
        int i = 0, j = 0;
        for (; j < (int)c.size() && i < (int)d.size(); i++, j++) {
            if (c[j] == '*') {
                found = true;
                break;
            } else if (c[j] == '?') {
                // '?' eats the extra UTF-8 bytes of a character
                uint8_t b = (uint8_t)d[i];
                if ((b & 0xe0) == 0xc0) i++;
                else if ((b & 0xf0) == 0xe0) i += 2;
                else if ((b & 0xf8) == 0xf0) i += 3;
                continue;
            } else if (compareUtf8CharI(c, d, &j, &i)) {
                break;
            }
        }
        if (i == (int)d.size() && j == (int)c.size()) found = true;
        else if (i == (int)d.size() && at(c, j) == '*') found = true;

        if (found) {
            std::string r = dir + "/" + d;
            if (wildcardType) {
                struct stat st;
                mode_t mode = ::stat(r.c_str(), &st) == 0 ? st.st_mode : 0;
                if ((wildcardType == WILDCARD_DIR && !S_ISDIR(mode)) ||
                    (wildcardType == WILDCARD_PRG && !S_ISREG(mode))) {
                    found = false;
                    continue;
                }
            }
            ret = r;
            break;
        }
    }
    closedir(dirp);

    if (ret) {
        tmp = *ret;
    } else if (hasWildcardChars) {
        setError(0x62, 0, 0);
        return std::nullopt;
    } else {
        tmp = tmp2;                                 // the name as given
    }

    // Resolve with the OS
    ret = realPath(tmp);
    if (!ret) {
        if (mustExist) {
            setError(0x62, 0, 0);
        } else {
            // It need not exist, as long as everything but its last element does
            std::string t = name;
            size_t cpos = lastSep(t);
            if (cpos != std::string::npos) t = t.substr(0, cpos);
            std::string base = absolute ? m_fsroot : m_cwd + "/";
            std::string r;   // the parent (or, with no separator, the whole name)
            if (absolute && cpos == 0) {             // the leading slash was the only one
                r = m_fsroot + name;
                cpos = std::string::npos;
            } else {
                r = base + t;
            }
            if (cpos != std::string::npos) {
                if (!realPath(r)) {
                    setError(0x62, 0, 0);            // a missing parent too
                    ret = std::nullopt;
                } else {
                    // The parent exists: the name as given (x16-emulator appends
                    // it to the cwd here even for an absolute name; the root here)
                    ret = absolute ? m_fsroot + name : m_cwd + "/" + name;
                }
            } else {
                ret = r;
            }
        }
    } else {
        m_pathExists = true;
    }

    if (!ret) return ret;

    // Nothing outside the root
    const std::string& root = m_fsroot;
    if (root.size() > ret->size() || ret->compare(0, root.size(), root) != 0 ||
        (root.size() < ret->size() && !isSep(root.back()) && !isSep((*ret)[root.size()]))) {
        setError(0x62, 0, 0);
        return std::nullopt;
    }
    return ret;
}

std::optional<std::string> CbmHostDos::resolvePathIso(const std::string& name, bool mustExist, int wildcardType) {
    std::string buf;
    for (size_t i = 0; i < name.size() && name[i]; i++) utf8Encode(buf, unicodeFromIso8859_15((uint8_t)name[i]));
    return resolvePathUtf8(buf, mustExist, wildcardType);
}

// The emulated cwd for the directory header, padded with spaces to len
void CbmHostDos::cgetcwd(uint8_t* buf, size_t len) {
    std::string src;
    int o = isSep(m_fsroot.back()) ? -1 : 0;
    if (m_fsroot.size() == m_cwd.size()) src = "/";
    else src = m_cwd.substr(m_fsroot.size() + o);
    for (size_t k = 0; k < len; k++) {
        uint8_t ch = k < src.size() ? (uint8_t)src[k] : 0;
        if (ch == 0) ch = ' ';
        if (ch == '\\') ch = '/';
        buf[k] = ch;
    }
}

// ---------------------------------------------------------------------------
// Directory listings
// ---------------------------------------------------------------------------

void CbmHostDos::closeDirectory() {
    if (m_dirlistDirp) {
        closedir((DIR*)m_dirlistDirp);
        m_dirlistDirp = nullptr;
    }
}

namespace {
void header(std::vector<uint8_t>& d, uint16_t loadAddr) {
    // load address, link (BASIC relinks), line 0, RVS ON
    const uint8_t h[] = { (uint8_t)loadAddr, (uint8_t)(loadAddr >> 8), 1, 1, 0, 0, 0x12, '"' };
    d.insert(d.end(), h, h + sizeof h);
}
void trailer(std::vector<uint8_t>& d) {
    const char* t = "\" HOST ";
    d.insert(d.end(), t, t + std::strlen(t));
    d.push_back(0);
}
void blocksFree(std::vector<uint8_t>& d) {
    d.push_back(1); d.push_back(1);
    d.push_back(255); d.push_back(255);                  // "65535"
    d.insert(d.end(), BLOCKS_FREE, BLOCKS_FREE + std::strlen(BLOCKS_FREE));
    d.push_back(0);
    d.push_back(0); d.push_back(0);                      // end of program
}
void blocksPad(std::vector<uint8_t>& d, size_t fileSize) {
    if (fileSize < 1000) {
        d.push_back(' ');
        if (fileSize < 100) {
            d.push_back(' ');
            if (fileSize < 10) d.push_back(' ');
        }
    }
}
} // namespace

// "$", with options such as "$=T:MATCH*=P"
std::vector<uint8_t> CbmHostDos::createDirectoryListing(const std::string& ds) {
    std::vector<uint8_t> data;
    m_dirlistEof = true;
    m_dirlistTimestamps = false;
    m_dirlistLong = false;
    m_dirlistTypeFilter = 0;
    m_dirlistWildcard.clear();

    size_t len = std::strlen(ds.c_str());
    size_t i = 0;
    while (i < len) {
        if (at(ds, i) == ':' || i == 0) {
            i++;
            while (i < len) {
                if (i == 1 && at(ds, i) == ':') i++;
                if (at(ds, i) == '=' || at(ds, i) == 0) {
                    uint8_t o = at(ds, ++i);
                    if (o == 'D') m_dirlistTypeFilter = 'D';
                    else if (o == 'P' && i != 2) m_dirlistTypeFilter = 'P';
                    else if (o == 'T' && i == 2) m_dirlistTimestamps = true;
                    else if (o == 'L' && i == 2) { m_dirlistTimestamps = true; m_dirlistLong = true; }
                    break;
                } else {
                    m_dirlistWildcard += (char)at(ds, i++);
                }
            }
        }
        i++;
    }

    header(data, m_dirLoadAddr);
    uint8_t title[16];
    cgetcwd(title, 16);
    data.insert(data.end(), title, title + 16);
    trailer(data);

    closeDirectory();
    if (!(m_dirlistDirp = opendir(m_cwd.c_str()))) return {};
    m_dirlistEof = false;
    return data;
}

std::vector<uint8_t> CbmHostDos::continueDirectoryListing() {
    std::vector<uint8_t> data;
    struct dirent* dp;
    while (m_dirlistDirp && (dp = readdir((DIR*)m_dirlistDirp))) {
        std::string dname = dp->d_name;
        struct stat st;
        if (m_dirlistTypeFilter) {
            // a quick stat to skip what the filter rejects
            std::string tn = m_cwd + "/" + dname;
            mode_t mode = ::stat(tn.c_str(), &st) == 0 ? st.st_mode : 0;
            if (m_dirlistTypeFilter == 'D' && !S_ISDIR(mode)) continue;
            if (m_dirlistTypeFilter == 'P' && !S_ISREG(mode)) continue;
        }

        auto resolved = resolvePathUtf8(dname, true, WILDCARD_ALL);
        if (!resolved) continue;
        if (::stat(resolved->c_str(), &st) != 0) std::memset(&st, 0, sizeof st);

        // no . or .. in the root, as on the SD card (FAT32)
        if ((dname == ".." || dname == ".") && m_cwd == m_fsroot) continue;

        std::string tmpnam = utf8ToIsoString(dname);

        if (!m_dirlistWildcard.empty()) {
            const std::string& w = m_dirlistWildcard;
            if ((w[0] == '*' || w[0] == '?') && at(tmpnam, 0) == '.') continue;
            bool found = false;
            size_t k = 0;
            for (; k < w.size() && k < tmpnam.size(); k++) {
                if (w[k] == '*') { found = true; break; }
                else if (w[k] == '?') continue;
                else if (caseFoldIso((uint8_t)w[k]) != caseFoldIso((uint8_t)tmpnam[k])) break;
            }
            if (k == tmpnam.size() && k == w.size()) found = true;
            else if (k == tmpnam.size() && at(w, k) == '*') found = true;
            if (!found) continue;
        }

        data.push_back(1);                          // link
        data.push_back(1);

        size_t fileSize;
        if (m_dirlistLong) {
            uint8_t unitCh = 'K';
            fileSize = ((size_t)st.st_size + 1023) / 1024;
            if (fileSize > 0xFFFF) { fileSize /= 1024; unitCh = 'M'; }
            if (fileSize > 0xFFFF) { fileSize /= 1024; unitCh = 'G'; }
            if (fileSize > 0xFFFF) fileSize = 0xFFFF;
            data.push_back(fileSize & 0xFF);
            data.push_back((uint8_t)(fileSize >> 8));
            data.push_back(unitCh);
            data.push_back('B');
        } else {
            fileSize = ((size_t)st.st_size + 255) / 256;
            if (fileSize > 0xFFFF) fileSize = 0xFFFF;
            data.push_back(fileSize & 0xFF);
            data.push_back((uint8_t)(fileSize >> 8));
        }
        blocksPad(data, fileSize);
        data.push_back('"');
        data.insert(data.end(), tmpnam.begin(), tmpnam.end());
        data.push_back('"');
        for (size_t k = tmpnam.size(); k < 16; k++) data.push_back(' ');
        data.push_back(' ');
        const char* type = S_ISDIR(st.st_mode) ? "DIR" : "PRG";
        data.insert(data.end(), type, type + 3);
        data.push_back(' ');                        // '<' if protected

        if (m_dirlistTimestamps) {
            struct tm mtime;
            if (localtime_r(&st.st_mtime, &mtime)) {
                char buf[24];
                size_t n = std::strftime(buf, 20, "%Y-%m-%d %H:%M:%S", &mtime);
                data.push_back(' ');
                data.insert(data.end(), buf, buf + n);
                data.push_back(' ');
            }
        }
        if (m_dirlistLong) {
            uint8_t attr = 0;
            if (S_ISDIR(st.st_mode)) attr |= 0x10;
            if (S_ISREG(st.st_mode)) attr |= 0x20;
            if (!(st.st_mode & 0200)) attr |= 0x01;  // read-only
            uint64_t full = (uint64_t)st.st_size;
            if (full > 0xffffffffULL) full = 0xffffffffULL;
            char buf[24];
            int n = std::snprintf(buf, sizeof buf, "%02X %08X ", attr, (unsigned)full);
            data.insert(data.end(), buf, buf + n);
        }
        data.push_back(0);
        return data;
    }

    blocksFree(data);
    closeDirectory();
    m_dirlistEof = true;
    return data;
}

// "$=C": the path from the cwd up to the root, as directory entries
std::vector<uint8_t> CbmHostDos::createCwdListing() {
    std::vector<uint8_t> data;
    header(data, m_dirLoadAddr);
    uint8_t title[16];
    cgetcwd(title, 16);
    data.insert(data.end(), title, title + 16);
    trailer(data);

    // C-string surgery as in x16-emulator: walk back through the cwd
    std::vector<char> tmp(m_cwd.begin(), m_cwd.end());
    tmp.resize(m_cwd.size() + 3, 0);
    int i = (int)m_cwd.size();
    int j = (int)m_fsroot.size();
    for (; i >= j - 1 && i >= 1; --i) {
        if (i >= j && !isSep(tmp[i - 1])) continue;   // the start of a path element
        tmp[i - 1] = 0;
        if (i < j) std::strcpy(&tmp[i], "/");
        size_t namlen = std::strlen(&tmp[i]);
        if (!namlen) continue;                      // a doubled separator
        data.push_back(1);
        data.push_back(1);
        data.push_back(0);
        data.push_back(0);
        blocksPad(data, 0);
        data.push_back('"');
        data.insert(data.end(), &tmp[i], &tmp[i] + namlen);
        data.push_back('"');
        for (size_t k = namlen; k < 16; k++) data.push_back(' ');
        data.push_back(' ');
        data.push_back('D'); data.push_back('I'); data.push_back('R');
        data.push_back(0);
    }
    blocksFree(data);
    m_dirlistEof = true;
    return data;
}

// ---------------------------------------------------------------------------
// DOS commands
// ---------------------------------------------------------------------------

void CbmHostDos::command(const std::string& cmd) {
    if (cmd.empty() || !cmd[0]) return;
    if (cmd[0] == 'P') logger()->debug("COMMAND \"P\" [binary parameters suppressed]");
    else logger()->debug("COMMAND \"{}\"", cstr(cmd));
    switch (cmd[0]) {
        case 'C':                                   // CD, CP, C (copy)
            switch (at(cmd, 1)) {
                case 'D': cchdir(cstr(cmd, 2)); return;
                case 'P': setError(0x02, 0, 0); return;
                default:  setError(0x30, 0, 0); return;   // copy: not yet
            }
        case 'I':
            clearError();
            return;
        case 'M':
            switch (at(cmd, 1)) {
                case 'D': cmkdir(cstr(cmd, 2)); return;
                default:  setError(0x31, 0, 0); return;   // M-R/M-W: not implemented
            }
        case 'P':                                   // seek
            cseek(at(cmd, 1), (uint32_t)at(cmd, 2) | (uint32_t)at(cmd, 3) << 8 |
                              (uint32_t)at(cmd, 4) << 16 | (uint32_t)at(cmd, 5) << 24);
            return;
        case 'R':
            switch (at(cmd, 1)) {
                case 'D': crmdir(cstr(cmd, 2)); return;
                default:  crename(cstr(cmd)); return;
            }
        case 'S':
            switch (at(cmd, 1)) {
                case '-':                           // swap
                    if (at(cmd, 2) == '8' || at(cmd, 2) == 'D') { m_unit = 8; clearError(); }
                    else if (at(cmd, 2) == '9') { m_unit = 9; clearError(); }
                    else setError(0x31, 0, 0);
                    return;
                default:                            // scratch
                    cunlink(cstr(cmd));
                    return;
            }
        case 'T':                                   // tell
            ctell(at(cmd, 1));
            return;
        case 'U':
            switch (at(cmd, 1)) {
                case 'I':                           // UI: reset
                    setError(0x73, 0, 0);
                    return;
                case '0':                           // U0>n: change the unit
                    if (at(cmd, 2) == '>' && at(cmd, 3) >= 8 && at(cmd, 3) <= 15) {
                        m_unit = at(cmd, 3);
                        clearError();
                        return;
                    }
                    if (cmd.size() > 2 && at(cmd, 2) != '>') {   // a burst command
                        m_burstOut.clear();
                        if (((at(cmd, 2) >> 1) & 7) == 7) {     // fastload: the drive reads the file
                            m_fastload = cmd.substr(3);
                            m_fastloadPending = true;
                        } else {
                            m_burstOut.push_back(0x0F);         // 1111: no disk to read sectors from
                        }
                        clearError();
                        return;
                    }
            }
            [[fallthrough]];
        default:
            logger()->debug("(unsupported command ignored)");
    }
    setError(0x30, 0, 0);
}

void CbmHostDos::cchdir(const std::string& dir) {
    auto parsed = parseDosFilename(dir == ":_" ? ":.." : dir, true);   // CD:← is CD:..
    if (!parsed) { setError(0x32, 0, 0); return; }
    auto resolved = resolvePathIso(*parsed, true, WILDCARD_DIR);
    if (!resolved) return;                          // error already set
    struct stat st;
    if (::stat(resolved->c_str(), &st)) setError(0x62, 0, 0);
    else if (!S_ISDIR(st.st_mode)) setError(0x39, 0, 0);
    else m_cwd = *resolved;
}

void CbmHostDos::cmkdir(const std::string& dir) {
    auto parsed = parseDosFilename(dir, true);
    if (!parsed) { setError(0x32, 0, 0); return; }
    clearError();
    auto resolved = resolvePathIso(*parsed, false, WILDCARD_DIR);
    if (!resolved) return;
    if (::mkdir(resolved->c_str(), 0777)) setError(errno == EEXIST ? 0x63 : 0x62, 0, 0);
}

// "R:NEW=OLD" or "RENAME:NEW=OLD"
void CbmHostDos::crename(const std::string& f) {
    size_t colon = f.find(':');
    if (colon == std::string::npos) { setError(0x34, 0, 0); return; }
    std::string rest = f.substr(colon + 1);
    size_t eq = rest.find('=');
    if (eq == std::string::npos) { setError(0x34, 0, 0); return; }
    std::string d = rest.substr(0, eq), s = rest.substr(eq + 1);

    clearError();
    auto src = resolvePathIso(s, true, WILDCARD_ALL);
    if (!src) { setError(0x62, 0, 0); return; }
    auto dst = resolvePathIso(d, false, WILDCARD_ALL);
    if (!dst) { setError(0x39, 0, 0); return; }
    struct stat st;
    if (::stat(dst->c_str(), &st) == 0) {
        setError(0x63, 0, 0);                       // the new name exists
    } else if (std::rename(src->c_str(), dst->c_str())) {
        if (errno == EACCES) setError(0x63, 0, 0);
        else if (errno == EINVAL) setError(0x33, 0, 0);
        else setError(0x62, 0, 0);
    }
}

void CbmHostDos::crmdir(const std::string& dir) {
    auto parsed = parseDosFilename(dir, true);
    if (!parsed) { setError(0x32, 0, 0); return; }
    clearError();
    auto resolved = resolvePathIso(*parsed, true, WILDCARD_DIR);
    if (!resolved) { setError(0x39, 0, 0); return; }
    if (::rmdir(resolved->c_str()))
        setError((errno == ENOTEMPTY || errno == EACCES) ? 0x63 : 0x62, 0, 0);
}

// "S:FILENAME" or "SCRATCH:FILENAME"
void CbmHostDos::cunlink(const std::string& f) {
    size_t colon = f.find(':');
    if (colon == std::string::npos) { setError(0x34, 0, 0); return; }
    clearError();
    auto resolved = resolvePathIso(f.substr(colon + 1), true, WILDCARD_PRG);
    if (!resolved) { setError(0x62, 0, 0); return; }
    if (::unlink(resolved->c_str())) setError(errno == EACCES ? 0x63 : 0x62, 0, 0);
    else setError(0x01, 0, 0);                      // 1 file scratched
}

void CbmHostDos::cseek(int channel, uint32_t pos) {
    if (channel == 15) { setError(0x30, 0, 0); return; }
    channel &= 15;
    if (m_channels[channel].f) std::fseek(m_channels[channel].f, (long)pos, SEEK_SET);
    else setError(0x70, 0, 0);
}

void CbmHostDos::ctell(int channel) {
    if (channel == 15) { setError(0x30, 0, 0); return; }
    channel &= 15;
    std::FILE* f = m_channels[channel].f;
    if (!f) { setError(0x70, 0, 0); return; }
    long cur = std::ftell(f);
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, cur, SEEK_SET);
    uint64_t pos = cur < 0 ? 0 : (uint64_t)cur, siz = size < 0 ? 0 : (uint64_t)size;
    if (pos > 0xffffffffULL) pos = 0xffffffffULL;
    if (siz > 0xffffffffULL) siz = 0xffffffffULL;
    char buf[32];
    std::snprintf(buf, sizeof buf, "%08X %08X", (uint32_t)pos, (uint32_t)siz);
    setErrorText(0x07, buf, 0, 0);
}

// ---------------------------------------------------------------------------
// Channels
// ---------------------------------------------------------------------------

int CbmHostDos::copen(int channel) {
    Channel& ch = m_channels[channel];
    if (channel == 15) {
        command(ch.name);
        return -1;
    }

    int ret = -1;
    // a ",P,W"-like suffix: read, write, append or modify
    bool append = false;
    ch.read = true;
    ch.write = false;
    size_t first = ch.name.find(',');
    if (first != std::string::npos) {
        std::string suffix = ch.name.substr(first + 1);
        ch.name.resize(first);
        size_t second = suffix.find(',');
        if (second != std::string::npos) {
            switch (at(suffix, second + 1)) {
                case 'A':
                    append = true;
                    [[fallthrough]];
                case 'W':
                    ch.read = false;
                    ch.write = true;
                    break;
                case 'M':
                    ch.read = true;
                    ch.write = true;
                    break;
            }
        }
    }
    if (channel <= 1) {                             // channels 0 and 1 are magic
        ch.write = channel;
        ch.read = !channel;
    }
    logger()->debug("OPEN \"{}\",{} ({}{})", ch.name, channel, ch.read ? "R" : "", ch.write ? "W" : "");

    if (ch.name.empty() || !ch.name[0]) {           // empty file name
        setError(0x34, 0, 0);
        return -2;
    }

    if (!ch.write && ch.name[0] == '$') {
        m_dirlistPos = 0;
        if (ch.name.compare(0, 3, "$=C") == 0) m_dirlist = createCwdListing();
        else m_dirlist = createDirectoryListing(ch.name);
    } else {
        auto parsed = parseDosFilename(ch.name, false);
        if (!parsed) {
            setError(0x32, 0, 0);
            return -2;
        }
        auto resolved = resolvePathIso(*parsed, false, WILDCARD_PRG);
        if (!resolved) return -2;                   // error already set
        if (m_prgFallback && ch.read && !ch.write && ::access(resolved->c_str(), F_OK) != 0 &&
            parsed->find('.') == std::string::npos) {
            auto prg = resolvePathIso(*parsed + ".prg", false, WILDCARD_PRG);
            if (prg && ::access(prg->c_str(), F_OK) == 0) resolved = prg;
        }

        if (m_pathExists && !m_overwrite && !append && !ch.read) {
            setError(0x63, 0, 0);                   // overwrite only when asked
            return -1;
        }

        const char* path = resolved->c_str();
        if (append) ch.f = std::fopen(path, "ab+");
        else if (ch.read && ch.write) ch.f = std::fopen(path, ::access(path, F_OK) == 0 ? "rb+" : "wb+");
        else ch.f = std::fopen(path, ch.write ? "wb" : "rb");

        if (!ch.f) {
            logger()->debug("FILE NOT FOUND");
            setError(0x62, 0, 0);
            ret = -2;
        } else {
            clearError();
        }
    }
    return ret;
}

void CbmHostDos::cclose(int channel) {
    Channel& ch = m_channels[channel];
    ch.name.clear();
    if (ch.f) {
        std::fclose(ch.f);
        ch.f = nullptr;
    }
}

// ---------------------------------------------------------------------------
// The KERNAL IEEE API
// ---------------------------------------------------------------------------

int CbmHostDos::second(uint8_t a) {
    if (!m_listening) return -2;
    int ret = -1;
    m_channel = a & 0xf;
    m_opening = false;
    if (m_channel == 15) ret = 0;
    switch (a & 0xf0) {
        case 0x60:                                  // write
            break;
        case 0xe0:
            cclose(m_channel);
            break;
        case 0xf0:                                  // open: the name follows
            m_opening = true;
            m_channels[m_channel].name.clear();
            break;
    }
    return ret;
}

int CbmHostDos::tksa(uint8_t a) {
    if (!m_talking) return -2;
    m_channel = a & 0xf;
    return -1;
}

int CbmHostDos::acptr(uint8_t* a) {
    if (!m_talking) return -2;
    int ret = 0;
    Channel& ch = m_channels[m_channel];
    if (m_channel == 15) {
        *a = m_error[m_errorPos++];
        if (m_errorPos >= m_errorLen) {
            clearError();
            ret = 0x40;                             // EOI
        }
    } else if (ch.read) {
        if (!ch.name.empty() && ch.name[0] == '$') {
            *a = m_dirlistPos < m_dirlist.size() ? m_dirlist[m_dirlistPos++] : 0;
            if (m_dirlistPos == m_dirlist.size()) {
                if (m_dirlistEof) {
                    ret = 0x40;
                } else {
                    m_dirlistPos = 0;
                    m_dirlist = continueDirectoryListing();
                }
            }
        } else if (ch.f) {
            int c = std::fgetc(ch.f);
            if (c == EOF) {
                ret = 0x42;
                *a = 0;
            } else {
                *a = (uint8_t)c;
                // EOI with the last byte (checked each time: files can be
                // read and written at random positions)
                long cur = std::ftell(ch.f);
                std::fseek(ch.f, 0, SEEK_END);
                if (cur == std::ftell(ch.f)) {
                    ret = 0x40;
                    ch.read = false;
                    cclose(m_channel);
                } else {
                    std::fseek(ch.f, cur, SEEK_SET);
                }
            }
        } else {
            ret = 0x42;
        }
    } else {
        ret = 0x42;                                 // file not found
    }
    return ret;
}

int CbmHostDos::ciout(uint8_t a) {
    if (!m_listening) return -2;
    int ret = 0;
    Channel& ch = m_channels[m_channel];
    if (m_opening) {
        if (ch.name.size() < CHANNEL_NAME_MAX) ch.name += (char)a;
    } else if (m_channel == 15) {
        // P and T take binary parameters: no CR termination for them
        // A CR ends a command, except in binary ones (P, T, U0 burst)
        if (a == 13 && at(m_cmd, 0) != 'P' && at(m_cmd, 0) != 'T' &&
            !(at(m_cmd, 0) == 'U' && at(m_cmd, 1) == '0' && m_cmd.size() > 2 && at(m_cmd, 2) != '>')) {
            command(m_cmd);
            m_cmd.clear();
        } else if (m_cmd.size() < CMD_MAX) {
            m_cmd += (char)a;
        }
    } else if (ch.write && ch.f) {
        if (std::fputc(a, ch.f) == EOF) ret = 0x40;
    } else {
        ret = 2;                                    // file not found
    }
    return ret;
}

int CbmHostDos::untlk() {
    if (!m_talking) return -2;
    m_talking = false;
    setActivity(false);
    return -1;
}

int CbmHostDos::unlsn() {
    if (!m_listening) return -2;
    m_listening = false;
    setActivity(false);
    if (m_opening) {
        m_opening = false;
        copen(m_channel);
    } else if (m_channel == 15) {
        command(m_cmd);
        m_cmd.clear();
    }
    return -1;
}

int CbmHostDos::listen(uint8_t a) {
    if ((a & 0x1f) != m_unit) return -2;            // not us
    m_listening = true;
    setActivity(true);
    return -1;
}

int CbmHostDos::talk(uint8_t a) {
    if ((a & 0x1f) != m_unit) return -2;
    m_talking = true;
    setActivity(true);
    return -1;
}

