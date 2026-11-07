#include "cbm_image_dos.h"
#include "plugins/cbm-loader/main/d64_parser.h"
#include "plugins/cbm-loader/main/d71_parser.h"
#include "plugins/cbm-loader/main/d80_parser.h"
#include "plugins/cbm-loader/main/d81_parser.h"
#include "plugins/cbm-loader/main/d82_parser.h"
#include "imd_disk.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

CbmImageDos::CbmImageDos() { setError(0, " OK"); }
CbmImageDos::~CbmImageDos() = default;

bool CbmImageDos::open(const std::string& path) {
    std::string ext = path.size() > 4 ? path.substr(path.size() - 3) : "";
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    if (ext == "imd") {                          // an MFM disk
        auto m = std::make_unique<ImdDisk>();
        if (!m->open(path)) return false;
        m_mfm = std::move(m);
        m_disk.reset();
        m_path = path;
        reset();
        return true;
    }
    std::unique_ptr<CbmSectorDisk> d;
    if      (ext == "d64") d.reset(new D64Parser());
    else if (ext == "d71") d.reset(new D71Parser());
    else if (ext == "d80") d.reset(new D80Parser());
    else if (ext == "d81") d.reset(new D81Parser());
    else if (ext == "d82") d.reset(new D82Parser());
    else return false;
    if (!d->open(path)) return false;
    m_disk = std::move(d);
    m_mfm.reset();
    m_path = path;
    reset();
    return true;
}

void CbmImageDos::reset() {
    for (auto& c : m_ch) c = Channel{};
    m_listening = m_talking = m_opening = m_active = false;
    m_name.clear();
    m_cmd.clear();
    m_bwLeft = 0;
    m_burstLast = 0x10;
    m_loggedIn = false;
    m_burstInterleave = 5;
    setError(0, " OK");
}

void CbmImageDos::setError(int code, const char* text, int track, int sector) {
    // As the drives send them: "62,FILE NOT FOUND,00,00" ("00, OK,00,00")
    char buf[64];
    std::snprintf(buf, sizeof buf, "%02d,%s,%02d,%02d\r", code, text, track, sector);
    m_error = buf;
    m_errorPos = 0;
}

bool CbmImageDos::matches(const std::string& pattern, const std::string& name) {
    for (size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] == '*') return true;
        if (i >= name.size()) return false;
        if (pattern[i] != '?' && pattern[i] != name[i]) return false;
    }
    return pattern.size() == name.size();
}

// ---------------------------------------------------------------------------
// The directory
// ---------------------------------------------------------------------------

std::vector<uint8_t> CbmImageDos::directory(const std::string& pattern) {
    std::vector<uint8_t> out;
    uint16_t addr = 0x0401;                     // the PET's BASIC start, as the drives send it
    out.push_back((uint8_t)addr);
    out.push_back((uint8_t)(addr >> 8));
    // One BASIC line: link to the next, line number, text, 0
    auto line = [&](uint16_t number, const std::string& text) {
        uint16_t next = (uint16_t)(addr + 2 + 2 + text.size() + 1);
        out.push_back((uint8_t)next);
        out.push_back((uint8_t)(next >> 8));
        out.push_back((uint8_t)number);
        out.push_back((uint8_t)(number >> 8));
        out.insert(out.end(), text.begin(), text.end());
        out.push_back(0);
        addr = next;
    };
    if (!m_disk) return out;
    std::string name = m_disk->getDiskName();
    name.resize(16, ' ');
    line(0, std::string("\x12\"") + name + "\" " + m_disk->getDiskId() + " " + m_disk->getDosType());

    static const char* types[] = {"DEL", "SEQ", "PRG", "USR", "REL"};
    for (const auto& e : m_disk->getDirectory()) {
        if (!pattern.empty() && !matches(pattern, e.filename)) continue;
        std::string text;
        if (e.sizeBlocks < 10) text += "   ";
        else if (e.sizeBlocks < 100) text += "  ";
        else if (e.sizeBlocks < 1000) text += " ";
        text += "\"" + e.filename + "\"";
        text.append(16 - std::min<size_t>(16, e.filename.size()), ' ');
        text += (e.type & 0x80) ? ' ' : '*';                     // not closed: a splat file
        int t = e.type & 0x07;
        text += t < 5 ? types[t] : "???";
        if (e.type & 0x40) text += '<';                          // locked
        line(e.sizeBlocks, text);
    }
    line(m_disk->getFreeBlocks(), "BLOCKS FREE.             ");
    out.push_back(0);
    out.push_back(0);
    return out;
}

// ---------------------------------------------------------------------------
// Channels
// ---------------------------------------------------------------------------

void CbmImageDos::openChannel(int ch, const std::string& spec) {
    Channel& c = m_ch[ch];
    c = Channel{};
    if (ch == 15) {                             // OPEN 15,8,15,"command"
        if (!spec.empty()) command(spec);
        return;
    }
    if (!m_disk && (spec.empty() || spec[0] != '#')) {   // an MFM disk: no Commodore files
        setError(74, "DRIVE NOT READY");
        return;
    }
    if (!spec.empty() && spec[0] == '#') {      // a direct-access buffer ("#", "#n")
        c.open = true;
        c.buffer = true;
        c.ptr = 1;
        c.last = 255;
        setError(0, " OK");
        return;
    }
    // "0:NAME,S,R" -> drive prefix off, then the name and its options
    std::string s = spec;
    if (s.size() >= 2 && s[1] == ':' && std::isdigit((unsigned char)s[0])) s = s.substr(2);
    else if (!s.empty() && s[0] == ':') s = s.substr(1);
    if (!s.empty() && s[0] == '$') {            // "$", "$0", "$0:PAT*"
        std::string pat;
        size_t colon = s.find(':');
        if (colon != std::string::npos) pat = s.substr(colon + 1);
        c.data = directory(pat);
        c.open = true;
        setError(0, " OK");
        return;
    }
    std::string name = s, opts;
    size_t comma = s.find(',');
    if (comma != std::string::npos) { name = s.substr(0, comma); opts = s.substr(comma); }
    bool append = opts.find(",A") != std::string::npos;
    bool write = ch == 1 || opts.find(",W") != std::string::npos || append || (!name.empty() && name[0] == '@');
    if (write) {                                // SAVE (channel 1) or OPEN for writing
        if (!m_disk->writable()) { setError(26, "WRITE PROTECT ON"); return; }
        bool replace = false;
        if (!name.empty() && name[0] == '@') {  // "@0:NAME" / "@:NAME"
            replace = true;
            name = name.substr(1);
            if (name.size() >= 2 && name[1] == ':' && std::isdigit((unsigned char)name[0])) name = name.substr(2);
            else if (!name.empty() && name[0] == ':') name = name.substr(1);
        }
        int type = ch == 1 ? 2 : 1;             // SAVE: PRG; OPEN default: SEQ
        if (opts.find(",P") != std::string::npos) type = 2;
        if (opts.find(",U") != std::string::npos) type = 3;
        if (opts.find(",S") != std::string::npos) type = 1;
        c.open = true;
        c.writing = true;
        c.replace = replace || append;
        c.wname = name;
        c.wtype = type;
        if (append) {
            if (!m_disk->readFile(name, c.data)) { c = Channel{}; setError(62, "FILE NOT FOUND"); return; }
        } else {
            for (const auto& e : m_disk->getDirectory())
                if (e.filename == name && (e.type & 0x07) && !replace) {
                    c = Channel{};
                    setError(63, "FILE EXISTS");
                    return;
                }
        }
        setError(0, " OK");
        return;
    }
    for (const auto& e : m_disk->getDirectory()) {
        if (!matches(name, e.filename) || (e.type & 0x07) == 0) continue;
        if (!m_disk->readFile(e.filename, c.data)) break;
        c.open = true;
        setError(0, " OK");
        return;
    }
    setError(62, "FILE NOT FOUND");
}

void CbmImageDos::command(const std::string& raw) {
    if (raw.size() > 2 && raw[0] == 'U' && raw[1] == '0') { burst(raw.substr(2)); return; }
    std::string cmd = raw;
    while (!cmd.empty() && (cmd.back() == '\r' || cmd.back() == '\n')) cmd.pop_back();
    if (cmd.empty()) return;
    char c = cmd[0];
    if (c == 'I' || cmd.rfind("UJ", 0) == 0 || cmd.rfind("UI", 0) == 0 || cmd.rfind("U9", 0) == 0) {
        setError(0, " OK");                     // initialise / reset
    } else if (!m_disk) {                       // an MFM disk: no Commodore DOS on it
        setError(74, "DRIVE NOT READY");
    } else if (cmd.rfind("U1", 0) == 0 || cmd.rfind("UA", 0) == 0) {
        blockRead(cmd.substr(2), true);
    } else if (cmd.rfind("B-R", 0) == 0) {
        blockRead(cmd.substr(3), false);
    } else if (cmd.rfind("B-P", 0) == 0) {
        bufferPointer(cmd.substr(3));
    } else if (cmd.rfind("U2", 0) == 0 || cmd.rfind("UB", 0) == 0) {
        blockWrite(cmd.substr(2), true);
    } else if (cmd.rfind("B-W", 0) == 0) {
        blockWrite(cmd.substr(3), false);
    } else if ((c == 'S' || c == 'R') && cmd.find(':') != std::string::npos && cmd.rfind("S-", 0) != 0) {
        std::string arg = cmd.substr(cmd.find(':') + 1);
        if (!m_disk->writable()) { setError(26, "WRITE PROTECT ON"); return; }
        if (c == 'S') {                         // S0:PAT1,PAT2...
            int total = 0;
            size_t start = 0;
            while (start <= arg.size()) {
                size_t comma = arg.find(',', start);
                std::string pat = arg.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                if (pat.size() >= 2 && pat[1] == ':' && std::isdigit((unsigned char)pat[0])) pat = pat.substr(2);
                int n = 0;
                if (!pat.empty()) m_disk->scratch(pat, n);
                total += n;
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
            dosResult(1, total);
        } else {                                // R0:NEW=OLD
            size_t eq = arg.find('=');
            if (eq == std::string::npos) { setError(30, "SYNTAX ERROR"); return; }
            std::string from = arg.substr(eq + 1);
            if (from.size() >= 2 && from[1] == ':' && std::isdigit((unsigned char)from[0])) from = from.substr(2);
            dosResult(m_disk->rename(arg.substr(0, eq), from));
        }
    } else if (c == 'N' || c == 'C' || c == 'V' ||
               cmd.rfind("M-W", 0) == 0) {
        setError(26, "WRITE PROTECT ON");       // would change the disk
    } else {
        setError(31, "SYNTAX ERROR");
    }
}

// ---------------------------------------------------------------------------
// Direct access (1541 DOS: U1 reads the whole block with the pointer at 0;
// B-R takes the byte count from the block's first byte, pointer at 1)
// ---------------------------------------------------------------------------

namespace {
// "ch dr t s" with spaces, commas or a colon between the numbers
std::vector<int> numbers(const std::string& s) {
    std::vector<int> v;
    int cur = -1;
    for (char ch : s) {
        if (std::isdigit((unsigned char)ch)) cur = (cur < 0 ? 0 : cur * 10) + (ch - '0');
        else if (cur >= 0) { v.push_back(cur); cur = -1; }
    }
    if (cur >= 0) v.push_back(cur);
    return v;
}
}  // namespace

void CbmImageDos::blockRead(const std::string& args, bool u1) {
    auto n = numbers(args);
    if (n.size() < 4) { setError(30, "SYNTAX ERROR"); return; }
    int ch = n[0], t = n[2], sec = n[3];
    if (ch < 0 || ch > 14 || !m_ch[ch].buffer) { setError(70, "NO CHANNEL"); return; }
    uint32_t off = m_disk->sectorOffset(t, sec);
    if (off == 0xFFFFFFFFu || off + 256 > m_disk->data().size()) {
        setError(66, "ILLEGAL TRACK OR SECTOR", t, sec);
        return;
    }
    Channel& c = m_ch[ch];
    std::copy(m_disk->data().begin() + off, m_disk->data().begin() + off + 256, c.buf);
    if (u1) { c.ptr = 0; c.last = 255; }
    else { c.ptr = 1; c.last = c.buf[0] ? c.buf[0] : 255; }
    setError(0, " OK");
}

// U2 writes the buffer as it is; B-W first stores the pointer (the byte
// count) in the buffer's first byte
void CbmImageDos::blockWrite(const std::string& args, bool u2) {
    auto n = numbers(args);
    if (n.size() < 4) { setError(30, "SYNTAX ERROR"); return; }
    int ch = n[0], t = n[2], sec = n[3];
    if (ch < 0 || ch > 14 || !m_ch[ch].buffer) { setError(70, "NO CHANNEL"); return; }
    Channel& c = m_ch[ch];
    if (m_disk->sectorOffset(t, sec) == 0xFFFFFFFFu) { setError(66, "ILLEGAL TRACK OR SECTOR", t, sec); return; }
    if (!m_disk->writable()) { setError(26, "WRITE PROTECT ON", t, sec); return; }
    if (!u2) c.buf[0] = (uint8_t)c.ptr;
    if (!m_disk->writeSector(t, sec, c.buf)) { setError(25, "WRITE ERROR", t, sec); return; }
    setError(0, " OK");
}

void CbmImageDos::dosResult(int code, int track) {
    switch (code) {
        case 0: setError(0, " OK"); break;
        case 1: setError(1, "FILES SCRATCHED", track); break;
        case 25: setError(25, "WRITE ERROR"); break;
        case 26: setError(26, "WRITE PROTECT ON"); break;
        case 33: setError(33, "SYNTAX ERROR"); break;
        case 62: setError(62, "FILE NOT FOUND"); break;
        case 63: setError(63, "FILE EXISTS"); break;
        case 72: setError(72, "DISK FULL"); break;
        default: setError(code, "ERROR"); break;
    }
}

// CLOSE: a file being written goes to the disk
void CbmImageDos::closeChannel(int ch) {
    Channel& c = m_ch[ch];
    if (c.writing) dosResult(m_disk->writeFile(c.wname, c.wtype, c.data, c.replace));
    c = Channel{};
}

// ---------------------------------------------------------------------------
// Burst commands (Commodore's burst command instruction set, as the 1571
// answers them on fast serial; byte 0: bits 1-3 the command, bit 0 the drive)
// ---------------------------------------------------------------------------

// The status byte: bit 7 foreign format, bits 5-4 sector size (01 = 256
// bytes), bits 3-0 the controller status (0 OK)
uint8_t CbmImageDos::burstStatus(int controller) const {
    return (uint8_t)(0x10 | (controller & 0x0F));
}

// The next sector of a multi-sector burst on a GCR track: the soft
// interleave on (5 by default), round the track: the order of the C128's
// CP/M (its Z80 ROM's tables at $0FB5: 0 5 10 15 20 4 9 ... on a 21-sector
// track)
int CbmImageDos::nextBurstSector(int track, int sector) const {
    int spt = 0;
    while (spt < 64 && m_disk->sectorOffset(track, spt) != 0xFFFFFFFFu) ++spt;
    return spt ? (sector + m_burstInterleave) % spt : 0;
}

// Byte 0 (the 1571 User's Guide, Appendix G): bits 3-1 the command, bit 0
// the drive number; the switches above
void CbmImageDos::burst(const std::string& b) {
    m_burstOut.clear();
    m_bwLeft = 0;
    m_bwSize = 256;
    if (b.empty()) { m_burstOut.push_back(burstStatus(0x0E)); return; }
    // READ and WRITE need the disk logged in (INQUIRE DISK, QUERY DISK
    // FORMAT) since it was put in: until then, disk change ($B)
    if (((b[0] >> 1) & 7) <= 1 && !m_loggedIn) {
        const uint8_t st = m_mfm ? mfmStatus(0, 0, 0x0B) : burstStatus(0x0B);
        m_burstOut.push_back(m_burstLast = st);
        setError(0, " OK");
        return;
    }
    if (m_mfm && mfmBurst(b)) { setError(0, " OK"); return; }
    const uint8_t c = (uint8_t)b[0];
    auto at = [&](size_t i, int def) { return i < b.size() ? (int)(uint8_t)b[i] : def; };
    switch ((c >> 1) & 7) {
        case 0: {                                // READ: T E B S 0 0 0 N, track, sector, count
            const bool noTransfer = c & 0x80;
            int t = at(1, 1), s = at(2, 0), n = at(3, 1);
            for (int i = 0; i < n; ++i, s = nextBurstSector(t, s)) {
                uint32_t off = m_disk->sectorOffset(t, s);
                if (off == 0xFFFFFFFFu || off + 256 > m_disk->data().size()) {
                    m_burstOut.push_back(m_burstLast = burstStatus(0x02));   // sector not found
                    break;
                }
                m_burstOut.push_back(m_burstLast = burstStatus(0));
                if (!noTransfer)
                    m_burstOut.insert(m_burstOut.end(), m_disk->data().begin() + off, m_disk->data().begin() + off + 256);
            }
            break;
        }
        case 1:                                  // WRITE: the host sends each sector
            m_bwTrack = at(1, 1);
            m_bwSector = at(2, 0);
            m_bwLeft = at(3, 1);
            m_bwBuf.clear();
            break;
        case 2:                                  // INQUIRE DISK: logs it in
            m_loggedIn = true;
            m_burstOut.push_back(m_burstLast = burstStatus(0));
            break;
        case 3:                                  // FORMAT: byte 1 bit 7 M (1 MFM, 0 GCR)
            // GCR (no directory): every sector as the 1571 formats it; no
            // output, the status is kept for INQUIRE STATUS. MFM cannot go
            // on a GCR image: format error.
            if (at(1, 0) & 0x80) m_burstLast = burstStatus(0x06);
            else if (!m_disk->writable()) m_burstLast = burstStatus(0x08);   // write protect
            else m_burstLast = burstStatus(m_disk->formatSectors() ? 0 : 0x06);
            m_loggedIn = false;                  // to be logged in again
            break;
        case 4:                                  // SECTOR INTERLEAVE: W X X 0 0 1 0 0 N
            if (c & 0x80) m_burstOut.push_back((uint8_t)m_burstInterleave);
            else m_burstInterleave = at(1, m_burstInterleave);
            break;
        case 5:                                  // QUERY DISK FORMAT: a GCR disk answers its status only
            m_loggedIn = true;
            m_burstOut.push_back(m_burstLast = burstStatus(0));
            break;
        case 6:                                  // INQUIRE STATUS: W C X 0 1 1 0 N
            if (!(c & 0x80)) {                   // write: the new status; C logs the disk in
                if (b.size() > 1) m_burstLast = (uint8_t)b[1];
                if (c & 0x40) m_loggedIn = true;
            } else if ((c & 0x40) && !m_loggedIn) {
                m_burstOut.push_back(burstStatus(0x0B));      // disk change: not logged in
            } else {
                m_burstOut.push_back(m_burstLast);
            }
            break;
        case 7:
            if ((c & 0x1F) == 0x1F) {            // FASTLOAD: P X X 1 1 1 1 1 + the file name
                m_fastload = b.substr(1);
                m_fastloadPending = true;
            } else if (c & 0x10) {               // CHGUTL: X X X 1 1 1 1 0 ('>' + the utility)
                burstUtility(b.substr(1));
                return;
            } else {                             // BACKUP DISK: undocumented
                m_burstOut.push_back(burstStatus(0x0E));
            }
            break;
    }
    setError(0, " OK");
}

// The CHGUTL utilities ("U0>S" + interleave, "U0>R" + retries, "U0>T" ROM
// test, "U0>M0/1" 1541/1571 mode, "U0>H0/1" head, "U0>" + device number):
// the image has nothing to change but the device number
void CbmImageDos::burstUtility(const std::string& a) {
    if (a.empty()) { setError(31, "SYNTAX ERROR"); return; }
    const uint8_t u = (uint8_t)a[0];
    if (u == 'S' || u == 'R' || u == 'T' || u == 'M' || u == 'H') {
        setError(0, " OK");
    } else if (u >= 4 && u <= 30) {
        m_unit = u;
        setError(0, " OK");
    } else {
        setError(31, "SYNTAX ERROR");
    }
}

// MFM disks (an .imd): the 1571 User's Guide, Appendix G, as the C128's
// CP/M uses them (cxdisk.asm, fast8502.asm): READ/WRITE `U0` $00/$02 (+$10
// side 1), the cylinder (from 0), the sector number as in its header, the
// count; QUERY DISK FORMAT `U0` $8A + the track to look at. The status
// byte: bit 7 MFM, bits 5-4 the sector size, bits 3-0 the controller
// status (2 sector not found, 3 no address mark, 5 data CRC, 6 format,
// 8 write protect).
uint8_t CbmImageDos::mfmStatus(int cyl, int side, int controller) {
    ImdDisk::Track* t = m_mfm->track(cyl, side);
    if (!t && !controller) controller = 0x03;                    // nothing there: no address mark
    return (uint8_t)(0x80 | ((t ? t->sizeCode & 3 : 1) << 4) | (controller & 0x0F));
}

bool CbmImageDos::mfmBurst(const std::string& b) {
    const uint8_t c = (uint8_t)b[0];
    auto at = [&](size_t i, int def) { return i < b.size() ? (int)(uint8_t)b[i] : def; };
    const int side = (c >> 4) & 1;
    switch ((c >> 1) & 7) {
        case 0: {                                // READ: T E B S 0 0 0 N, cylinder, sector, count
            const bool noTransfer = c & 0x80, ignoreError = c & 0x40;
            int t = at(1, 0), s = at(2, 1), n = at(3, 1);
            for (int i = 0; i < n; ++i, ++s) {
                ImdDisk::Sector* sec = m_mfm->sector(t, side, s);
                if (!sec || sec->type == 0) {
                    m_burstOut.push_back(m_burstLast = mfmStatus(t, side, m_mfm->track(t, side) ? 0x02 : 0x03));
                    break;
                }
                const bool crc = sec->type >= 5;
                m_burstOut.push_back(m_burstLast = mfmStatus(t, side, crc ? 0x05 : 0));
                if (crc && !ignoreError) break;
                if (!noTransfer) m_burstOut.insert(m_burstOut.end(), sec->data.begin(), sec->data.end());
            }
            return true;
        }
        case 1: {                                // WRITE: the host sends each sector
            m_bwTrack = at(1, 0);
            m_bwSector = at(2, 1);
            m_bwLeft = at(3, 1);
            m_bwSide = side;
            const int size = m_mfm->sectorSize(m_bwTrack, side);
            m_bwSize = size ? (size_t)size : 256;
            m_bwBuf.clear();
            return true;
        }
        case 2:                                  // INQUIRE DISK: logs it in
            m_loggedIn = true;
            m_burstOut.push_back(m_burstLast = mfmStatus(0, side, 0));
            return true;
        case 3: {                                // FORMAT: byte 1 bit 7 M (1 MFM), bit 6 T (a sector table)
            const int b1 = at(1, 0x80);
            if (!(b1 & 0x80)) { m_burstLast = mfmStatus(0, side, 0x06); return true; }   // GCR on an MFM image
            if (!m_mfm->writable()) { m_burstLast = mfmStatus(0, side, 0x08); return true; }
            const int sizeCode = at(3, 1) & 3;
            static const int DEFSECTORS[4] = {26, 16, 9, 5};
            std::vector<uint8_t> table;
            const int sectors = at(5, DEFSECTORS[sizeCode]);
            if (b1 & 0x40)
                for (int i = 0; i < sectors && 9 + (size_t)i < b.size(); ++i) table.push_back((uint8_t)b[9 + (size_t)i]);
            const bool ok = m_mfm->format(at(4, 39), sectors, sizeCode, b1 & 0x3F, at(2, 0), at(6, 0), at(7, 0),
                                          (uint8_t)at(8, 0xE5), c & 0x20, side, table);
            m_burstLast = mfmStatus(0, side, ok ? 0 : 0x06);
            m_loggedIn = false;
            return true;
        }
        case 5: {                                // QUERY DISK FORMAT: F X X S 1 0 1 N, the track (F)
            m_loggedIn = true;
            const int cyl = (c & 0x80) ? at(1, 0) : 0;
            const uint8_t st0 = mfmStatus(0, side, 0);
            m_burstOut.push_back(m_burstLast = st0);
            if (st0 & 0x0E) return true;
            ImdDisk::Track* t = m_mfm->track(cyl, side);
            const uint8_t st = mfmStatus(cyl, side, (t && !t->sectors.empty()) ? 0 : 0x03);
            m_burstOut.push_back(m_burstLast = st);
            if (st & 0x0E) return true;
            // sectors, the track number in the headers, the lowest and highest
            // sector numbers, the interleave (physical steps from one sector
            // number to the next)
            int lo = 255, hi = 0, plo = 0, pnext = -1;
            for (size_t i = 0; i < t->sectors.size(); ++i) {
                if (t->sectors[i].id < lo) { lo = t->sectors[i].id; plo = (int)i; }
                hi = std::max<int>(hi, t->sectors[i].id);
            }
            for (size_t i = 0; i < t->sectors.size(); ++i)
                if (t->sectors[i].id == lo + 1) pnext = (int)i;
            const int n = (int)t->sectors.size();
            const int interleave = pnext < 0 ? 1 : (pnext - plo + n) % n;
            m_burstOut.push_back((uint8_t)n);
            m_burstOut.push_back(t->sectors[0].cyl);
            m_burstOut.push_back((uint8_t)lo);
            m_burstOut.push_back((uint8_t)hi);
            m_burstOut.push_back((uint8_t)interleave);
            return true;
        }
        default:
            return false;                        // interleave, status, utilities: as on GCR
    }
}

bool CbmImageDos::burstWriteByte(uint8_t b) {
    if (m_bwLeft <= 0) return false;
    m_bwBuf.push_back(b);
    return m_bwBuf.size() >= m_bwSize;
}

void CbmImageDos::burstFinishSector() {
    if (m_bwLeft <= 0) return;
    if (m_mfm) {                                // an MFM sector, by its number on the cylinder and side
        int status = 0;
        ImdDisk::Sector* sec = m_mfm->sector(m_bwTrack, m_bwSide, m_bwSector);
        if (!sec || m_bwBuf.size() < m_bwSize) status = 0x02;
        else if (!m_mfm->writable()) status = 0x08;
        else {
            sec->data = m_bwBuf;
            if (sec->type == 0) sec->type = 1;
            if (!m_mfm->save()) status = 0x07;
        }
        m_burstOut.push_back(m_burstLast = mfmStatus(m_bwTrack, m_bwSide, status));
        m_bwBuf.clear();
        ++m_bwSector;
        if (--m_bwLeft < 0 || status) m_bwLeft = 0;
        return;
    }
    int status = 0;
    if (m_bwBuf.size() < 256 || m_disk->sectorOffset(m_bwTrack, m_bwSector) == 0xFFFFFFFFu) status = 0x02;
    else if (!m_disk->writable()) status = 0x08;                   // write protect on
    else if (!m_disk->writeSector(m_bwTrack, m_bwSector, m_bwBuf.data())) status = 0x07;
    m_burstOut.push_back(m_burstLast = burstStatus(status));
    m_bwBuf.clear();
    m_bwSector = nextBurstSector(m_bwTrack, m_bwSector);
    if (--m_bwLeft < 0 || status) m_bwLeft = 0;
}

void CbmImageDos::bufferPointer(const std::string& args) {
    auto n = numbers(args);
    if (n.size() < 2) { setError(30, "SYNTAX ERROR"); return; }
    int ch = n[0];
    if (ch < 0 || ch > 14 || !m_ch[ch].buffer) { setError(70, "NO CHANNEL"); return; }
    m_ch[ch].ptr = n[1] & 0xFF;
    m_ch[ch].last = 255;
    setError(0, " OK");
}

// ---------------------------------------------------------------------------
// The bus calls
// ---------------------------------------------------------------------------

int CbmImageDos::listen(uint8_t a) {
    if ((a & 0x1F) != m_unit) return -2;
    m_listening = true;
    m_active = true;
    return -1;
}

int CbmImageDos::talk(uint8_t a) {
    if ((a & 0x1F) != m_unit) return -2;
    m_talking = true;
    m_active = true;
    return -1;
}

int CbmImageDos::second(uint8_t a) {
    if (!m_listening) return -2;
    m_channel = a & 0x0F;
    m_opening = false;
    switch (a & 0xF0) {
        case 0xE0: closeChannel(m_channel); break;             // CLOSE
        case 0xF0: m_opening = true; m_name.clear(); break;     // OPEN: the name follows
        default: break;                                         // $6x: data
    }
    return -1;
}

int CbmImageDos::tksa(uint8_t a) {
    if (!m_talking) return -2;
    m_channel = a & 0x0F;
    return -1;
}

int CbmImageDos::ciout(uint8_t a) {
    if (!m_listening) return -2;
    if (m_opening) {
        if (m_name.size() < 80) m_name += (char)a;
    } else if (m_channel == 15) {
        const bool binary = m_cmd.size() >= 2 && m_cmd[0] == 'U' && m_cmd[1] == '0';   // burst: run at UNLISTEN
        if (a == '\r' && !binary) { command(m_cmd); m_cmd.clear(); }
        else if (m_cmd.size() < 80) m_cmd += (char)a;
    } else if (m_ch[m_channel].writing) {
        m_ch[m_channel].data.push_back(a);      // into the file being written
    } else if (m_ch[m_channel].buffer) {
        Channel& c = m_ch[m_channel];           // into the buffer (for B-W / U2)
        c.buf[c.ptr] = a;
        c.ptr = (c.ptr + 1) & 0xFF;
    } else if (m_disk && !m_disk->writable()) {
        setError(26, "WRITE PROTECT ON");       // data written to a file
    }                                           // else not open: dropped, the open's error stays
    return 0;
}

int CbmImageDos::unlsn() {
    if (!m_listening) return -2;
    m_listening = false;
    m_active = false;
    if (m_opening) {
        m_opening = false;
        openChannel(m_channel, m_name);
    } else if (m_channel == 15 && !m_cmd.empty()) {
        command(m_cmd);
        m_cmd.clear();
    }
    return -1;
}

int CbmImageDos::acptr(uint8_t* a) {
    if (!m_talking) return -2;
    if (m_channel == 15) {
        *a = (uint8_t)m_error[m_errorPos++];
        if (m_errorPos >= m_error.size()) {
            setError(0, " OK");                 // read once, then OK
            return 0x40;
        }
        return 0;
    }
    Channel& c = m_ch[m_channel];
    if (c.buffer) {                             // EOI with the last byte, then wrap
        *a = c.buf[c.ptr];
        bool eoi = c.ptr == c.last;
        c.ptr = (c.ptr + 1) & 0xFF;
        return eoi ? 0x40 : 0;
    }
    if (!c.open || c.pos >= c.data.size()) { *a = 0; return 0x42; }
    *a = c.data[c.pos++];
    if (c.pos == c.data.size()) return 0x40;    // EOI with the last byte
    return 0;
}

int CbmImageDos::untlk() {
    if (!m_talking) return -2;
    m_talking = false;
    m_active = false;
    return -1;
}
